#include "plugin.hpp"
#include "uuid.hpp"
#include "version.hpp"
#include "diskdefs_embed.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <windows.h>

// ── Per-panel state ────────────────────────────────────────────
static std::map<intptr_t, PluginPanel> g_Panels;
static intptr_t g_NextHandle = 1;   // -1/-2 would clash with PANEL_STOP/PANEL_PASSIVE

// ── Global plugin info (received via SetStartupInfoW) ──────────
PluginStartupInfo  PSI = {};
FarStandardFunctions FSF = {};

// ═══════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════

static std::string wide_to_utf8(const std::wstring &w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string r(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, r.data(), len, nullptr, nullptr);
    return r;
}

static std::wstring to_wide(const std::string &s)
{
    if (s.empty()) return {};
    // Try UTF-8 first
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   s.c_str(), -1, nullptr, 0);
    if (len > 0)
    {
        std::wstring r(static_cast<size_t>(len) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), len);
        return r;
    }
    // CP/M filenames are ASCII + KOI-8 on Soviet systems;
    // fall back to OEM codepage (CP-866 on Russian Windows)
    len = MultiByteToWideChar(CP_OEMCP, 0, s.c_str(), -1, nullptr, 0);
    if (len > 0)
    {
        std::wstring r(static_cast<size_t>(len) - 1, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, s.c_str(), -1, r.data(), len);
        return r;
    }
    // Last resort: assume Latin-1
    std::wstring r;
    r.reserve(s.size());
    for (unsigned char c : s) r += (wchar_t)c;
    return r;
}

// Retro filesystems allow characters and names that Windows does not. The panel shows
// the same safe name that extraction writes, so F3/F4/F5 and the panel always agree.
static std::wstring sanitize_name(const std::wstring &name)
{
    std::wstring r = name;
    for (auto &c : r)
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c))
            c = L'_';
    // Windows drops trailing dots and spaces
    for (size_t i = r.size(); i > 0 && (r[i - 1] == L'.' || r[i - 1] == L' '); --i)
        r[i - 1] = L'_';
    if (r.empty())
        return L"_";

    // Reserved device names, with or without an extension
    std::wstring base = r.substr(0, r.find(L'.'));
    static const wchar_t *const reserved[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    bool isReserved = false;
    for (auto *n : reserved)
        isReserved |= _wcsicmp(base.c_str(), n) == 0;
    if (base.size() == 4 && iswdigit(base[3]) && base[3] != L'0'
        && (_wcsnicmp(base.c_str(), L"COM", 3) == 0 || _wcsnicmp(base.c_str(), L"LPT", 3) == 0))
        isReserved = true;
    if (isReserved)
        r.insert(base.size(), L"_");
    return r;
}

// Display/extraction names for a listing: sanitized and unique within it
// (Windows names are case-insensitive), a clash gets "~2", "~3"... before the extension
static std::vector<std::wstring> build_wide_names(const dsk_tools::Files &files)
{
    std::vector<std::wstring> names;
    names.reserve(files.size());
    std::set<std::wstring> used;
    auto key = [](std::wstring s) { CharUpperBuffW(&s[0], static_cast<DWORD>(s.size())); return s; };
    for (const auto &f : files)
    {
        std::wstring name = sanitize_name(to_wide(f.name));
        if (used.count(key(name)))
        {
            auto dot = f.is_dir ? std::wstring::npos : name.rfind(L'.');
            std::wstring stem = name.substr(0, dot);
            std::wstring ext  = (dot == std::wstring::npos) ? L"" : name.substr(dot);
            for (int n = 2; used.count(key(name)); ++n)
                name = stem + L"~" + std::to_wstring(n) + ext;
        }
        used.insert(key(name));
        names.push_back(name);
    }
    return names;
}

