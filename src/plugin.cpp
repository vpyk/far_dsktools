#include "plugin.hpp"
#include "lang.hpp"
#include "uuid.hpp"
#include "version.hpp"
#include "diskdefs_embed.h"

#include <DlgBuilder.hpp>

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

// ── Messages ───────────────────────────────────────────────────
// Texts come from DskToolsEng.lng / DskToolsRus.lng next to the DLL, by FAR's interface language

static const wchar_t *msg(LngId id)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    return PSI.GetMsg(&PluginUuid, id);
}

// A message with %1, %2... replaced by args
static std::wstring msg(LngId id, std::initializer_list<std::wstring> args)
{
    std::wstring r;
    for (const wchar_t *c = msg(id); *c; ++c)
    {
        if (c[0] == L'%' && c[1] >= L'1' && c[1] <= L'9' && static_cast<size_t>(c[1] - L'1') < args.size())
            r += args.begin()[*++c - L'1'];
        else
            r += *c;
    }
    return r;
}

// "N items" in the right Russian plural form (1/21, 2..4/22..24, 5..20); n >= 2 here,
// a single item is always named
static std::wstring items_text(size_t n)
{
    LngId id = (n % 10 == 1 && n % 100 != 11) ? MItemsOne
             : (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) ? MItemsFew
             : MItemsMany;
    return msg(id, { std::to_wstring(n) });
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

// ── File dates ─────────────────────────────────────────────────
// dsk_tools gives a date as the retro system kept it (FAT, ProDOS, RT-11, Sprite OS):
// local time, no time zone. FAR takes UTC and shows it in local time, so the date goes
// through the local time zone and comes back on screen unchanged. No date (or no
// time) gives a zero FILETIME (or midnight); FAR leaves a zero date column empty.
static FILETIME to_filetime(const dsk_tools::FileDate &d)
{
    FILETIME ft = {};
    if (!d.valid())
        return ft;
    SYSTEMTIME local = {};
    local.wYear   = d.year;
    local.wMonth  = d.month;
    local.wDay    = d.day;
    local.wHour   = static_cast<WORD>(d.hour   >= 0 ? d.hour   : 0);
    local.wMinute = static_cast<WORD>(d.minute >= 0 ? d.minute : 0);
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft))
        return {};  // e.g. 31 February
    return ft;
}

static bool has_time(const FILETIME &ft)
{
    return ft.dwLowDateTime || ft.dwHighDateTime;
}

// The other way: the last write time of a host file or folder as local time, for
// put_file/mkdir (FAT and RT-11 keep it, others ignore it); no date if unreadable
static dsk_tools::FileDate host_date(const std::wstring &path)
{
    dsk_tools::FileDate d;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    SYSTEMTIME utc, local;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)
        || !FileTimeToSystemTime(&fad.ftLastWriteTime, &utc)
        || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return d;
    d.year   = local.wYear;
    d.month  = static_cast<uint8_t>(local.wMonth);
    d.day    = static_cast<uint8_t>(local.wDay);
    d.hour   = static_cast<int8_t>(local.wHour);
    d.minute = static_cast<int8_t>(local.wMinute);
    return d;
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
    // Areas made by F7 are shown until the panel is closed, even while still empty
    std::set<unsigned> areas = panel.cpmUser == 0 ? panel.cpmNewAreas : std::set<unsigned>{};
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
        if (error) *error = describe(msg(MFormatDetectionFailed), r);
        return false;
    }
    return true;
}

// Short filesystem name for the panel title
static std::wstring fs_name(dsk_tools::FS fs)
{
    switch (fs)
    {
    case dsk_tools::FS::DOS33:    return L"DOS3.3";
    case dsk_tools::FS::Sprite:   return L"SpriteOS";
    case dsk_tools::FS::CPM:      return L"CP/M";
    case dsk_tools::FS::FAT:      return L"FAT";
    case dsk_tools::FS::Iskra226: return L"Iskra-226";
    case dsk_tools::FS::ProDOS:   return L"ProDOS";
    case dsk_tools::FS::Onix:     return L"Onix";
    case dsk_tools::FS::RT11:     return L"RT-11";
    default:                      return L"?";
    }
}

static const dsk_tools::DiskDefs &diskdefs()
{
    static const dsk_tools::DiskDefs defs =
        dsk_tools::parse_diskdefs(dsk_tools::embedded_diskdefs());
    return defs;
}

// Loads panel.hostPath as panel.formatId/typeId/fsId and mounts its filesystem.
// Returns an error text, empty on success. Used to open an image and to reload it.
static std::wstring mount_image(PluginPanel &panel)
{
    std::string uPath = wide_to_utf8(panel.hostPath);

    auto img = dsk_tools::prepare_image(uPath, panel.formatId, panel.typeId, diskdefs());
    if (!img)
        return msg(MUnsupportedImageType, { to_wide(panel.typeId) });

    auto r = img->load();
    if (!r)
        return describe(msg(MCannotLoadImage), r);

    auto fs = dsk_tools::prepare_filesystem(img.get(), panel.fsId, diskdefs());
    if (!fs)
        return msg(MUnsupportedFilesystem, { to_wide(panel.fsId) });

    r = fs->open();
    if (!r)
        return describe(msg(MCannotOpenFilesystem), r);

    // The filesystem points into the image: drop the old one first
    panel.filesystem.reset();
    panel.image      = std::move(img);
    panel.filesystem = std::move(fs);
    panel.dirStack.clear();
    panel.cpmUser    = 0;
    return L"";
}

