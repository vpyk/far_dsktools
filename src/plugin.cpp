#include "plugin.hpp"
#include "uuid.hpp"
#include "version.hpp"
#include "diskdefs_embed.h"

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

static std::vector<std::wstring> build_wide_names(const dsk_tools::Files &files)
{
    std::vector<std::wstring> names;
    names.reserve(files.size());
    for (const auto &f : files)
        names.push_back(to_wide(f.name));
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

// Re-reads the current directory from the filesystem
static dsk_tools::Result refresh_dir(PluginPanel &panel)
{
    dsk_tools::Files files;
    auto r = panel.filesystem->dir(files, false);
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

    if (!Info->Dir || wcscmp(Info->Dir, L"/") == 0)
    {
        // Navigate to root: cd_up until root, then re-read
        while (!panel.filesystem->is_root())
            panel.filesystem->cd_up();
        panel.currentDir = L"/";
        return refresh_dir(panel) ? TRUE : FALSE;
    }

    if (wcscmp(Info->Dir, L"..") == 0)
    {
        if (panel.filesystem->is_root())
            return TRUE;
        panel.filesystem->cd_up();
        // Strip last path segment
        if (!panel.currentDir.empty() && panel.currentDir.back() == L'/')
            panel.currentDir.pop_back();
        auto pos = panel.currentDir.find_last_of(L'/');
        panel.currentDir = (pos == 0) ? L"/" : panel.currentDir.substr(0, pos + 1);
        return refresh_dir(panel) ? TRUE : FALSE;
    }

    // Named directory: extract the last path component
    std::wstring dirPath = Info->Dir;
    // Remove trailing slash if present
    if (!dirPath.empty() && dirPath.back() == L'/')
        dirPath.pop_back();
    // Get last component
    auto pos = dirPath.find_last_of(L'/');
    std::wstring dirName = (pos == std::wstring::npos) ? dirPath : dirPath.substr(pos + 1);

    // Special case: navigating from root to subdir
    if (dirName.empty())
    {
        panel.currentDir = dirPath;
        return refresh_dir(panel) ? TRUE : FALSE;
    }

    // Find the directory in the current listing
    dsk_tools::UniversalFile fd;
    std::string uName = wide_to_utf8(dirName);
    auto r = panel.filesystem->find_file(uName, fd);
    if (!r || !fd.is_dir)
        return FALSE;

    panel.filesystem->cd(fd);

    // Update currentDir
    if (panel.currentDir == L"/")
        panel.currentDir = L"/" + dirName;
    else
        panel.currentDir = panel.currentDir + dirName + L"/";

    return refresh_dir(panel) ? TRUE : FALSE;
}

static std::wstring join_path(std::wstring dir, const std::wstring &name)
{
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';
    return dir + name;
}

// Extracts one file from the image to outPath
static bool extract_file(PluginPanel &panel, const dsk_tools::UniversalFile &uf,
                         const std::wstring &outPath)
{
    dsk_tools::BYTES fileData;
    if (!panel.filesystem->get_file(uf, "raw", fileData))
        return false;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, fileData.data(),
                        static_cast<DWORD>(fileData.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok && written == fileData.size();
}

// CP/M: copies a whole user area (virtual USER_nn directory) into <destDir>/USER_nn
static bool extract_cpm_area(PluginPanel &panel, unsigned user, const std::wstring &destDir)
{
    std::wstring dirPath = join_path(destDir, cpm_user_dir(user));
    if (!CreateDirectoryW(dirPath.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    bool ok = true;
    for (const auto &f : panel.cpmAll)
        if (!f.is_dir && cpm_user(f) == user)
            ok = extract_file(panel, f, join_path(dirPath, to_wide(f.name))) && ok;
    return ok;
}

extern "C" intptr_t WINAPI GetFilesW(struct GetFilesInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    intptr_t successCount = 0;

    for (size_t i = 0; i < Info->ItemsNumber; ++i)
    {
        intptr_t idx = reinterpret_cast<intptr_t>(Info->PanelItem[i].UserData.Data);
        if (idx < 0 || static_cast<size_t>(idx) >= panel.files.size())
            continue;

        const auto &uf = panel.files[idx];
        bool ok;
        if (uf.is_dir)
        {
            if (!panel.cpmUsers)
                continue;   // TODO: recursive copy of real directories
            ok = extract_cpm_area(panel, cpm_user(uf), Info->DestPath);
        }
        else
            ok = extract_file(panel, uf, join_path(Info->DestPath, Info->PanelItem[i].FileName));

        if (ok)
            ++successCount;
    }

    return successCount > 0 ? TRUE : FALSE;
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