static std::wstring extract_filename(const std::wstring &path)
{
    auto pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

// PSI.Message shows each item as one line and does not break on '\n',
// so the text is split into separate items
static void show_error(const std::wstring &text)
{
    static const UUID PluginUuid = PLUGIN_UUID;

    std::vector<std::wstring> lines;
    size_t start = 0;
    for (;;)
    {
        size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? end : end - start);
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    while (lines.size() > 1 && lines.back().empty())
        lines.pop_back();

    std::vector<const wchar_t *> items;
    items.reserve(lines.size() + 1);
    items.push_back(L"DskTools");
    for (const auto &l : lines)
        items.push_back(l.c_str());
    PSI.Message(&PluginUuid, nullptr, FMSG_WARNING | FMSG_MB_OK, nullptr,
                items.data(), items.size(), 0);
}

static std::wstring describe(const wchar_t *what, const dsk_tools::Result &r)
{
    std::wstring msg = what;
    msg += L": " + to_wide(dsk_tools::decode_error(r));
    if (!r.message.empty())
        msg += L"\n" + to_wide(r.message);
    return msg;
}

// ═══════════════════════════════════════════════════════════════
//  Refresh directory listing after navigation
// ═══════════════════════════════════════════════════════════════

// ── CP/M user areas ────────────────────────────────────────────
// dsk_tools lists all user areas flat and keeps the ST byte in attributes >> 8.
// The plugin shows user 0 at the root and every other used area as a USER_nn directory.

static const unsigned CPM_MAX_USER = 31;

static unsigned cpm_user(const dsk_tools::UniversalFile &f)
{
    return (f.attributes >> 8) & 0xFF;
}

static std::wstring cpm_user_dir(unsigned user)
{
    wchar_t buf[16];
    FSF.snprintf(buf, ARRAYSIZE(buf), L"USER_%02u", user);
    return buf;
}

// USER_nn → nn (1..31), otherwise -1
static int parse_cpm_user_dir(const std::wstring &name)
{
    if (name.size() != 7 || name.compare(0, 5, L"USER_") != 0
        || !iswdigit(name[5]) || !iswdigit(name[6]))
        return -1;
    int user = (name[5] - L'0') * 10 + (name[6] - L'0');
    return (user >= 1 && user <= static_cast<int>(CPM_MAX_USER)) ? user : -1;
}

// Builds panel.files for panel.cpmUser from panel.cpmAll
static void build_cpm_view(PluginPanel &panel)
{
    std::set<unsigned> areas;
    dsk_tools::Files view;
    for (const auto &f : panel.cpmAll)
    {
        unsigned user = cpm_user(f);
        if (user > CPM_MAX_USER)
            continue;   // CP/M 3 directory label / date stamps, not files
        if (user == panel.cpmUser)
            view.push_back(f);
        else if (panel.cpmUser == 0)
            areas.insert(user);
    }

    panel.files.clear();
    for (unsigned user : areas)
    {
        dsk_tools::UniversalFile d{};
        d.fs         = dsk_tools::FS::CPM;
        d.name       = wide_to_utf8(cpm_user_dir(user));
        d.is_dir     = true;
        d.attributes = user << 8;
        panel.files.push_back(d);
    }
    panel.files.insert(panel.files.end(), view.begin(), view.end());
    panel.wideNames = build_wide_names(panel.files);
    panel.currentDir = panel.cpmUser ? L"/" + cpm_user_dir(panel.cpmUser) : L"/";
}

// dir() of the current directory without the ".." entry some filesystems add
// (DOS 3.3, FAT, ProDOS, Onix, Sprite OS): FAR shows its own with OPIF_ADDDOTS
static dsk_tools::Result list_dir(dsk_tools::fileSystem &fs, dsk_tools::Files &files)
{
    auto r = fs.dir(files, false);
    if (r)
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const dsk_tools::UniversalFile &f) { return f.name == ".."; }),
                    files.end());
    return r;
}

// Re-reads the current directory from the filesystem
static dsk_tools::Result refresh_dir(PluginPanel &panel)
{
    dsk_tools::Files files;
    auto r = list_dir(*panel.filesystem, files);
    if (!r)
    {
        // On error, keep old listing but update wideNames anyway
        panel.wideNames.clear();
        return r;
    }
    if (panel.cpmUsers)
    {
        panel.cpmAll = std::move(files);
        build_cpm_view(panel);
    }
    else
    {
        panel.files = std::move(files);
        panel.wideNames = build_wide_names(panel.files);
    }
    return r;
}