// Why the image cannot be modified, empty if it can. Only raw images are saved for now:
// they are written back byte for byte, while HFE/MFM writers re-encode every track.
static std::wstring read_only_reason(PluginPanel &panel)
{
    using dsk_tools::FSCaps;
    if (!static_cast<bool>(panel.filesystem->get_caps() & FSCaps::Add))
        return msg(MFsReadOnly, { fs_name(panel.filesystem->get_fs()) });
    if (panel.formatId != "FILE_RAW_MSB")
        return msg(MFormatNotSaved, { to_wide(panel.formatId) });

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(panel.hostPath.c_str(), GetFileExInfoStandard, &fad))
        return msg(MCannotReadAttributes);
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        return msg(MImageReadOnly);
    unsigned long long size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size != panel.image->get_buffer()->size())
        return msg(MImageHasHeader);
    return L"";
}

// Loads the image and mounts its filesystem. Returns nullptr on failure;
// the error is shown to the user unless `silent`.
static HANDLE open_image(const DetectedImage &di, bool silent)
{
    auto fail = [&](const std::wstring &msg) -> HANDLE
    {
        if (!silent)
            show_error(extract_filename(di.path) + L"\n" + msg);
        return nullptr;
    };

    PluginPanel panel;
    panel.currentDir = L"/";
    panel.hostPath   = di.path;
    panel.formatId   = di.formatId;
    panel.typeId     = di.typeId;
    panel.fsId       = di.fsId;

    std::wstring error = mount_image(panel);
    if (!error.empty())
        return fail(error);

    panel.cpmUsers   = panel.filesystem->get_fs() == dsk_tools::FS::CPM;
    panel.title      = L" DskTools:" + fs_name(panel.filesystem->get_fs())
                     + L":" + extract_filename(di.path);

    auto r = refresh_dir(panel);
    if (!r)
        return fail(describe(msg(MCannotReadDirectory), r));

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
        show_error(msg(MNoImageSpecified));
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

    const std::wstring &title = it->second.title;

    Info->StructSize       = sizeof(OpenPanelInfo);
    Info->Flags            = OPIF_SHOWPRESERVECASE | OPIF_ADDDOTS;
    Info->HostFile         = it->second.hostPath.c_str();
    // Empty CurDir at the root lets FAR close the panel on ".." and return to the host file
    Info->CurDir           = it->second.currentDir == L"/" ? L"" : it->second.currentDir.c_str();
    Info->Format           = title.c_str();
    Info->PanelTitle       = title.c_str();
    Info->InfoLines        = nullptr;
    Info->InfoLinesNumber  = 0;
    Info->DescrFiles       = nullptr;
    Info->DescrFilesNumber = 0;
    Info->PanelModesArray  = nullptr;
    Info->PanelModesNumber = 0;
    // Full (name, size, date, time) where the filesystem keeps dates, Wide (name, size) otherwise
    bool dates = static_cast<bool>(it->second.filesystem->get_caps() & dsk_tools::FSCaps::Date);
    Info->StartPanelMode   = dates ? '3' : '4';
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

        // A copy owned by the item: FAR keeps this list while it enters subdirectories
        // (Alt+F7 search), and SetDirectoryW rebuilds panel.wideNames
        const auto &name = panel.wideNames[i];
        auto *fileName = new wchar_t[name.size() + 1];
        wmemcpy(fileName, name.c_str(), name.size() + 1);
        items[i].FileName          = fileName;
        items[i].AlternateFileName = nullptr;
        items[i].Description       = nullptr;  // TODO: show type_label if available
        items[i].Owner             = nullptr;
        items[i].CustomColumnData  = nullptr;
        items[i].CustomColumnNumber = 0;
        items[i].Flags             = PPIF_NONE;
        items[i].UserData          = { reinterpret_cast<void*>(static_cast<intptr_t>(i)), nullptr };
        items[i].LastWriteTime     = to_filetime(uf.date);
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
    for (size_t i = 0; i < Info->ItemsNumber; ++i)
        delete[] Info->PanelItem[i].FileName;
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

// ── File operations: shared state and prompts ─────────────────

enum class Outcome { Ok, Skipped, Cancelled };

struct OpContext
{
    PluginPanel &panel;
    bool ask;                   // interactive operation: confirm overwrite
    bool report;                // show errors
    const wchar_t *action;      // error title: "Cannot extract", "Cannot delete"...
    bool overwriteAll = false;
    bool skipExisting = false;
    bool skipErrors   = false;
    bool changed      = false;  // the image was modified and has to be saved
    bool broken       = false;  // a write failed halfway: the in-memory image is not trusted
};

// Error on `what`: Skip / Skip all / Cancel
static Outcome op_error(OpContext &ctx, const std::wstring &what, const std::wstring &reason)
{
    if (!ctx.report || ctx.skipErrors)
        return Outcome::Skipped;
    switch (ask({ ctx.action, what, reason }, { msg(MSkip), msg(MSkipAll), msg(MCancel) }))
    {
    case 0:  return Outcome::Skipped;
    case 1:  ctx.skipErrors = true; return Outcome::Skipped;
    default: return Outcome::Cancelled;
    }
}

// Errors dsk_tools reports before it touches the image; anything else may come after a
// part of the change is done (a directory entry made, sectors taken)
static bool fails_before_change(const dsk_tools::Result &r)
{
    using dsk_tools::ErrorCode;
    return r.code == ErrorCode::InvalidName || r.code == ErrorCode::FileAlreadyExists
        || r.code == ErrorCode::FileAddErrorSpace || r.code == ErrorCode::DirNotEmpty
        || r.code == ErrorCode::NotImplementedYet;
}

// A write failed halfway: stop the whole operation, the caller discards the changes
static Outcome op_broken(OpContext &ctx, const std::wstring &what, const std::wstring &reason)
{
    ctx.broken = true;
    if (ctx.report)
        show_error(std::wstring(ctx.action) + L"\n" + what + L"\n" + reason
                   + L"\n" + msg(MOperationStopped));
    return Outcome::Cancelled;
}

// `what` already exists: Overwrite / All / Skip / Skip all / Cancel. Ok means overwrite.
static Outcome confirm_overwrite(OpContext &ctx, const std::wstring &what)
{
    if (!ctx.ask || ctx.overwriteAll)
        return Outcome::Ok;
    if (ctx.skipExisting)
        return Outcome::Skipped;
    switch (ask({ msg(MFileExists), what },
                { msg(MOverwrite), msg(MAll), msg(MSkip), msg(MSkipAllExisting), msg(MCancel) }))
    {
    case 0:  return Outcome::Ok;
    case 1:  ctx.overwriteAll = true; return Outcome::Ok;
    case 2:  return Outcome::Skipped;
    case 3:  ctx.skipExisting = true; return Outcome::Skipped;
    default: return Outcome::Cancelled;
    }
}

// ── Extraction ─────────────────────────────────────────────────

// Extracts one file from the image to outPath
static Outcome extract_file(OpContext &ctx, const dsk_tools::UniversalFile &uf,
                            const std::wstring &outPath)
{
    DWORD attrs = GetFileAttributesW(outPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
    {
        Outcome o = confirm_overwrite(ctx, outPath);
        if (o != Outcome::Ok)
            return o;
    }

    dsk_tools::BYTES fileData;
    auto r = ctx.panel.filesystem->get_file(uf, "FILE_BINARY", fileData);
    if (!r)
        return op_error(ctx, outPath, describe(msg(MCannotReadFile), r));

    // A read-only target (e.g. extracted before from an R/O file) would fail CREATE_ALWAYS
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(outPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return op_error(ctx, outPath, last_error_text(GetLastError()));

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, fileData.data(),
                        static_cast<DWORD>(fileData.size()), &written, nullptr);
    DWORD err = GetLastError();
    FILETIME ft = to_filetime(uf.date);
    if (ok && has_time(ft))
        SetFileTime(hFile, nullptr, nullptr, &ft);
    CloseHandle(hFile);
    if (!ok || written != fileData.size())
    {
        DeleteFileW(outPath.c_str());
        return op_error(ctx, outPath, ok ? std::wstring(msg(MWriteIncomplete)) : last_error_text(err));
    }
    return Outcome::Ok;
}

static Outcome make_dir(OpContext &ctx, const std::wstring &dirPath)
{
    if (CreateDirectoryW(dirPath.c_str(), nullptr))
        return Outcome::Ok;
    DWORD err = GetLastError();
    DWORD attrs = GetFileAttributesW(dirPath.c_str());
    if (err == ERROR_ALREADY_EXISTS && attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return Outcome::Ok;
    return op_error(ctx, dirPath, last_error_text(err));
}

// Extracts a listing (already named by build_wide_names) into destDir; worst outcome wins
static Outcome extract_list(OpContext &ctx, const dsk_tools::Files &files,
                            const std::vector<std::wstring> &names, const std::wstring &destDir);

// Recursively extracts a directory of a hierarchical filesystem into destDir\name
static Outcome extract_dir(OpContext &ctx, const dsk_tools::UniversalFile &dir,
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
        o = op_error(ctx, dirPath, describe(msg(MCannotReadDirectory), r));
    fs.cd_up();

    // After its contents: writing them changes the folder's time
    FILETIME ft = to_filetime(dir.date);
    if (o != Outcome::Cancelled && has_time(ft))
    {
        HANDLE h = CreateFileW(dirPath.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
    }
    return o;
}

// CP/M: copies a whole user area (virtual USER_nn directory) into destDir\USER_nn
static Outcome extract_cpm_area(OpContext &ctx, unsigned user, const std::wstring &destDir)
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
static Outcome extract_item(OpContext &ctx, const dsk_tools::UniversalFile &uf,
                            const std::wstring &name, const std::wstring &destDir)
{
    if (!uf.is_dir)
        return extract_file(ctx, uf, join_path(destDir, name));
    if (ctx.panel.cpmUsers)
        return extract_cpm_area(ctx, cpm_user(uf), destDir);
    return extract_dir(ctx, uf, name, destDir);
}

static Outcome extract_list(OpContext &ctx, const dsk_tools::Files &files,
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

// ── Saving the image ───────────────────────────────────────────

// Writes the image back to panel.hostPath: into a temporary file next to it, then
// ReplaceFileW, so a failure midway never leaves a half-written image.
// Returns an error text, empty on success.
static std::wstring save_image(PluginPanel &panel)
{
    auto writer = dsk_tools::create_writer(panel.formatId,
                                           static_cast<uint8_t>(panel.filesystem->get_volume_id()),
                                           panel.image.get());
    if (!writer)
        return msg(MNoWriter, { to_wide(panel.formatId) });
    dsk_tools::BYTES buffer;
    auto r = writer->write(buffer);
    if (!r)
        return describe(msg(MCannotBuildImage), r);

    std::wstring tmp = panel.hostPath + L".dsktools-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return tmp + L"\n" + last_error_text(GetLastError());
    DWORD written = 0;
    BOOL ok = WriteFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr)
              && written == buffer.size() && FlushFileBuffers(h);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        return tmp + L"\n" + last_error_text(err);
    }
    if (!ReplaceFileW(panel.hostPath.c_str(), tmp.c_str(), nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
    {
        err = GetLastError();
        DeleteFileW(tmp.c_str());
        return panel.hostPath + L"\n" + last_error_text(err);
    }
    panel.filesystem->reset_changed();
    return L"";
}

static void discard_changes(PluginPanel &panel, bool report);

// Saves the image after an operation and re-reads the panel listing. If saving fails,
// the changes are dropped by reloading the image, so the panel never shows what is
// not on disk.
static bool commit_changes(PluginPanel &panel, bool report)
{
    std::wstring error = save_image(panel);
    if (error.empty())
    {
        refresh_dir(panel);
        return true;
    }
    if (report)
        show_error(msg(MCannotSave) + (L"\n" + error));
    discard_changes(panel, report);
    return false;
}

// Drops the in-memory changes: reloads the image from disk and returns to the same place
static void discard_changes(PluginPanel &panel, bool report)
{
    std::vector<std::wstring> path = panel.dirStack;
    unsigned user = panel.cpmUser;
    std::wstring reload = mount_image(panel);
    if (!reload.empty())
    {
        if (report)
            show_error(msg(MCannotReload) + (L"\n" + reload));
        return;
    }
    if (panel.cpmUsers)
    {
        panel.cpmUser = user;
        refresh_dir(panel);
    }
    else
    {
        std::wstring p = L"/";
        for (const auto &d : path)
            p += d + L"/";
        set_dir(panel, p.c_str());
    }
}

// Can the operation `cap` be done on this image? Shows why not unless silent.
static bool check_writable(PluginPanel &panel, bool report, dsk_tools::FSCaps cap, LngId what)
{
    std::wstring reason = read_only_reason(panel);
    if (reason.empty() && !static_cast<bool>(panel.filesystem->get_caps() & cap))
        reason = msg(MFsNoSupport, { fs_name(panel.filesystem->get_fs()), msg(what) });
    if (reason.empty())
        return true;
    if (report)
        show_error(msg(MCannotModify) + (L"\n" + reason));
    return false;
}

// ── Writing into the image ─────────────────────────────────────

static bool read_host_file(const std::wstring &path, dsk_tools::BYTES &data, std::wstring &error)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error = last_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) != 0;
    if (ok && size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(h);
        error = msg(MFileTooLarge);
        return false;
    }
    DWORD read = 0;
    if (ok)
    {
        data.resize(static_cast<size_t>(size.QuadPart));
        ok = ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)
             && read == data.size();
    }
    if (!ok)
        error = last_error_text(GetLastError());
    CloseHandle(h);
    return ok;
}

// Entry of the current directory (CP/M: of the current user area) with this name
static bool find_entry(PluginPanel &panel, const std::wstring &name, dsk_tools::UniversalFile &found)
{
    dsk_tools::Files files;
    if (!list_dir(*panel.filesystem, files))
        return false;
    for (const auto &f : files)
        if ((!panel.cpmUsers || cpm_user(f) == panel.cpmUser)
            && _wcsicmp(to_wide(f.name).c_str(), name.c_str()) == 0)
        {
            found = f;
            return true;
        }
    return false;
}

static std::wstring name_error(PluginPanel &panel, const dsk_tools::Result &r, const wchar_t *what)
{
    if (r.code == dsk_tools::ErrorCode::InvalidName)
        return msg(MInvalidName, { fs_name(panel.filesystem->get_fs()) });
    return describe(what, r);
}

// Copies one host file into the current directory of the image
static Outcome put_file(OpContext &ctx, const std::wstring &srcPath, const std::wstring &name)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;

    dsk_tools::BYTES data;
    std::wstring error;
    if (!read_host_file(srcPath, data, error))
        return op_error(ctx, srcPath, error);

    bool replace = false;
    dsk_tools::UniversalFile existing;
    if (find_entry(panel, name, existing))
    {
        if (existing.is_dir)
            return op_error(ctx, name, msg(MDirExists));
        Outcome o = confirm_overwrite(ctx, name);
        if (o != Outcome::Ok)
            return o;
        replace = true;
        // DOS 3.3 put_file neither detects nor replaces an existing file
        if (fs.get_fs() == dsk_tools::FS::DOS33)
        {
            auto r = fs.delete_file(existing);
            if (!r)
                return fails_before_change(r) ? op_error(ctx, name, describe(msg(MCannotReplace), r))
                                              : op_broken(ctx, name, describe(msg(MCannotReplace), r));
            ctx.changed = true;
        }
    }

    dsk_tools::UniversalFile uf{};
    uf.fs   = dsk_tools::FS::None;
    uf.name = wide_to_utf8(name);
    uf.date = host_date(srcPath);
    if (panel.cpmUsers)
    {
        // fsCPM::put_file takes the user area from a CP/M source
        uf.fs         = dsk_tools::FS::CPM;
        uf.attributes = panel.cpmUser << 8;
    }

    std::string format = "FILE_BINARY";
    for (const auto &f : fs.get_add_file_formats())
        if (f == "FILE_ANY")
            format = f;

    auto r = fs.put_file(uf, format, data, replace);
    if (!r)
        return fails_before_change(r) ? op_error(ctx, name, name_error(panel, r, msg(MCannotWrite)))
                                      : op_broken(ctx, name, name_error(panel, r, msg(MCannotWrite)));
    ctx.changed = true;
    return Outcome::Ok;
}

static Outcome put_item(OpContext &ctx, const std::wstring &srcPath, const std::wstring &name, bool isDir);

// Copies a host directory tree into the current directory of the image
static Outcome put_dir(OpContext &ctx, const std::wstring &srcPath, const std::wstring &name)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;
    if (panel.cpmUsers || !static_cast<bool>(fs.get_caps() & dsk_tools::FSCaps::MkDir))
        return op_error(ctx, name, msg(MNoDirectories, { fs_name(fs.get_fs()) }));

    dsk_tools::UniversalFile dir;
    if (!find_entry(panel, name, dir))
    {
        dsk_tools::UniversalFile src{}, created;
        src.fs   = dsk_tools::FS::None;
        src.name = wide_to_utf8(name);
        src.date = host_date(srcPath);
        auto r = fs.mkdir(src, created);
        if (!r)
            return fails_before_change(r) ? op_error(ctx, name, name_error(panel, r, msg(MCannotCreateDir)))
                                          : op_broken(ctx, name, name_error(panel, r, msg(MCannotCreateDir)));
        ctx.changed = true;
        // Enter it through a fresh listing entry, as for any other directory
        if (!find_entry(panel, name, dir))
            return op_error(ctx, name, msg(MCannotFindNewDir));
    }
    else if (!dir.is_dir)
        return op_error(ctx, name, msg(MFileWithNameExists));

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(join_path(srcPath, L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
        return op_error(ctx, srcPath, last_error_text(GetLastError()));

    fs.cd(dir);
    Outcome result = Outcome::Ok;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        Outcome o = put_item(ctx, join_path(srcPath, fd.cFileName), fd.cFileName,
                             (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        if (o == Outcome::Cancelled)
        {
            result = o;
            break;
        }
        if (o == Outcome::Skipped)
            result = o;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    fs.cd_up();
    return result;
}

static Outcome put_item(OpContext &ctx, const std::wstring &srcPath, const std::wstring &name, bool isDir)
{
    return isDir ? put_dir(ctx, srcPath, name) : put_file(ctx, srcPath, name);
}

// Removes a host file or directory tree after it has been moved into the image
static bool delete_host(const std::wstring &path, bool isDir)
{
    if (!isDir)
    {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(join_path(path, L"*").c_str(), &fd);
    bool ok = true;
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            ok = delete_host(join_path(path, fd.cFileName),
                             (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) && ok;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) && ok;
}

// ── Deleting from the image ────────────────────────────────────

// Deletes a file, a directory tree (the libraries delete only empty directories)
// or a whole CP/M user area
static Outcome delete_item(OpContext &ctx, const dsk_tools::UniversalFile &uf, const std::wstring &name)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;

    if (uf.is_dir && panel.cpmUsers)
    {
        unsigned user = cpm_user(uf);
        panel.cpmNewAreas.erase(user);
        Outcome result = Outcome::Ok;
        for (const auto &f : panel.cpmAll)
            if (!f.is_dir && cpm_user(f) == user)
            {
                Outcome o = delete_item(ctx, f, to_wide(f.name));
                if (o == Outcome::Cancelled)
                    return o;
                if (o == Outcome::Skipped)
                    result = o;
            }
        return result;
    }

    if (uf.is_dir)
    {
        fs.cd(uf);
        dsk_tools::Files files;
        auto r = list_dir(fs, files);
        Outcome result = Outcome::Ok;
        if (!r)
            result = op_error(ctx, name, describe(msg(MCannotReadDirectory), r));
        auto names = build_wide_names(files);
        for (size_t i = 0; r && i < files.size(); ++i)
        {
            Outcome o = delete_item(ctx, files[i], names[i]);
            if (o == Outcome::Cancelled)
            {
                result = o;
                break;
            }
            if (o == Outcome::Skipped)
                result = o;
        }
        fs.cd_up();
        if (result != Outcome::Ok)
            return result;      // keep a directory that is not empty now
    }

    auto r = fs.delete_file(uf);
    if (!r)
        return fails_before_change(r) ? op_error(ctx, name, describe(msg(MCannotDelete), r))
                                      : op_broken(ctx, name, describe(msg(MCannotDelete), r));
    ctx.changed = true;
    return Outcome::Ok;
}

// ── Copy dialog ────────────────────────────────────────────────

enum class ExistingFiles { Ask, Overwrite, Skip };

// F5/F6 out of the image: FAR shows no dialog of its own there (it calls GetFilesW with
// the other panel's path), so the plugin does. Returns false on Cancel; dest is the
// destination as the user left it, existing what to do with files already there
// (Ask by default every time, as in FAR's own dialogs).
static bool copy_dialog(const GetFilesInfo *Info, PluginPanel &panel, std::wstring &dest,
                        ExistingFiles &existing)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    static const UUID DialogUuid = COPY_DIALOG_UUID;
    const bool move = Info->Move != 0;

    std::wstring prompt;
    if (Info->ItemsNumber == 1)
    {
        intptr_t idx = reinterpret_cast<intptr_t>(Info->PanelItem[0].UserData.Data);
        std::wstring name = (idx >= 0 && static_cast<size_t>(idx) < panel.wideNames.size())
                          ? panel.wideNames[idx]
                          : std::wstring(Info->PanelItem[0].FileName ? Info->PanelItem[0].FileName : L"");
        if (name.size() > 50)
            name = name.substr(0, 47) + L"...";
        prompt = msg(move ? MMoveTo : MCopyTo, { L"\"" + name + L"\"" });
    }
    else
        prompt = msg(move ? MMoveTo : MCopyTo, { items_text(Info->ItemsNumber) });

    // Laid out like FAR's own copy dialog; the builder sizes it by the translated texts
    PluginDialogBuilder builder(PSI, PluginUuid, DialogUuid, move ? MMoveTitle : MCopyTitle, nullptr);
    builder.AddText(prompt.c_str());
    builder.AddEditField(dest, 66, L"Copy")->Flags |= DIF_EDITPATH | DIF_EDITEXPAND;
    builder.AddSeparator();
    static const int existingIds[] = { MExistingAsk, MExistingOverwrite, MExistingSkip };
    int choice = static_cast<int>(ExistingFiles::Ask);
    auto *combo = builder.AddComboBox(&choice, nullptr, 20, existingIds, ARRAYSIZE(existingIds), DIF_DROPDOWNLIST);
    builder.AddTextBefore(combo, MExistingFiles);
    builder.AddOKCancel(move ? MMoveButton : MCopyButton, MDialogCancel);

    if (!builder.ShowDialog())
        return false;
    existing = static_cast<ExistingFiles>(choice);
    return true;
}

// The path typed in the copy dialog: unquoted, a relative one taken from `base`
// (the folder FAR offered), normalized
static std::wstring resolve_dest(std::wstring path, const std::wstring &base)
{
    auto first = path.find_first_not_of(L" \"");
    auto last  = path.find_last_not_of(L" \"");
    path = (first == std::wstring::npos) ? L"" : path.substr(first, last - first + 1);
    if (path.empty())
        return base;

    bool absolute = (path.size() >= 2 && path[1] == L':') || path.compare(0, 2, L"\\\\") == 0;
    if (!absolute)
    {
        if (path[0] == L'\\' || path[0] == L'/')
            path = base.substr(0, 2) + path;    // root of the base drive
        else
            path = join_path(base, path);
    }

    size_t len = FSF.ConvertPath(CPM_FULL, path.c_str(), nullptr, 0);
    if (len)
    {
        std::vector<wchar_t> full(len);
        FSF.ConvertPath(CPM_FULL, path.c_str(), full.data(), len);
        path = full.data();
    }
    return path;
}

static bool is_host_dir(const std::wstring &path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Creates a folder with all missing parents
static bool create_host_dirs(const std::wstring &path)
{
    if (path.empty() || is_host_dir(path))
        return true;
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 2 && !create_host_dirs(path.substr(0, pos)))
        return false;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// ═══════════════════════════════════════════════════════════════
//  File operation exports
// ═══════════════════════════════════════════════════════════════

extern "C" intptr_t WINAPI GetFilesW(struct GetFilesInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    const bool silent = (Info->OpMode & (OPM_SILENT | OPM_FIND | OPM_QUICKVIEW)) != 0;

    // F6: extract, then delete from the image
    if (Info->Move && !check_writable(panel, !silent, dsk_tools::FSCaps::Delete, MOpDeleting))
        return FALSE;

    // F3/F4/Ctrl+Q extract into a temporary folder: no dialog and no overwrite questions there
    const bool interactive =
        (Info->OpMode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW | OPM_DESCR)) == 0;
    OpContext ctx{ panel, interactive, !silent, msg(MCannotExtract) };

    std::wstring destDir = Info->DestPath ? Info->DestPath : L"";
    std::wstring renameTo;      // one file copied under a name typed in the dialog
    if (interactive)
    {
        std::wstring typed = destDir;
        ExistingFiles existing = ExistingFiles::Ask;
        if (!copy_dialog(Info, panel, typed, existing))
            return -1;
        std::wstring dest = resolve_dest(typed, destDir);

        // As in FAR: a single file goes to a path that is not a folder under that name
        bool toFolder = Info->ItemsNumber != 1 || is_host_dir(dest) || typed.empty()
                     || typed.back() == L'\\' || typed.back() == L'/';
        if (!toFolder)
        {
            intptr_t idx = reinterpret_cast<intptr_t>(Info->PanelItem[0].UserData.Data);
            toFolder = idx < 0 || static_cast<size_t>(idx) >= panel.files.size() || panel.files[idx].is_dir;
        }
        if (toFolder)
            destDir = dest;
        else
        {
            auto pos = dest.find_last_of(L"\\/");
            destDir  = dest.substr(0, pos);
            renameTo = dest.substr(pos + 1);
        }
        if (!create_host_dirs(destDir))
        {
            show_error(msg(MCannotCreateFolder) + (L"\n" + destDir + L"\n" + last_error_text(GetLastError())));
            return FALSE;
        }
        panel.destPath  = destDir;
        Info->DestPath  = panel.destPath.c_str();   // FAR updates descriptions there
        ctx.overwriteAll = existing == ExistingFiles::Overwrite;
        ctx.skipExisting = existing == ExistingFiles::Skip;
    }

    std::vector<size_t> extracted;
    bool cancelled = false;
    for (size_t i = 0; i < Info->ItemsNumber && !cancelled; ++i)
    {
        auto &item = Info->PanelItem[i];
        intptr_t idx = reinterpret_cast<intptr_t>(item.UserData.Data);
        if (idx < 0 || static_cast<size_t>(idx) >= panel.files.size())
            continue;

        Outcome o = extract_item(ctx, panel.files[idx],
                                 renameTo.empty() ? panel.wideNames[idx] : renameTo, destDir);
        if (o == Outcome::Cancelled)
            cancelled = true;
        else if (o == Outcome::Ok)
        {
            extracted.push_back(i);
            // Unless all went well, FAR deselects only the items with this flag cleared
            item.Flags &= ~PPIF_SELECTED;
        }
    }

    if (Info->Move && !extracted.empty())
    {
        // Only what was extracted completely is deleted
        ctx.action = msg(MCannotDelete);
        std::vector<std::pair<dsk_tools::UniversalFile, std::wstring>> victims;
        for (size_t i : extracted)
        {
            auto idx = reinterpret_cast<intptr_t>(Info->PanelItem[i].UserData.Data);
            victims.emplace_back(panel.files[idx], panel.wideNames[idx]);
        }
        for (const auto &v : victims)
            if (delete_item(ctx, v.first, v.second) == Outcome::Cancelled)
                break;
        if (ctx.broken)
        {
            discard_changes(panel, !silent);
            return FALSE;
        }
        if (ctx.changed && !commit_changes(panel, !silent))
            return FALSE;
    }

    if (cancelled)
        return -1;
    // TRUE makes FAR deselect everything: only when every item is done
    return extracted.size() == Info->ItemsNumber ? TRUE : FALSE;
}

// ── Delete confirmations ───────────────────────────────────────
// FAR asks nothing before DeleteFiles on a plugin panel (FileList::PluginDelete),
// so the plugin asks itself, following FAR's own Confirmations settings.

// A switch of Options > Confirmations ("Delete", "DeleteFolder"); true if unreadable
static bool far_confirmation(const wchar_t *name)
{
    FarSettingsCreate create = { sizeof(FarSettingsCreate), FarGuid, INVALID_HANDLE_VALUE };
    if (!PSI.SettingsControl(INVALID_HANDLE_VALUE, SCTL_CREATE, PSL_ROAMING, &create))
        return true;
    FarSettingsItem item = {};
    item.StructSize = sizeof(FarSettingsItem);
    item.Root       = FSSF_CONFIRMATIONS;
    item.Name       = name;
    item.Type       = FST_UNKNOWN;
    bool result = true;
    if (PSI.SettingsControl(create.Handle, SCTL_GET, 0, &item) && item.Type == FST_QWORD)
        result = item.Number != 0;
    PSI.SettingsControl(create.Handle, SCTL_FREE, 0, nullptr);
    return result;
}

using Victims = std::vector<std::pair<dsk_tools::UniversalFile, std::wstring>>;

// "Do you wish to delete ...": Delete / Cancel
static bool confirm_delete(PluginPanel &panel, const Victims &victims)
{
    std::vector<std::wstring> lines;
    if (victims.size() == 1)
    {
        const auto &v = victims.front();
        lines.push_back(msg(!v.first.is_dir ? MDeleteFile : panel.cpmUsers ? MDeleteArea : MDeleteFolder));
        lines.push_back(v.second);
    }
    else
    {
        lines.push_back(msg(MDeleteItems));
        lines.push_back(items_text(victims.size()));
    }
    return ask(lines, { msg(MDelete), msg(MCancel) }) == 0;
}

// Does a selected folder (or CP/M user area) have anything in it?
static bool dir_has_entries(PluginPanel &panel, const dsk_tools::UniversalFile &dir)
{
    if (panel.cpmUsers)
    {
        for (const auto &f : panel.cpmAll)
            if (!f.is_dir && cpm_user(f) == cpm_user(dir))
                return true;
        return false;
    }
    panel.filesystem->cd(dir);
    dsk_tools::Files files;
    bool any = list_dir(*panel.filesystem, files) && !files.empty();
    panel.filesystem->cd_up();
    return any;
}

extern "C" intptr_t WINAPI DeleteFilesW(const struct DeleteFilesInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    const bool silent = (Info->OpMode & OPM_SILENT) != 0;
    if (!check_writable(panel, !silent, dsk_tools::FSCaps::Delete, MOpDeleting))
        return FALSE;

    // Copies: the listing is re-read only after the whole operation
    Victims victims;
    for (size_t i = 0; i < Info->ItemsNumber; ++i)
    {
        intptr_t idx = reinterpret_cast<intptr_t>(Info->PanelItem[i].UserData.Data);
        if (idx >= 0 && static_cast<size_t>(idx) < panel.files.size())
            victims.emplace_back(panel.files[idx], panel.wideNames[idx]);
    }

    if (victims.empty())
        return FALSE;
    if (!silent && far_confirmation(L"Delete") && !confirm_delete(panel, victims))
        return FALSE;

    const bool confirmFolders = !silent && far_confirmation(L"DeleteFolder");
    bool deleteAllFolders = false;

    OpContext ctx{ panel, false, !silent, msg(MCannotDelete) };
    bool cancelled = false, anyOk = false;
    for (const auto &v : victims)
    {
        // As FAR does: once more for every selected folder that is not empty
        if (v.first.is_dir && confirmFolders && !deleteAllFolders && dir_has_entries(panel, v.first))
        {
            intptr_t answer = ask({ msg(panel.cpmUsers ? MDeleteNonEmptyArea : MDeleteNonEmptyFolder), v.second },
                                  { msg(MDelete), msg(MAll), msg(MSkip), msg(MCancel) });
            if (answer == 1)
                deleteAllFolders = true;
            else if (answer == 2)
                continue;
            else if (answer != 0)
            {
                cancelled = true;
                break;
            }
        }

        Outcome o = delete_item(ctx, v.first, v.second);
        if (o == Outcome::Cancelled)
        {
            cancelled = true;
            break;
        }
        anyOk |= (o == Outcome::Ok);
    }

    if (ctx.broken)
    {
        discard_changes(panel, !silent);
        return FALSE;
    }
    if (ctx.changed)
    {
        if (!commit_changes(panel, !silent))
            return FALSE;
    }
    else if (panel.cpmUsers)
        build_cpm_view(panel);  // an empty area made by F7 may have gone

    if (cancelled)
        return -1;
    return anyOk ? TRUE : FALSE;
}

extern "C" intptr_t WINAPI MakeDirectoryW(struct MakeDirectoryInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    const bool silent = (Info->OpMode & OPM_SILENT) != 0;
    if (!check_writable(panel, !silent,
                        panel.cpmUsers ? dsk_tools::FSCaps::Add : dsk_tools::FSCaps::MkDir,
                        MOpDirectories))
        return FALSE;
    if (panel.cpmUsers && panel.cpmUser != 0)
    {
        if (!silent)
            show_error(msg(MAreasAtRoot));
        return FALSE;
    }

    std::wstring name = Info->Name ? Info->Name : L"";
    if (!silent)
    {
        static const UUID PluginUuid = PLUGIN_UUID;
        wchar_t buf[256] = {};
        if (!PSI.InputBox(&PluginUuid, nullptr, msg(MMakeDirTitle),
                          msg(panel.cpmUsers ? MMakeDirArea : MMakeDirName),
                          L"DskTools.MakeDirectory", name.c_str(), buf, ARRAYSIZE(buf),
                          nullptr, FIB_BUTTONS | FIB_NOUSELASTHISTORY))
            return -1;
        name = buf;
    }
    auto first = name.find_first_not_of(L' ');
    auto last  = name.find_last_not_of(L' ');
    name = (first == std::wstring::npos) ? L"" : name.substr(first, last - first + 1);
    if (name.empty())
        return -1;

    if (panel.cpmUsers)
    {
        // A user area exists in CP/M only through its files: until the first file is
        // copied into it, the new USER_nn lives in the panel only
        int user = parse_cpm_user_dir(name);
        if (user < 0 && name.size() <= 2 && iswdigit(name[0]) && (name.size() == 1 || iswdigit(name[1])))
            user = _wtoi(name.c_str());
        if (user < 1 || user > static_cast<int>(CPM_MAX_USER))
        {
            if (!silent)
                show_error(msg(MBadUserArea));
            return FALSE;
        }
        panel.cpmNewAreas.insert(static_cast<unsigned>(user));
        build_cpm_view(panel);
        panel.newDirName = cpm_user_dir(static_cast<unsigned>(user));
        Info->Name = panel.newDirName.c_str();
        return TRUE;
    }

    dsk_tools::UniversalFile created;
    auto r = panel.filesystem->mkdir(wide_to_utf8(name), created);
    if (!r)
    {
        if (!silent)
            show_error(name + L"\n" + name_error(panel, r, msg(MCannotCreateDir)));
        if (!fails_before_change(r))
            discard_changes(panel, !silent);
        return FALSE;
    }
    if (!commit_changes(panel, !silent))
        return FALSE;

    // FAR puts the cursor on Info->Name: give it the name as the panel shows it
    panel.newDirName = name;
    for (const auto &n : panel.wideNames)
        if (_wcsicmp(n.c_str(), name.c_str()) == 0)
            panel.newDirName = n;
    Info->Name = panel.newDirName.c_str();
    return TRUE;
}

extern "C" intptr_t WINAPI PutFilesW(const struct PutFilesInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    const bool silent = (Info->OpMode & (OPM_SILENT | OPM_FIND)) != 0;
    if (!check_writable(panel, !silent, dsk_tools::FSCaps::Add, MOpAdding))
        return FALSE;

    OpContext ctx{ panel, !silent, !silent, msg(MCannotCopyInto) };
    std::vector<size_t> done;
    bool cancelled = false;
    for (size_t i = 0; i < Info->ItemsNumber && !cancelled; ++i)
    {
        auto &item = Info->PanelItem[i];
        std::wstring src = join_path(Info->SrcPath ? Info->SrcPath : L"", item.FileName);
        Outcome o = put_item(ctx, src, extract_filename(item.FileName),
                             (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        if (o == Outcome::Cancelled)
            cancelled = true;
        else if (o == Outcome::Ok)
        {
            done.push_back(i);
            item.Flags &= ~PPIF_SELECTED;   // see GetFilesW
        }
    }

    if (ctx.broken)
    {
        discard_changes(panel, !silent);   // the sources of a move stay where they are
        return FALSE;
    }
    if (ctx.changed && !commit_changes(panel, !silent))
        return FALSE;
    if (!ctx.changed && panel.cpmUsers)
        build_cpm_view(panel);

    // F6: the sources go only once they are safely in the saved image
    if (Info->Move)
        for (size_t i : done)
        {
            const auto &item = Info->PanelItem[i];
            delete_host(join_path(Info->SrcPath ? Info->SrcPath : L"", item.FileName),
                        (item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }

    if (cancelled)
        return -1;
    return done.size() == Info->ItemsNumber ? TRUE : FALSE;
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