static void update_current_dir(PluginPanel &panel)
{
    panel.currentDir = L"/";
    for (size_t i = 0; i < panel.dirStack.size(); ++i)
        panel.currentDir += (i ? L"/" : L"") + panel.dirStack[i];
}

// SetDirectoryW for hierarchical filesystems. Dir is "..", a name from the current
// listing, or a path ("/A/B", "A/B"); names are matched against the displayed
// (sanitized) names, so a directory is entered through its UniversalFile, not by name.
static bool set_dir(PluginPanel &panel, const wchar_t *dir)
{
    std::wstring path = dir ? dir : L"";
    bool ok = true;

    if (path.empty() || path[0] == L'/' || path[0] == L'\\')
    {
        while (!panel.filesystem->is_root())
            panel.filesystem->cd_up();
        panel.dirStack.clear();
    }

    size_t start = 0;
    while (ok && start <= path.size())
    {
        size_t end = path.find_first_of(L"/\\", start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring name = path.substr(start, end - start);
        start = end + 1;

        if (name.empty() || name == L".")
            continue;
        if (name == L"..")
        {
            if (!panel.dirStack.empty())
            {
                panel.filesystem->cd_up();
                panel.dirStack.pop_back();
            }
            continue;
        }

        // The listing of the directory we are in now
        if (!refresh_dir(panel))
            return false;
        auto found = std::find(panel.wideNames.begin(), panel.wideNames.end(), name);
        if (found == panel.wideNames.end()
            || !panel.files[found - panel.wideNames.begin()].is_dir)
        {
            ok = false;
            break;
        }
        panel.filesystem->cd(panel.files[found - panel.wideNames.begin()]);
        panel.dirStack.push_back(name);
    }

    update_current_dir(panel);
    return refresh_dir(panel) && ok;
}

// SetDirectoryW for CP/M: Dir is "..", "/", "USER_nn" or "/USER_nn"
static bool set_cpm_dir(PluginPanel &panel, const wchar_t *dir)
{
    std::wstring path = dir ? dir : L"";
    unsigned user;
    if (path == L"..")
        user = 0;
    else
    {
        while (!path.empty() && (path.back() == L'/' || path.back() == L'\\'))
            path.pop_back();
        auto pos = path.find_last_of(L"/\\");
        std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
        if (name.empty())
            user = 0;
        else
        {
            int parsed = parse_cpm_user_dir(name);
            if (parsed < 0)
                return false;
            user = static_cast<unsigned>(parsed);
        }
    }
    panel.cpmUser = user;
    build_cpm_view(panel);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Opening a disk image
// ═══════════════════════════════════════════════════════════════

struct DetectedImage
{
    std::wstring path;
    std::string  formatId;
    std::string  typeId;
    std::string  fsId;
};

// AnalyseW results, owned until OpenW(OPEN_ANALYSE) or CloseAnalyseW
static std::map<intptr_t, DetectedImage> g_Analysed;
static intptr_t g_NextAnalyseHandle = 1;

static bool detect_image(const std::wstring &path, DetectedImage &out, std::wstring *error)
{
    out.path = path;
    auto r = dsk_tools::detect_fdd_type(wide_to_utf8(path), out.formatId, out.typeId, out.fsId, false);
    if (!r)
    {
        if (error) *error = describe(L"Format detection failed", r);
        return false;
    }
    return true;
}

// Loads the image and mounts its filesystem. Returns nullptr on failure;
// the error is shown to the user unless `silent`.
static HANDLE open_image(const DetectedImage &di, bool silent)
{
    static const dsk_tools::DiskDefs diskdefs =
        dsk_tools::parse_diskdefs(dsk_tools::embedded_diskdefs());

    auto fail = [&](const std::wstring &msg) -> HANDLE
    {
        if (!silent)
            show_error(extract_filename(di.path) + L"\n" + msg);
        return nullptr;
    };

    std::string uPath = wide_to_utf8(di.path);

    auto img = dsk_tools::prepare_image(uPath, di.formatId, di.typeId, diskdefs);
    if (!img)
        return fail(L"Unsupported image type: " + to_wide(di.typeId));

    auto r = img->load();
    if (!r)
        return fail(describe(L"Cannot load image", r));

    auto fs = dsk_tools::prepare_filesystem(img.get(), di.fsId, diskdefs);
    if (!fs)
        return fail(L"Unsupported filesystem: " + to_wide(di.fsId));

    r = fs->open();
    if (!r)
        return fail(describe(L"Cannot open filesystem", r));

    PluginPanel panel;
    panel.currentDir = L"/";
    panel.hostPath   = di.path;
    panel.image      = std::move(img);
    panel.filesystem = std::move(fs);
    panel.cpmUsers   = panel.filesystem->get_fs() == dsk_tools::FS::CPM;

    r = refresh_dir(panel);
    if (!r)
        return fail(describe(L"Cannot read directory", r));

    intptr_t h = g_NextHandle++;
    g_Panels[h] = std::move(panel);
    return reinterpret_cast<HANDLE>(h);
}

// Full path of the file under cursor in the active panel, or empty
static std::wstring current_panel_file()
{
    PanelInfo pi = {};
    pi.StructSize = sizeof(PanelInfo);
    if (!PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi))
        return {};
    if ((pi.Flags & PFLAGS_PLUGIN) && !(pi.Flags & PFLAGS_REALNAMES))
        return {};

    size_t size = static_cast<size_t>(
        PSI.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, nullptr));
    if (!size) return {};
    std::vector<char> itemBuf(size);
    FarGetPluginPanelItem gpi = {};
    gpi.StructSize = sizeof(FarGetPluginPanelItem);
    gpi.Size       = size;
    gpi.Item       = reinterpret_cast<PluginPanelItem *>(itemBuf.data());
    if (!PSI.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &gpi))
        return {};
    if (!gpi.Item->FileName || (gpi.Item->FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return {};
    std::wstring name = gpi.Item->FileName;

    // FileName may already be a full path (e.g. temporary panel)
    if (name.find_first_of(L"\\:") != std::wstring::npos)
        return name;

    size = static_cast<size_t>(
        PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr));
    if (!size) return {};
    std::vector<char> dirBuf(size);
    auto *dir = reinterpret_cast<FarPanelDirectory *>(dirBuf.data());
    dir->StructSize = sizeof(FarPanelDirectory);
    if (!PSI.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, size, dir) || !dir->Name)
        return {};

    std::wstring path = dir->Name;
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    return path + name;
}

// ═══════════════════════════════════════════════════════════════
//  Global plugin exports
// ═══════════════════════════════════════════════════════════════

extern "C" void WINAPI GetGlobalInfoW(struct GlobalInfo *Info)
{
    Info->StructSize    = sizeof(GlobalInfo);
    Info->MinFarVersion = FARMANAGERVERSION;
    Info->Version       = MAKEFARVERSION(PLUGIN_VERSION_MAJOR,
                                          PLUGIN_VERSION_MINOR,
                                          PLUGIN_VERSION_BUILD,
                                          0, VS_RELEASE);
    static const UUID PluginUuid = PLUGIN_UUID;
    Info->Guid          = PluginUuid;
    Info->Title         = L"DskTools";
    Info->Description   = L"Retro disk image browser (CP/M, DOS 3.3, Agat, FAT, ProDOS, RT-11)";
    Info->Author        = L"Author";
    Info->Instance      = nullptr;
}

extern "C" void WINAPI SetStartupInfoW(const struct PluginStartupInfo *Info)
{
    PSI = *Info;
    FSF = *Info->FSF;
}

extern "C" void WINAPI GetPluginInfoW(struct PluginInfo *Info)
{
    Info->StructSize = sizeof(PluginInfo);
    Info->Flags      = PF_NONE;

    static const wchar_t *DiskMenuStrings = nullptr;

    static const wchar_t *PluginMenuStrings[] = { L"DskTools" };
    static const UUID PluginMenuGuids[] = { PLUGIN_UUID };
    static PluginMenuItem PluginMenu = { PluginMenuGuids, PluginMenuStrings, 1 };

    static const wchar_t *PluginCfgStrings[] = { L"DskTools" };
    static const UUID PluginCfgGuids[] = { PLUGIN_UUID };
    static PluginMenuItem PluginCfg = { PluginCfgGuids, PluginCfgStrings, 1 };

    Info->DiskMenu     = { nullptr, &DiskMenuStrings, 0 };
    Info->PluginMenu   = PluginMenu;
    Info->PluginConfig = PluginCfg;
    Info->CommandPrefix = L"dsk";
    Info->Instance      = nullptr;
}

// ═══════════════════════════════════════════════════════════════
//  Panel operations
// ═══════════════════════════════════════════════════════════════

// Called by FAR on Enter / Ctrl+PgDn over a file: claim it if it is a disk image
extern "C" HANDLE WINAPI AnalyseW(const struct AnalyseInfo *Info)
{
    if (!Info->FileName || !Info->FileName[0])
        return nullptr;

    DetectedImage di;
    if (!detect_image(Info->FileName, di, nullptr))
        return nullptr;

    intptr_t h = g_NextAnalyseHandle++;
    g_Analysed[h] = std::move(di);
    return reinterpret_cast<HANDLE>(h);
}

extern "C" void WINAPI CloseAnalyseW(const struct CloseAnalyseInfo *Info)
{
    g_Analysed.erase(reinterpret_cast<intptr_t>(Info->Handle));
}

extern "C" HANDLE WINAPI OpenW(const struct OpenInfo *Info)
{
    if (Info->OpenFrom == OPEN_ANALYSE)
    {
        auto *oai = reinterpret_cast<const OpenAnalyseInfo *>(Info->Data);
        if (!oai) return nullptr;
        auto it = g_Analysed.find(reinterpret_cast<intptr_t>(oai->Handle));
        if (it == g_Analysed.end()) return nullptr;
        DetectedImage di = std::move(it->second);
        g_Analysed.erase(it);
        bool silent = oai->Info && (oai->Info->OpMode & (OPM_SILENT | OPM_FIND));
        return open_image(di, silent);
    }

    std::wstring wPath;
    if (Info->OpenFrom == OPEN_COMMANDLINE)
    {
        // dsk:<path>, relative to the current panel directory
        auto *cmd = reinterpret_cast<const OpenCommandLineInfo *>(Info->Data);
        if (cmd && cmd->CommandLine)
            wPath = cmd->CommandLine;
        auto first = wPath.find_first_not_of(L" \"");
        auto last  = wPath.find_last_not_of(L" \"");
        wPath = (first == std::wstring::npos) ? L"" : wPath.substr(first, last - first + 1);
        if (!wPath.empty())
        {
            size_t len = FSF.ConvertPath(CPM_FULL, wPath.c_str(), nullptr, 0);
            if (len)
            {
                std::vector<wchar_t> full(len);
                FSF.ConvertPath(CPM_FULL, wPath.c_str(), full.data(), len);
                wPath = full.data();
            }
        }
    }
    else if (Info->OpenFrom == OPEN_PLUGINSMENU)
    {
        // F11 menu: open the file under cursor
        wPath = current_panel_file();
    }
    else
        return nullptr;

    if (wPath.empty())
    {
        show_error(L"No disk image file specified");
        return nullptr;
    }

    DetectedImage di;
    std::wstring error;
    if (!detect_image(wPath, di, &error))
    {
        show_error(extract_filename(wPath) + L"\n" + error);
        return nullptr;
    }
    return open_image(di, false);
}

extern "C" void WINAPI GetOpenPanelInfoW(struct OpenPanelInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return;

    static std::wstring s_title;
    s_title = L" " + extract_filename(it->second.hostPath);

    Info->StructSize       = sizeof(OpenPanelInfo);
    Info->Flags            = OPIF_SHOWPRESERVECASE | OPIF_ADDDOTS;
    Info->HostFile         = it->second.hostPath.c_str();
    // Empty CurDir at the root lets FAR close the panel on ".." and return to the host file
    Info->CurDir           = it->second.currentDir == L"/" ? L"" : it->second.currentDir.c_str();
    Info->Format           = s_title.c_str();
    Info->PanelTitle       = s_title.c_str();
    Info->InfoLines        = nullptr;
    Info->InfoLinesNumber  = 0;
    Info->DescrFiles       = nullptr;
    Info->DescrFilesNumber = 0;
    Info->PanelModesArray  = nullptr;
    Info->PanelModesNumber = 0;
    Info->StartPanelMode   = '4';
    Info->StartSortMode    = SM_UNSORTED;
    Info->StartSortOrder   = 0;
    Info->KeyBar           = nullptr;
    Info->ShortcutData     = nullptr;
    Info->FreeSize         = 0;
    Info->UserData         = { nullptr, nullptr };
    Info->Instance         = nullptr;
}

extern "C" intptr_t WINAPI GetFindDataW(struct GetFindDataInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    size_t count = panel.files.size();
    if (count == 0)
    {
        Info->PanelItem   = nullptr;
        Info->ItemsNumber = 0;
        return TRUE;
    }

    auto *items = new PluginPanelItem[count];
    memset(items, 0, sizeof(PluginPanelItem) * count);

    for (size_t i = 0; i < count; ++i)
    {
        const auto &uf = panel.files[i];

        items[i].FileName          = panel.wideNames[i].c_str();
        items[i].AlternateFileName = nullptr;
        items[i].Description       = nullptr;  // TODO: show type_label if available
        items[i].Owner             = nullptr;
        items[i].CustomColumnData  = nullptr;
        items[i].CustomColumnNumber = 0;
        items[i].Flags             = PPIF_NONE;
        items[i].UserData          = { reinterpret_cast<void*>(static_cast<intptr_t>(i)), nullptr };
        items[i].FileSize          = uf.size;
        items[i].AllocationSize    = uf.size;
        items[i].FileAttributes    = uf.is_dir    ? FILE_ATTRIBUTE_DIRECTORY
                                   : uf.is_protected ? (FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_READONLY)
                                   : FILE_ATTRIBUTE_NORMAL;
        items[i].NumberOfLinks     = 0;
        items[i].CRC32             = 0;
    }

    Info->PanelItem   = items;
    Info->ItemsNumber = count;
    return TRUE;
}

extern "C" void WINAPI FreeFindDataW(const struct FreeFindDataInfo *Info)
{
    delete[] Info->PanelItem;
}

extern "C" intptr_t WINAPI SetDirectoryW(const struct SetDirectoryInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;

    if (panel.cpmUsers)
        return set_cpm_dir(panel, Info->Dir) ? TRUE : FALSE;

    return set_dir(panel, Info->Dir) ? TRUE : FALSE;
}

static std::wstring join_path(std::wstring dir, const std::wstring &name)
{
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';
    return dir + name;
}

static std::wstring last_error_text(DWORD code)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
    std::wstring text = buf ? buf : L"";
    LocalFree(buf);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
        text.pop_back();
    return text;
}

// Shows a warning with buttons; returns the button index, -1 on Esc
static intptr_t ask(const std::vector<std::wstring> &lines, const std::vector<const wchar_t *> &buttons)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    std::vector<const wchar_t *> items;
    items.push_back(L"DskTools");
    for (const auto &l : lines)
        items.push_back(l.c_str());
    items.insert(items.end(), buttons.begin(), buttons.end());
    return PSI.Message(&PluginUuid, nullptr, FMSG_WARNING, nullptr,
                       items.data(), items.size(), static_cast<intptr_t>(buttons.size()));
}

// ── Extraction ─────────────────────────────────────────────────

enum class Outcome { Ok, Skipped, Cancelled };

struct ExtractContext
{
    PluginPanel &panel;
    bool ask;                   // interactive copy: confirm overwrite
    bool report;                // show errors
    bool overwriteAll = false;
    bool skipExisting = false;
    bool skipErrors   = false;
};

// Error while extracting `what`: Skip / Skip all / Cancel
static Outcome extract_error(ExtractContext &ctx, const std::wstring &what, const std::wstring &reason)
{
    if (!ctx.report || ctx.skipErrors)
        return Outcome::Skipped;
    switch (ask({ L"Cannot extract", what, reason }, { L"&Skip", L"Skip &all", L"&Cancel" }))
    {
    case 0:  return Outcome::Skipped;
    case 1:  ctx.skipErrors = true; return Outcome::Skipped;
    default: return Outcome::Cancelled;
    }
}

// Extracts one file from the image to outPath
static Outcome extract_file(ExtractContext &ctx, const dsk_tools::UniversalFile &uf,
                            const std::wstring &outPath)
{
    DWORD attrs = GetFileAttributesW(outPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && ctx.ask && !ctx.overwriteAll)
    {
        if (ctx.skipExisting)
            return Outcome::Skipped;
        switch (ask({ L"File already exists", outPath },
                    { L"&Overwrite", L"&All", L"&Skip", L"S&kip all", L"&Cancel" }))
        {
        case 0:  break;
        case 1:  ctx.overwriteAll = true; break;
        case 2:  return Outcome::Skipped;
        case 3:  ctx.skipExisting = true; return Outcome::Skipped;
        default: return Outcome::Cancelled;
        }
    }

    dsk_tools::BYTES fileData;
    auto r = ctx.panel.filesystem->get_file(uf, "raw", fileData);
    if (!r)
        return extract_error(ctx, outPath, describe(L"Cannot read file from image", r));

    // A read-only target (e.g. extracted before from an R/O file) would fail CREATE_ALWAYS
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(outPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return extract_error(ctx, outPath, last_error_text(GetLastError()));

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, fileData.data(),
                        static_cast<DWORD>(fileData.size()), &written, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hFile);
    if (!ok || written != fileData.size())
    {
        DeleteFileW(outPath.c_str());
        return extract_error(ctx, outPath, ok ? std::wstring(L"Write incomplete") : last_error_text(err));
    }
    return Outcome::Ok;
}

static Outcome make_dir(ExtractContext &ctx, const std::wstring &dirPath)
{
    if (CreateDirectoryW(dirPath.c_str(), nullptr))
        return Outcome::Ok;
    DWORD err = GetLastError();
    DWORD attrs = GetFileAttributesW(dirPath.c_str());
    if (err == ERROR_ALREADY_EXISTS && attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return Outcome::Ok;
    return extract_error(ctx, dirPath, last_error_text(err));
}

// Extracts a listing (already named by build_wide_names) into destDir; worst outcome wins
static Outcome extract_list(ExtractContext &ctx, const dsk_tools::Files &files,
                            const std::vector<std::wstring> &names, const std::wstring &destDir);

// Recursively extracts a directory of a hierarchical filesystem into destDir\name
static Outcome extract_dir(ExtractContext &ctx, const dsk_tools::UniversalFile &dir,
                           const std::wstring &name, const std::wstring &destDir)
{
    std::wstring dirPath = join_path(destDir, name);
    Outcome o = make_dir(ctx, dirPath);
    if (o != Outcome::Ok)
        return o;

    auto &fs = *ctx.panel.filesystem;
    fs.cd(dir);
    dsk_tools::Files files;
    auto r = list_dir(fs, files);
    if (r)
        o = extract_list(ctx, files, build_wide_names(files), dirPath);
    else
        o = extract_error(ctx, dirPath, describe(L"Cannot read directory", r));
    fs.cd_up();
    return o;
}

// CP/M: copies a whole user area (virtual USER_nn directory) into destDir\USER_nn
static Outcome extract_cpm_area(ExtractContext &ctx, unsigned user, const std::wstring &destDir)
{
    std::wstring dirPath = join_path(destDir, cpm_user_dir(user));
    Outcome o = make_dir(ctx, dirPath);
    if (o != Outcome::Ok)
        return o;

    dsk_tools::Files area;
    for (const auto &f : ctx.panel.cpmAll)
        if (!f.is_dir && cpm_user(f) == user)
            area.push_back(f);
    return extract_list(ctx, area, build_wide_names(area), dirPath);
}

// One panel item: a file, a directory or a CP/M user area
static Outcome extract_item(ExtractContext &ctx, const dsk_tools::UniversalFile &uf,
                            const std::wstring &name, const std::wstring &destDir)
{
    if (!uf.is_dir)
        return extract_file(ctx, uf, join_path(destDir, name));
    if (ctx.panel.cpmUsers)
        return extract_cpm_area(ctx, cpm_user(uf), destDir);
    return extract_dir(ctx, uf, name, destDir);
}

static Outcome extract_list(ExtractContext &ctx, const dsk_tools::Files &files,
                            const std::vector<std::wstring> &names, const std::wstring &destDir)
{
    Outcome result = Outcome::Ok;
    for (size_t i = 0; i < files.size(); ++i)
    {
        Outcome o = extract_item(ctx, files[i], names[i], destDir);
        if (o == Outcome::Cancelled)
            return o;
        if (o == Outcome::Skipped)
            result = Outcome::Skipped;
    }
    return result;
}

extern "C" intptr_t WINAPI GetFilesW(struct GetFilesInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    const bool silent = (Info->OpMode & (OPM_SILENT | OPM_FIND | OPM_QUICKVIEW)) != 0;

    if (Info->Move)
    {
        // The image is read-only for now: refuse rather than copy and keep the originals
        if (!silent)
            show_error(L"Moving files out of a disk image is not supported:\n"
                       L"the image is read-only. Use F5 to copy.");
        return FALSE;
    }

    // F3/F4/Ctrl+Q extract into a temporary folder: no overwrite questions there
    ExtractContext ctx{ panel,
                        (Info->OpMode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) == 0,
                        !silent };

    bool anyOk = false;
    for (size_t i = 0; i < Info->ItemsNumber; ++i)
    {
        auto &item = Info->PanelItem[i];
        intptr_t idx = reinterpret_cast<intptr_t>(item.UserData.Data);
        if (idx < 0 || static_cast<size_t>(idx) >= panel.files.size())
        {
            item.Flags &= ~PPIF_SELECTED;
            continue;
        }

        Outcome o = extract_item(ctx, panel.files[idx], panel.wideNames[idx], Info->DestPath);
        if (o == Outcome::Cancelled)
            return -1;
        if (o == Outcome::Ok)
            anyOk = true;
        else
            item.Flags &= ~PPIF_SELECTED;   // FAR keeps items it was not done with selected
    }

    return anyOk ? TRUE : FALSE;
}

extern "C" intptr_t WINAPI DeleteFilesW(const struct DeleteFilesInfo * /*Info*/)
{
    return FALSE;
}

extern "C" intptr_t WINAPI MakeDirectoryW(struct MakeDirectoryInfo * /*Info*/)
{
    return FALSE;
}

extern "C" intptr_t WINAPI PutFilesW(const struct PutFilesInfo * /*Info*/)
{
    return FALSE;
}

extern "C" intptr_t WINAPI ProcessHostFileW(const struct ProcessHostFileInfo * /*Info*/)
{
    return FALSE;
}

extern "C" intptr_t WINAPI ProcessPanelInputW(const struct ProcessPanelInputInfo * /*Info*/)
{
    return FALSE;
}

extern "C" void WINAPI ClosePanelW(const struct ClosePanelInfo *Info)
{
    g_Panels.erase(reinterpret_cast<intptr_t>(Info->hPanel));
}

// ── Optional exports ───────────────────────────────────────────

extern "C" intptr_t WINAPI ProcessPanelEventW(const struct ProcessPanelEventInfo * /*Info*/)
{
    return FALSE;
}

extern "C" void WINAPI ExitFARW(const struct ExitInfo * /*Info*/)
{
    g_Panels.clear();
}
