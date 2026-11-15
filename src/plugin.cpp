#include "plugin.hpp"
#include "lang.hpp"
#include "info_tokens.hpp"
#include "uuid.hpp"
#include "version.hpp"
#include "cpm_geometry.hpp"
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

// Saves of every image by any panel, by name_key() of its path (sync_image()): exact even
// where the file time is too coarse to tell two saves apart (FAT: 2 s; a raw image never
// changes its size). The file time and size catch the changes made by other programs.
static std::map<std::wstring, unsigned> g_SaveCounts;

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
static std::wstring name_key(std::wstring s)
{
    CharUpperBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

// `name` made unique among `used` (keys of name_key()), then added there
static std::wstring unique_name(std::wstring name, bool isDir, std::set<std::wstring> &used)
{
    if (used.count(name_key(name)))
    {
        auto dot = isDir ? std::wstring::npos : name.rfind(L'.');
        std::wstring stem = name.substr(0, dot);
        std::wstring ext  = (dot == std::wstring::npos) ? L"" : name.substr(dot);
        for (int n = 2; used.count(name_key(name)); ++n)
            name = stem + L"~" + std::to_wstring(n) + ext;
    }
    used.insert(name_key(name));
    return name;
}

static std::vector<std::wstring> build_wide_names(const dsk_tools::Files &files)
{
    std::vector<std::wstring> names;
    names.reserve(files.size());
    std::set<std::wstring> used;
    for (const auto &f : files)
        names.push_back(unique_name(sanitize_name(to_wide(f.name)), f.is_dir, used));
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

// The text of a dsk_tools error (English: the library has no translations)
static std::wstring error_text(const dsk_tools::Result &r)
{
    std::wstring text = to_wide(dsk_tools::decode_error(r));
    if (!r.message.empty())
        text += L"\n" + to_wide(r.message);
    return text;
}

static std::wstring describe(const wchar_t *what, const dsk_tools::Result &r)
{
    return what + (L": " + error_text(r));
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

// ── Settings (F9 > Plugins configuration > DskTools) ───────────
// Kept in FAR's profile under the plugin GUID. The defaults are the behaviour before
// there were settings.

// Filesystems AnalyseW may claim, as detect_fdd_type() names them; in the order of the
// dialog: two columns, the first FS_LEFT_COLUMN in the left one
struct FsSwitch
{
    const char    *ids[3];
    const wchar_t *key;     // settings name
    LngId          label;
};
static const FsSwitch FS_SWITCHES[] =
{
    { { "FILESYSTEM_FAT" },                                                     L"OpenFAT",      MCfgFsFat },
    { { "FILESYSTEM_CPM_RAW", "FILESYSTEM_CPM_DOS", "FILESYSTEM_CPM_PRODOS" }, L"OpenCPM",      MCfgFsCpm },
    { { "FILESYSTEM_RT11" },                                                    L"OpenRT11",     MCfgFsRt11 },
    { { "FILESYSTEM_DOS33" },                                                   L"OpenDOS33",    MCfgFsDos33 },
    { { "FILESYSTEM_FIL" },                                                     L"OpenFIL",      MCfgFsFil },
    { { "FILESYSTEM_PRODOS" },                                                  L"OpenProDOS",   MCfgFsProdos },
    { { "FILESYSTEM_SPRITE_OS" },                                               L"OpenSpriteOS", MCfgFsSprite },
    { { "FILESYSTEM_ONIX" },                                                    L"OpenOnix",     MCfgFsOnix },
    { { "FILESYSTEM_ISKRA-226" },                                               L"OpenIskra226", MCfgFsIskra },
};
static const size_t FS_COUNT = ARRAYSIZE(FS_SWITCHES);
static const size_t FS_LEFT_COLUMN = 5;
static_assert(FS_COUNT == 9, "a filesystem added: give it a default in Settings::openFs");

struct Settings
{
    bool         openOnEnter = true;    // AnalyseW claims images on Enter / Ctrl+PgDn
    std::wstring mask        = L"*";    // ... only files matching this FAR mask
    bool         openFs[FS_COUNT] = { true, true, true, true, true, true, true, true, true };
                                        // ... and only these filesystems (FS_SWITCHES)
    bool         allowWrite  = true;    // otherwise every image is read-only
    bool         backup      = false;   // <image>.bak before the first change in a panel
};
static Settings g_Settings;

namespace
{
    // The plugin's own settings root for the time of one read or write
    class PluginSettings
    {
    public:
        PluginSettings()
        {
            FarSettingsCreate create = { sizeof(FarSettingsCreate), PLUGIN_UUID, INVALID_HANDLE_VALUE };
            if (PSI.SettingsControl(INVALID_HANDLE_VALUE, SCTL_CREATE, PSL_ROAMING, &create))
                handle = create.Handle;
        }
        ~PluginSettings()
        {
            if (handle != INVALID_HANDLE_VALUE)
                PSI.SettingsControl(handle, SCTL_FREE, 0, nullptr);
        }
        void get(const wchar_t *name, bool &value) const
        {
            FarSettingsItem item = { sizeof(FarSettingsItem), 0, name, FST_QWORD, {} };
            if (handle != INVALID_HANDLE_VALUE && PSI.SettingsControl(handle, SCTL_GET, 0, &item))
                value = item.Number != 0;
        }
        void get(const wchar_t *name, std::wstring &value) const
        {
            FarSettingsItem item = { sizeof(FarSettingsItem), 0, name, FST_STRING, {} };
            if (handle != INVALID_HANDLE_VALUE && PSI.SettingsControl(handle, SCTL_GET, 0, &item) && item.String)
                value = item.String;
        }
        void set(const wchar_t *name, bool value) const
        {
            FarSettingsItem item = { sizeof(FarSettingsItem), 0, name, FST_QWORD, {} };
            item.Number = value ? 1 : 0;
            if (handle != INVALID_HANDLE_VALUE)
                PSI.SettingsControl(handle, SCTL_SET, 0, &item);
        }
        void set(const wchar_t *name, const std::wstring &value) const
        {
            FarSettingsItem item = { sizeof(FarSettingsItem), 0, name, FST_STRING, {} };
            item.String = value.c_str();
            if (handle != INVALID_HANDLE_VALUE)
                PSI.SettingsControl(handle, SCTL_SET, 0, &item);
        }
    private:
        HANDLE handle = INVALID_HANDLE_VALUE;
    };
}

static void load_settings()
{
    PluginSettings s;
    s.get(L"OpenOnEnter", g_Settings.openOnEnter);
    s.get(L"Mask",        g_Settings.mask);
    for (size_t i = 0; i < FS_COUNT; ++i)
        s.get(FS_SWITCHES[i].key, g_Settings.openFs[i]);
    s.get(L"AllowWrite",  g_Settings.allowWrite);
    s.get(L"Backup",      g_Settings.backup);
}

static void save_settings()
{
    PluginSettings s;
    s.set(L"OpenOnEnter", g_Settings.openOnEnter);
    s.set(L"Mask",        g_Settings.mask);
    for (size_t i = 0; i < FS_COUNT; ++i)
        s.set(FS_SWITCHES[i].key, g_Settings.openFs[i]);
    s.set(L"AllowWrite",  g_Settings.allowWrite);
    s.set(L"Backup",      g_Settings.backup);
}

// May AnalyseW claim an image of this filesystem? A filesystem the table does not know (a
// newer dsk_tools) is allowed; none (a known format with an unknown filesystem) is not:
// OpenW could only report "Unsupported filesystem", while FAR may hand the file elsewhere
static bool filesystem_enabled(const std::string &fsId)
{
    if (fsId.empty())
        return false;
    for (size_t i = 0; i < FS_COUNT; ++i)
        for (const char *id : FS_SWITCHES[i].ids)
            if (id && fsId == id)
                return g_Settings.openFs[i];
    return true;
}

// Does the file name (without the path) match the mask of the settings?
static bool matches_mask(const std::wstring &path)
{
    std::wstring name = extract_filename(path);
    return FSF.ProcessName(g_Settings.mask.c_str(), &name[0], 0, PN_CMPNAMELIST | PN_SKIPPATH) != 0;
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
    std::wstring   path;
    std::string    formatId;
    std::string    typeId;
    std::string    fsId;
    GeometrySource geometry = GeometrySource::Standard;
    bool           geometryWritable = true;
};

// AnalyseW results, owned until OpenW(OPEN_ANALYSE) or CloseAnalyseW
static std::map<intptr_t, DetectedImage> g_Analysed;
static intptr_t g_NextAnalyseHandle = 1;

static bool find_cpm_geometry(DetectedImage &di, bool detected);

static bool detect_image(const std::wstring &path, DetectedImage &out, std::wstring *error)
{
    out.path = path;
    auto r = dsk_tools::detect_fdd_type(wide_to_utf8(path), out.formatId, out.typeId, out.fsId, false);
    // dsk_tools picks a CP/M geometry by the extension and the size alone
    if (!find_cpm_geometry(out, static_cast<bool>(r)))
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

// The library's own diskdefs
static const dsk_tools::DiskDefs &library_diskdefs()
{
    static const dsk_tools::DiskDefs defs =
        dsk_tools::parse_diskdefs(dsk_tools::embedded_diskdefs());
    return defs;
}

// ... plus the CP/M geometries find_cpm_geometry() registered
static dsk_tools::DiskDefs &diskdefs()
{
    static dsk_tools::DiskDefs defs = library_diskdefs();
    return defs;
}

// ── CP/M geometry ──────────────────────────────────────────────
// dsk_tools takes the geometry of a CP/M image from its extension and size, and a wrong
// one shows garbage instead of names (and fsCPM reads past the image on such entries).
// So a raw CP/M image is checked by its directory. A geometry from the disk's own
// parameter block in the boot sector comes first, then dsk_tools' one, then the CP/M
// diskdefs of that size with the system tracks counted by sides. Only the first may be
// written; an image whose directory fits none opens as dsk_tools sees it, read-only.

// The geometry of a TYPE_CPM:<diskdef> type
static bool geometry_of_type(const std::string &typeId, cpm_geometry::Geometry &g)
{
    if (typeId.rfind("TYPE_CPM:", 0) != 0)
        return false;
    std::string key = typeId.substr(9);
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
    auto it = diskdefs().find(key);
    return it != diskdefs().end() && cpm_geometry::from_diskdef(it->second, g);
}

static std::string cpm_type(const std::string &name)
{
    std::string type = "TYPE_CPM:" + name;
    std::transform(type.begin(), type.end(), type.begin(), [](char c) { return static_cast<char>(toupper(static_cast<unsigned char>(c))); });
    return type;
}

// The first bytes of the file and its size
static bool read_head(const std::wstring &path, uint8_t *buf, DWORD size, unsigned long long &fileSize)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER li = {};
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &li) && ReadFile(h, buf, size, &got, nullptr) && got == size;
    CloseHandle(h);
    fileSize = static_cast<unsigned long long>(li.QuadPart);
    return ok;
}

static void use_geometry(DetectedImage &di, const std::string &name, const cpm_geometry::Geometry &g,
                         GeometrySource source, bool writable)
{
    diskdefs()[name]    = cpm_geometry::to_diskdef(g, name);
    di.formatId         = "FILE_RAW_MSB";
    di.typeId           = cpm_type(name);
    di.fsId             = "FILESYSTEM_CPM_RAW";
    di.geometry         = source;
    di.geometryWritable = writable;
}

// Checks (and replaces) the geometry of a raw CP/M image dsk_tools detected, and opens one
// it did not by the parameter block of its boot sector. False: the image stays unknown.
static bool find_cpm_geometry(DetectedImage &di, bool detected)
{
    if (detected && !(di.formatId == "FILE_RAW_MSB" && di.typeId.rfind("TYPE_CPM:", 0) == 0))
        return detected;

    uint8_t boot[32];
    unsigned long long size = 0;
    if (!read_head(di.path, boot, sizeof(boot), size))
        return detected;
    cpm_geometry::Geometry standard;
    const bool known = detected && geometry_of_type(di.typeId, standard);
    if (detected && !known)
        return true;

    cpm_geometry::Found found;
    if (!cpm_geometry::find(wide_to_utf8(di.path), boot, sizeof(boot), size,
                            known ? &standard : nullptr, library_diskdefs(), found))
        return detected;
    if (found.source == GeometrySource::Boot || found.source == GeometrySource::Guessed)
        use_geometry(di, found.name, found.geometry, found.source, found.writable);
    else
    {
        di.geometry         = found.source;
        di.geometryWritable = found.writable;
    }
    return true;
}

// Loads panel.hostPath as panel.formatId/typeId/fsId and mounts its filesystem.
// Returns an error text, empty on success. Used to open an image and to reload it.
static void remember_stamp(PluginPanel &panel);
static void sync_image(PluginPanel &panel, bool report);

// The block numbers fsCPM can read: DSM and the directory blocks (and those a file shorter
// than the disk holds: the loader fills the rest with E5). After mounting and after a save,
// which may lengthen a short file.
static void set_cpm_bounds(PluginPanel &panel)
{
    panel.cpmDsm = panel.cpmDirBlocks = panel.cpmLastBlock = 0;
    if (!panel.filesystem || panel.filesystem->get_fs() != dsk_tools::FS::CPM)
        return;
    cpm_geometry::Geometry g;
    if (geometry_of_type(panel.typeId, g))
    {
        panel.cpmDsm       = g.dsm();
        panel.cpmDirBlocks = g.dir_blocks();
        // fsCPM reads up to the last block of the tracks, past a lower "dsm"
        long long last     = panel.diskSize ? cpm_geometry::last_block_in_file(g, panel.diskSize) : g.last_block();
        panel.cpmLastBlock = last < 0 ? 0 : static_cast<unsigned>(std::min<long long>(last, g.last_block()));
    }
    else if (panel.typeId == "TYPE_AGAT_140")
    {
        panel.cpmDsm       = 127;   // the DPB fsCPM::open() sets for it
        panel.cpmDirBlocks = 2;
        panel.cpmLastBlock = 127;
    }
}

static std::wstring mount_image(PluginPanel &panel)
{
    std::string uPath = wide_to_utf8(panel.hostPath);
    remember_stamp(panel);      // before reading: a change made meanwhile is seen next time

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
    panel.infoValid  = false;

    set_cpm_bounds(panel);
    return L"";
}

// Whether fsCPM may read the file: a block number past the disk in a damaged entry would
// make it read outside the image
static bool cpm_blocks_ok(const PluginPanel &panel, const dsk_tools::UniversalFile &uf)
{
    return !panel.cpmDsm || uf.is_dir
        || cpm_geometry::blocks_valid(uf, panel.cpmDsm, panel.cpmDirBlocks, panel.cpmLastBlock);
}

// Why the image cannot be modified, empty if it can. Only raw images are saved for now:
// they are written back byte for byte, while HFE/MFM writers re-encode every track.
static std::wstring read_only_reason(PluginPanel &panel)
{
    using dsk_tools::FSCaps;
    if (!g_Settings.allowWrite)
        return msg(MWriteDisabled);
    if (!static_cast<bool>(panel.filesystem->get_caps() & FSCaps::Add))
        return msg(MFsReadOnly, { fs_name(panel.filesystem->get_fs()) });
    if (!panel.geometryWritable)
        return msg(panel.geometry == GeometrySource::Guessed ? MGeoGuessedRO
                 : panel.geometry == GeometrySource::Damaged ? MDirDamaged : MGeoDiffersRO);
    if (panel.formatId != "FILE_RAW_MSB")
        return msg(MFormatNotSaved, { to_wide(panel.formatId) });

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(panel.hostPath.c_str(), GetFileExInfoStandard, &fad))
        return msg(MCannotReadAttributes);
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        return msg(MImageReadOnly);
    unsigned long long size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    // A CP/M file with its empty end cut off grows as far as the data needs (save_image()).
    // Only the boot sector geometry opens such files; the E5 filler suits CP/M alone.
    if (size < panel.image->get_buffer()->size() && panel.geometry != GeometrySource::Boot)
        return msg(MImageShort);
    if (size < panel.image->get_buffer()->size())
        return L"";
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
    panel.geometry         = di.geometry;
    panel.geometryWritable = di.geometryWritable;

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
    load_settings();
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
    // Only the opening by Enter / Ctrl+PgDn obeys these; dsk: and F11 open any file
    if (!g_Settings.openOnEnter || !matches_mask(Info->FileName))
        return nullptr;

    // A file not claimed here goes on to other plugins, then to FAR's own Enter
    DetectedImage di;
    if (!detect_image(Info->FileName, di, nullptr) || !filesystem_enabled(di.fsId))
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

// ── Information panel (Ctrl+L) ─────────────────────────────────

// A dsk_tools text with its {$TOKEN} placeholders replaced by their labels (info_tokens.hpp);
// a token not there is shown by its name: PRODOS_DATA_FORK -> "Prodos data fork"
static std::wstring replace_tokens(const std::string &text)
{
    std::wstring out;
    size_t pos = 0;
    for (;;)
    {
        size_t open  = text.find("{$", pos);
        size_t close = open == std::string::npos ? open : text.find('}', open);
        if (close == std::string::npos)
            return out + to_wide(text.substr(pos));
        out += to_wide(text.substr(pos, open - pos));
        const std::string token = text.substr(open + 2, close - open - 2);
        const InfoToken *known = nullptr;
        for (const auto &t : INFO_TOKENS)
            if (token == t.token)
                known = &t;
        if (known)
            out += msg(known->id);
        else
        {
            std::wstring name = to_wide(token);
            for (auto &c : name)
                c = c == L'_' ? L' ' : static_cast<wchar_t>(towlower(c));
            if (!name.empty())
                name[0] = static_cast<wchar_t>(towupper(name[0]));
            out += name;
        }
        pos = close + 1;
    }
}

static std::wstring format_size(unsigned long long size)
{
    wchar_t buf[64] = {};
    FSF.FormatFileSize(size, 0, FFFS_COMMAS, buf, ARRAYSIZE(buf));
    return buf;
}

// Info panel lines: the image (file, format, type, filesystem, why it is read-only), the
// space from get_stats() and the filesystem's own information(), whose lines are
// "Label: value", "Group:" (a separator here) or plain text
static void build_info(PluginPanel &panel)
{
    struct Row { std::wstring text, data; bool separator; };
    std::vector<Row> rows;
    auto add = [&](const std::wstring &text, const std::wstring &data) { rows.push_back({ text, data, false }); };
    auto group = [&](const std::wstring &text) { rows.push_back({ text, L"", true }); };
    auto strip = [](const std::string &id, const char *prefix)
    {
        return to_wide(id.compare(0, strlen(prefix), prefix) == 0 ? id.substr(strlen(prefix)) : id);
    };
    auto &fs = *panel.filesystem;

    group(msg(MInfoImage));
    add(msg(MInfoFile), extract_filename(panel.hostPath));
    add(msg(MInfoFormat), strip(panel.formatId, "FILE_"));
    add(msg(MInfoType), strip(panel.typeId, "TYPE_"));
    add(msg(MInfoFilesystem), fs_name(fs.get_fs()));
    if (panel.fsId == "FILESYSTEM_CPM_RAW")
        add(msg(MInfoGeometry), msg(panel.geometry == GeometrySource::Boot    ? MGeoBoot
                                  : panel.geometry == GeometrySource::Guessed ? MGeoGuessed
                                  : panel.geometry == GeometrySource::Damaged ? MDirDamaged : MGeoStandard));
    if (fs.get_volume_id() >= 0)
        add(msg(MInfoVolume), std::to_wstring(fs.get_volume_id()));
    std::wstring readOnly = read_only_reason(panel);
    add(msg(MInfoWrite), readOnly.empty() ? std::wstring(msg(MInfoWritable)) : readOnly);

    auto stats = fs.get_stats().int_values;
    panel.freeSize = 0;
    if (stats["total_space"] > 0)
    {
        group(msg(MInfoSpace));
        add(msg(MInfoTotal), format_size(stats["total_space"]));
        add(msg(MInfoUsed),  format_size(stats["occupied_space"]));
        add(msg(MInfoFree),  format_size(stats["free_space"]));
        panel.freeSize = stats["free_space"];
    }

    std::string info = fs.information();
    if (!info.empty() && info != "Not implemented")
    {
        group(fs_name(fs.get_fs()));
        size_t start = 0;
        while (start < info.size())
        {
            size_t end = info.find('\n', start);
            if (end == std::string::npos) end = info.size();
            std::string line = info.substr(start, end - start);
            start = end + 1;

            while (!line.empty() && line.back() == '\r') line.pop_back();
            size_t first = line.find_first_not_of(' ');
            if (first == std::string::npos)
                continue;
            line = line.substr(first);
            size_t colon = line.find(": ");
            if (colon != std::string::npos)
            {
                std::string value = line.substr(colon + 2);
                while (!value.empty() && value.back() == ' ') value.pop_back();
                add(replace_tokens(line.substr(0, colon)), replace_tokens(value));
            }
            else if (line.back() == ':')
                group(replace_tokens(line.substr(0, line.size() - 1)));
            else
                add(replace_tokens(line), L"");
        }
    }

    panel.infoTexts.clear();
    panel.infoLines.clear();
    for (const auto &r : rows)
    {
        panel.infoTexts.push_back(r.text);
        const wchar_t *text = panel.infoTexts.back().c_str();
        panel.infoTexts.push_back(r.data);
        const wchar_t *data = panel.infoTexts.back().c_str();
        panel.infoLines.push_back({ text, data, r.separator ? IPLFLAGS_SEPARATOR : IPLFLAGS_NONE });
    }
    panel.infoValid = true;
}

// Does the filesystem have file types to show in the "Type" column?
static bool has_types(PluginPanel &panel)
{
    return static_cast<bool>(panel.filesystem->get_caps() & dsk_tools::FSCaps::Types);
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
    if (!it->second.infoValid)
        build_info(it->second);
    Info->InfoLines        = it->second.infoLines.data();
    Info->InfoLinesNumber  = it->second.infoLines.size();
    Info->DescrFiles       = nullptr;
    Info->DescrFilesNumber = 0;
    // Filesystems with file types (FSCaps::Types: DOS 3.3 B/A/T, ProDOS BIN/BAS, CP/M and FAT
    // attributes...) get a "Type" column (C0, from UniversalFile::type_label) in Full (3) and
    // Wide (4); the other modes are FAR's own (no ColumnTypes)
    static const wchar_t *fullTitles[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    static const wchar_t *wideTitles[] = { nullptr, nullptr, nullptr };
    static PanelMode modes[10] = {};
    fullTitles[1] = wideTitles[1] = msg(MColumnType);
    modes[3] = { L"N,C0,S,D,T", L"0,4,10,0,5", fullTitles, nullptr, nullptr, PMFLAGS_NONE };
    modes[4] = { L"N,C0,S",     L"0,4,0",      wideTitles, nullptr, nullptr, PMFLAGS_NONE };
    const bool types = has_types(it->second);
    Info->PanelModesArray  = types ? modes : nullptr;
    Info->PanelModesNumber = types ? ARRAYSIZE(modes) : 0;
    // Full (name, size, date, time) where the filesystem keeps dates, Wide (name, size) otherwise
    bool dates = static_cast<bool>(it->second.filesystem->get_caps() & dsk_tools::FSCaps::Date);
    Info->StartPanelMode   = dates ? '3' : '4';
    Info->StartSortMode    = SM_UNSORTED;
    Info->StartSortOrder   = 0;
    // F6 only moves here (FAR's label is RenMov): renaming is Shift+F6, see ProcessPanelInputW;
    // in CP/M Alt+F6 moves files into another user area. Two sets: FAR keeps the pointer.
    static KeyBarLabel keyBarLabels[] = {
        { { VK_F6, 0 },                nullptr, nullptr },
        { { VK_F6, LEFT_ALT_PRESSED }, nullptr, nullptr },
    };
    static KeyBarTitles keyBar    = { 1, keyBarLabels };
    static KeyBarTitles keyBarCpm = { 2, keyBarLabels };
    keyBarLabels[0].Text = keyBarLabels[0].LongText = msg(MKeyBarMove);
    keyBarLabels[1].Text = keyBarLabels[1].LongText = msg(MKeyBarArea);
    Info->KeyBar           = it->second.cpmUsers ? &keyBarCpm : &keyBar;
    Info->ShortcutData     = nullptr;
    Info->FreeSize         = it->second.freeSize;
    Info->UserData         = { nullptr, nullptr };
    Info->Instance         = nullptr;
}

extern "C" intptr_t WINAPI GetFindDataW(struct GetFindDataInfo *Info)
{
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end()) return FALSE;

    auto &panel = it->second;
    sync_image(panel, false);   // a redraw or Ctrl+R after the other panel saved the image
    size_t count = panel.files.size();
    if (count == 0)
    {
        Info->PanelItem   = nullptr;
        Info->ItemsNumber = 0;
        return TRUE;
    }

    auto *items = new PluginPanelItem[count];
    memset(items, 0, sizeof(PluginPanelItem) * count);
    const bool types = has_types(panel);

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
        items[i].Description       = nullptr;
        items[i].Owner             = nullptr;
        items[i].CustomColumnData  = nullptr;
        items[i].CustomColumnNumber = 0;
        if (types)
        {
            // The "Type" column (C0); freed with the name in FreeFindDataW
            std::wstring label = to_wide(uf.type_label);
            auto *text = new wchar_t[label.size() + 1];
            wmemcpy(text, label.c_str(), label.size() + 1);
            items[i].CustomColumnData   = new const wchar_t *[1] { text };
            items[i].CustomColumnNumber = 1;
        }
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
    {
        const auto &item = Info->PanelItem[i];
        delete[] item.FileName;
        for (size_t c = 0; c < item.CustomColumnNumber; ++c)
            delete[] item.CustomColumnData[c];
        delete[] item.CustomColumnData;
    }
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
    // PSI.Message shows one item a line: split the lines on '\n' as show_error() does
    std::vector<std::wstring> parts;
    for (const auto &l : lines)
        for (size_t start = 0;;)
        {
            size_t end = l.find(L'\n', start);
            parts.push_back(l.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }
    std::vector<const wchar_t *> items;
    items.push_back(L"DskTools");
    for (const auto &p : parts)
        items.push_back(p.c_str());
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
    // Copy dialog options for extraction
    bool readOnly     = false;  // protected files get the read-only attribute
    bool flatAreas    = false;  // CP/M user areas go into the destination itself, not USER_nn
    std::set<std::wstring> flatNames;   // name_key()s used there by this operation
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

    if (!cpm_blocks_ok(ctx.panel, uf))
        return op_error(ctx, outPath, msg(MDamagedEntry));
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
    if (ctx.readOnly && uf.is_protected)
        SetFileAttributesW(outPath.c_str(), FILE_ATTRIBUTE_READONLY);
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
// or, with OpContext::flatAreas, into destDir itself: a name already taken there by this
// operation (another area, a selected file) gets "~2"
static Outcome extract_cpm_area(OpContext &ctx, unsigned user, const std::wstring &destDir)
{
    dsk_tools::Files area;
    for (const auto &f : ctx.panel.cpmAll)
        if (!f.is_dir && cpm_user(f) == user)
            area.push_back(f);

    if (ctx.flatAreas)
    {
        std::vector<std::wstring> names;
        for (const auto &f : area)
            names.push_back(unique_name(sanitize_name(to_wide(f.name)), false, ctx.flatNames));
        return extract_list(ctx, area, names, destDir);
    }

    std::wstring dirPath = join_path(destDir, cpm_user_dir(user));
    Outcome o = make_dir(ctx, dirPath);
    if (o != Outcome::Ok)
        return o;
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

// A file shorter than the disk (its empty end cut off; the loader filled it with E5) stays
// short: it is written up to the last byte that is not E5, rounded up to a track, and never
// shorter than it was. What is left out is E5 alone, which the loader puts back.
static void keep_short(const PluginPanel &panel, dsk_tools::BYTES &buffer)
{
    if (panel.diskSize == 0 || panel.diskSize >= buffer.size())
        return;
    cpm_geometry::Geometry g;
    size_t track = geometry_of_type(panel.typeId, g) ? g.sectrk * g.seclen : 0;
    if (track == 0)
        return;
    size_t keep = buffer.size();
    while (keep > panel.diskSize && buffer[keep - 1] == 0xE5)
        --keep;
    if (keep > panel.diskSize)
    {
        keep = (keep + track - 1) / track * track;
        if (keep > buffer.size())
            keep = buffer.size();
    }
    else
        keep = static_cast<size_t>(panel.diskSize);
    buffer.resize(keep);
}

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
    keep_short(panel, buffer);

    // The image as it was before the first change made in this panel
    if (g_Settings.backup && !panel.backedUp)
    {
        std::wstring bak = panel.hostPath + L".bak";
        if (!CopyFileW(panel.hostPath.c_str(), bak.c_str(), FALSE))
            return msg(MCannotBackup) + (L"\n" + bak + L"\n" + last_error_text(GetLastError()));
        panel.backedUp = true;
    }

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
    ++g_SaveCounts[name_key(panel.hostPath)];
    remember_stamp(panel);
    set_cpm_bounds(panel);      // a short file may have grown
    panel.infoValid = false;    // free space, file count
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

// ── The image file changed on disk ─────────────────────────────
// The same image may be open in both panels (or be written by another program), and every
// panel holds its own copy in memory: one saving over the other's changes would lose them
// (F6 between two panels of one image: the target saves the files, then the source saves
// its copy without them). So before using its copy a panel checks that the file is as it
// loaded or saved it last, and reloads it otherwise.

static bool file_stamp(const std::wstring &path, FILETIME &time, unsigned long long &size)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    time = fad.ftLastWriteTime;
    size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return true;
}

static void remember_stamp(PluginPanel &panel)
{
    if (!file_stamp(panel.hostPath, panel.diskTime, panel.diskSize))
        panel.diskSize = 0;
    panel.saveCount = g_SaveCounts[name_key(panel.hostPath)];
}

// Reloads the image (back in the same directory) if the file is not as this panel left it
static void sync_image(PluginPanel &panel, bool report)
{
    FILETIME time;
    unsigned long long size;
    if (!file_stamp(panel.hostPath, time, size))
        return;                 // gone: the operation itself reports it
    if (CompareFileTime(&time, &panel.diskTime) != 0 || size != panel.diskSize
        || g_SaveCounts[name_key(panel.hostPath)] != panel.saveCount)
        discard_changes(panel, report);
}

// Index in panel.files of the item named `name` (as the panel shows it), -1 if none
static intptr_t name_index(const PluginPanel &panel, const std::wstring &name)
{
    for (size_t i = 0; i < panel.wideNames.size(); ++i)
        if (panel.wideNames[i] == name)
            return static_cast<intptr_t>(i);
    return -1;
}

// Index in panel.files of an item FAR passed back: its UserData while the name there
// still matches, otherwise (the image was reloaded since FAR got the list) by name
static intptr_t item_index(const PluginPanel &panel, const PluginPanelItem &item)
{
    const std::wstring name = item.FileName ? item.FileName : L"";
    intptr_t idx = reinterpret_cast<intptr_t>(item.UserData.Data);
    if (idx >= 0 && static_cast<size_t>(idx) < panel.wideNames.size() && panel.wideNames[idx] == name)
        return idx;
    return name_index(panel, name);
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

// Why `name` cannot be a CP/M file name, empty if it can. fsCPM checks the 8.3 lengths only
// (in bytes of UTF-8) and copies the bytes as they are: CCP cannot reach a file named with a
// space or one of the characters it parses, and the high bit of a name byte is an attribute
// (R/O, SYS, archive), so nothing but ASCII can be written. Cyrillic is refused, not recoded:
// KOI-8 sets that bit, and the KOI-7 of Korvet and Orion would go through fsCPM's to_upper.
static std::wstring cpm_name_error(const std::wstring &name)
{
    static const wchar_t parsed[] = L"<>.,;:=?*[]|";
    const auto dot = name.find(L'.');
    const std::wstring base = name.substr(0, dot);
    const std::wstring ext  = dot == std::wstring::npos ? L"" : name.substr(dot + 1);
    bool ok = !base.empty() && base.size() <= 8 && ext.size() <= 3;
    for (const std::wstring *part : { &base, &ext })
        for (wchar_t c : *part)
            if (c <= L' ' || c > L'~' || wcschr(parsed, c))
                ok = false;
    if (ok)
        return L"";
    return msg(MInvalidName, { fs_name(dsk_tools::FS::CPM) }) + (L"\n" + std::wstring(msg(MCpmNameRule)));
}

static std::wstring name_error(PluginPanel &panel, const dsk_tools::Result &r, const wchar_t *what)
{
    if (r.code == dsk_tools::ErrorCode::InvalidName)
        return msg(MInvalidName, { fs_name(panel.filesystem->get_fs()) });
    return describe(what, r);
}

// Makes way for a file `name` in the current directory: Ok when it is free or is to be
// overwritten (`replace`; on DOS 3.3 the old file is deleted here: its put_file neither
// detects nor replaces one)
static Outcome make_room(OpContext &ctx, const std::wstring &name, bool &replace)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;
    replace = false;
    dsk_tools::UniversalFile existing;
    if (!find_entry(panel, name, existing))
        return Outcome::Ok;
    if (existing.is_dir)
        return op_error(ctx, name, msg(MDirExists));
    Outcome o = confirm_overwrite(ctx, name);
    if (o != Outcome::Ok)
        return o;
    replace = true;
    if (fs.get_fs() == dsk_tools::FS::DOS33)
    {
        auto r = fs.delete_file(existing);
        if (!r)
            return fails_before_change(r) ? op_error(ctx, name, describe(msg(MCannotReplace), r))
                                          : op_broken(ctx, name, describe(msg(MCannotReplace), r));
        ctx.changed = true;
    }
    return Outcome::Ok;
}

// put_file format for plain bytes
static std::string add_format(dsk_tools::fileSystem &fs)
{
    std::string format = "FILE_BINARY";
    for (const auto &f : fs.get_add_file_formats())
        if (f == "FILE_ANY")
            format = f;
    return format;
}

// Copies one host file into the current directory of the image
static Outcome put_file(OpContext &ctx, const std::wstring &srcPath, const std::wstring &name)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;

    if (fs.get_fs() == dsk_tools::FS::CPM)
    {
        std::wstring bad = cpm_name_error(name);
        if (!bad.empty())
            return op_error(ctx, name, bad);
    }

    dsk_tools::BYTES data;
    std::wstring error;
    if (!read_host_file(srcPath, data, error))
        return op_error(ctx, srcPath, error);

    bool replace = false;
    Outcome o = make_room(ctx, name, replace);
    if (o != Outcome::Ok)
        return o;

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

    auto r = fs.put_file(uf, add_format(fs), data, replace);
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
// destination as the user left it. The options start from their defaults every time
// (Ask, no read-only, areas in USER_nn), as in FAR's own dialogs.
struct CopyOptions
{
    ExistingFiles existing  = ExistingFiles::Ask;
    bool          readOnly  = false;
    bool          flatAreas = false;
};

static bool copy_dialog(const GetFilesInfo *Info, PluginPanel &panel, std::wstring &dest, CopyOptions &options)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    static const UUID DialogUuid = COPY_DIALOG_UUID;
    const bool move = Info->Move != 0;

    std::wstring prompt;
    if (Info->ItemsNumber == 1)
    {
        intptr_t idx = item_index(panel, Info->PanelItem[0]);
        std::wstring name = idx >= 0 ? panel.wideNames[idx]
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
    builder.AddCheckbox(MCopyReadOnly, &options.readOnly);

    // Only when a CP/M user area is among the items
    bool areas = false;
    for (size_t i = 0; panel.cpmUsers && i < Info->ItemsNumber; ++i)
    {
        intptr_t idx = item_index(panel, Info->PanelItem[i]);
        areas |= idx >= 0 && panel.files[idx].is_dir;
    }
    if (areas)
        builder.AddCheckbox(MCopyFlatAreas, &options.flatAreas);
    builder.AddOKCancel(move ? MMoveButton : MCopyButton, MDialogCancel);

    if (!builder.ShowDialog())
        return false;
    options.existing = static_cast<ExistingFiles>(choice);
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
    sync_image(panel, !silent);

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
        CopyOptions options;
        if (!copy_dialog(Info, panel, typed, options))
            return -1;
        std::wstring dest = resolve_dest(typed, destDir);

        // As in FAR: a single file goes to a path that is not a folder under that name
        bool toFolder = Info->ItemsNumber != 1 || is_host_dir(dest) || typed.empty()
                     || typed.back() == L'\\' || typed.back() == L'/';
        if (!toFolder)
        {
            intptr_t idx = item_index(panel, Info->PanelItem[0]);
            toFolder = idx < 0 || panel.files[idx].is_dir;
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
        ctx.overwriteAll = options.existing == ExistingFiles::Overwrite;
        ctx.skipExisting = options.existing == ExistingFiles::Skip;
        ctx.readOnly     = options.readOnly;
        ctx.flatAreas    = options.flatAreas;

        // Areas copied into one folder must not take the names of the selected files
        for (size_t i = 0; ctx.flatAreas && i < Info->ItemsNumber; ++i)
        {
            intptr_t idx = item_index(panel, Info->PanelItem[i]);
            if (idx >= 0 && !panel.files[idx].is_dir)
                ctx.flatNames.insert(name_key(renameTo.empty() ? panel.wideNames[idx] : renameTo));
        }
    }

    std::vector<size_t> extracted;
    bool cancelled = false;
    for (size_t i = 0; i < Info->ItemsNumber && !cancelled; ++i)
    {
        auto &item = Info->PanelItem[i];
        intptr_t idx = item_index(panel, item);
        if (idx < 0)
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
            intptr_t idx = item_index(panel, Info->PanelItem[i]);
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
    sync_image(panel, !silent);
    if (!check_writable(panel, !silent, dsk_tools::FSCaps::Delete, MOpDeleting))
        return FALSE;

    // Copies: the listing is re-read only after the whole operation
    Victims victims;
    for (size_t i = 0; i < Info->ItemsNumber; ++i)
    {
        intptr_t idx = item_index(panel, Info->PanelItem[i]);
        if (idx >= 0)
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
    sync_image(panel, !silent);
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
    sync_image(panel, !silent);
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

// ── Rename (Shift+F6) ──────────────────────────────────────────
// Without OPIF_REALNAMES FAR treats Shift+F6 on a plugin panel as F6 (FileList::ProcessKey →
// ProcessCopyKeys), so the plugin takes the key in ProcessPanelInputW and renames the
// current item itself.

// Name of the current panel item, empty if none
static std::wstring current_item(HANDLE hPanel, intptr_t &idx)
{
    size_t size = static_cast<size_t>(PSI.PanelControl(hPanel, FCTL_GETCURRENTPANELITEM, 0, nullptr));
    if (!size) return {};
    std::vector<char> buf(size);
    FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), size, reinterpret_cast<PluginPanelItem *>(buf.data()) };
    if (!PSI.PanelControl(hPanel, FCTL_GETCURRENTPANELITEM, 0, &gpi) || !gpi.Item->FileName)
        return {};
    idx = reinterpret_cast<intptr_t>(gpi.Item->UserData.Data);
    return gpi.Item->FileName;
}

// Re-reads the panel and puts the cursor on `name`; the other panel is re-read as well,
// as FAR does after its own file operations (it may show the same image)
static void update_panel_cursor(HANDLE hPanel, const std::wstring &name)
{
    PSI.PanelControl(PANEL_PASSIVE, FCTL_UPDATEPANEL, 1, nullptr);
    PSI.PanelControl(PANEL_PASSIVE, FCTL_REDRAWPANEL, 0, nullptr);
    PSI.PanelControl(hPanel, FCTL_UPDATEPANEL, 1, nullptr);
    PanelInfo pi = {};
    pi.StructSize = sizeof(PanelInfo);
    if (!PSI.PanelControl(hPanel, FCTL_GETPANELINFO, 0, &pi))
        return;
    for (size_t i = 0; i < pi.ItemsNumber; ++i)
    {
        size_t size = static_cast<size_t>(PSI.PanelControl(hPanel, FCTL_GETPANELITEM, i, nullptr));
        if (!size) continue;
        std::vector<char> buf(size);
        FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), size, reinterpret_cast<PluginPanelItem *>(buf.data()) };
        if (PSI.PanelControl(hPanel, FCTL_GETPANELITEM, i, &gpi) && gpi.Item->FileName
            && name == gpi.Item->FileName)
        {
            PanelRedrawInfo ri = { sizeof(PanelRedrawInfo), i, pi.TopPanelItem };
            PSI.PanelControl(hPanel, FCTL_REDRAWPANEL, 0, &ri);
            return;
        }
    }
    PSI.PanelControl(hPanel, FCTL_REDRAWPANEL, 0, nullptr);
}

static void rename_item(HANDLE hPanel, PluginPanel &panel)
{
    sync_image(panel, true);
    intptr_t idx = -1;
    std::wstring oldName = current_item(hPanel, idx);
    idx = oldName == L".." ? -1 : name_index(panel, oldName);
    if (idx < 0)
        return;
    const dsk_tools::UniversalFile uf = panel.files[idx];

    if (uf.is_dir && panel.cpmUsers)
    {
        show_error(msg(MRenameArea));
        return;
    }
    if (!check_writable(panel, true, dsk_tools::FSCaps::Rename, MOpRenaming))
        return;

    static const UUID PluginUuid = PLUGIN_UUID;
    wchar_t buf[256] = {};
    if (!PSI.InputBox(&PluginUuid, nullptr, msg(MRenameTitle), msg(MRenameTo, { L"\"" + oldName + L"\"" }).c_str(),
                      L"DskTools.Rename", oldName.c_str(), buf, ARRAYSIZE(buf),
                      nullptr, FIB_BUTTONS | FIB_NOUSELASTHISTORY))
        return;
    std::wstring name = buf;
    auto first = name.find_first_not_of(L' ');
    auto last  = name.find_last_not_of(L' ');
    name = (first == std::wstring::npos) ? L"" : name.substr(first, last - first + 1);
    if (name.empty() || name == oldName)
        return;

    auto fail = [&](const std::wstring &reason)
    {
        show_error(msg(MCannotRename) + (L"\n" + oldName + L"\n" + reason));
    };

    // The libraries of CP/M and DOS 3.3 do not look for a clash, DOS 3.3 cuts a long name.
    // A new name differing in case only is the same file.
    dsk_tools::UniversalFile existing;
    if (_wcsicmp(name.c_str(), to_wide(uf.name).c_str()) != 0 && find_entry(panel, name, existing))
    {
        fail(msg(existing.is_dir ? MDirExists : MFileWithNameExists));
        return;
    }
    if (panel.filesystem->get_fs() == dsk_tools::FS::DOS33 && name.size() > 30)
    {
        fail(msg(MInvalidName, { fs_name(dsk_tools::FS::DOS33) }));
        return;
    }
    if (panel.filesystem->get_fs() == dsk_tools::FS::CPM && !cpm_name_error(name).empty())
    {
        fail(cpm_name_error(name));
        return;
    }

    auto r = panel.filesystem->rename_file(uf, wide_to_utf8(name));
    if (!r)
    {
        fail(r.code == dsk_tools::ErrorCode::InvalidName ? msg(MInvalidName, { fs_name(panel.filesystem->get_fs()) })
             : r.code == dsk_tools::ErrorCode::FileAlreadyExists ? std::wstring(msg(MFileWithNameExists))
             : error_text(r));
        discard_changes(panel, true);   // nothing may be half renamed in memory
        update_panel_cursor(hPanel, oldName);
        return;
    }
    if (!commit_changes(panel, true))
    {
        update_panel_cursor(hPanel, oldName);
        return;
    }

    // The filesystem may have changed the name (upper case, 8.3): show what it holds now
    std::wstring shown = name;
    for (const auto &n : panel.wideNames)
        if (_wcsicmp(n.c_str(), name.c_str()) == 0)
            shown = n;
    update_panel_cursor(hPanel, shown);
}

// ── Copy in the image (Shift+F5) ───────────────────────────────
// Like Shift+F6, FAR turns Shift+F5 into F5 on this panel. Here it copies the current
// item into the same directory of the image under a new name. The source is read into
// memory first, so a folder copied next to itself never meets its copy.

struct TreeNode
{
    dsk_tools::UniversalFile uf;
    dsk_tools::BYTES         data;      // a file's contents
    std::vector<TreeNode>    kids;      // a directory's entries
};

static Outcome read_node(OpContext &ctx, TreeNode &node, const std::wstring &name)
{
    auto &fs = *ctx.panel.filesystem;
    if (!node.uf.is_dir)
    {
        if (!cpm_blocks_ok(ctx.panel, node.uf))
            return op_error(ctx, name, msg(MDamagedEntry));
        auto r = fs.get_file(node.uf, "FILE_BINARY", node.data);
        return r ? Outcome::Ok : op_error(ctx, name, describe(msg(MCannotReadFile), r));
    }

    fs.cd(node.uf);
    dsk_tools::Files files;
    auto r = list_dir(fs, files);
    Outcome result = r ? Outcome::Ok : op_error(ctx, name, describe(msg(MCannotReadDirectory), r));
    auto names = build_wide_names(files);
    for (size_t i = 0; r && i < files.size(); ++i)
    {
        TreeNode kid{ files[i], {}, {} };
        Outcome o = read_node(ctx, kid, names[i]);
        if (o == Outcome::Cancelled)
        {
            result = o;
            break;
        }
        if (o == Outcome::Skipped)
            result = o;
        else
            node.kids.push_back(std::move(kid));
    }
    fs.cd_up();
    return result;
}

// The source entry as put_file/mkdir take it for a copy named `name`: attributes, dates and
// the CP/M user come from its metadata. Under a new name without original_name, from which
// FAT and RT-11 would take the old one; entries inside a copied folder keep it and so
// their names byte for byte.
static dsk_tools::UniversalFile copy_proto(const dsk_tools::UniversalFile &src, const std::wstring &name)
{
    dsk_tools::UniversalFile uf = src;
    if (wide_to_utf8(name) != src.name)
    {
        uf.name = wide_to_utf8(name);
        uf.original_name.clear();
    }
    return uf;
}

static Outcome write_node(OpContext &ctx, const TreeNode &node, const std::wstring &name, bool replace);

static Outcome write_kids(OpContext &ctx, const std::vector<TreeNode> &kids)
{
    Outcome result = Outcome::Ok;
    for (const auto &kid : kids)
    {
        Outcome o = write_node(ctx, kid, to_wide(kid.uf.name), false);
        if (o == Outcome::Cancelled)
            return o;
        if (o == Outcome::Skipped)
            result = o;
    }
    return result;
}

// Writes `node` into the current directory as `name`. On DOS 3.3 a file keeps its type only
// through put_file's native mode (no format, a DOS 3.3 source), which takes the name from
// the source entry: the copy gets the source's name (copy_item() then renames the source).
static Outcome write_node(OpContext &ctx, const TreeNode &node, const std::wstring &name, bool replace)
{
    auto &panel = ctx.panel;
    auto &fs = *panel.filesystem;
    const bool dos33 = fs.get_fs() == dsk_tools::FS::DOS33;

    if (!node.uf.is_dir)
    {
        auto r = dos33 ? fs.put_file(node.uf, "", node.data, false)
                       : fs.put_file(copy_proto(node.uf, name), add_format(fs), node.data, replace);
        if (!r)
            return fails_before_change(r) ? op_error(ctx, name, name_error(panel, r, msg(MCannotWrite)))
                                          : op_broken(ctx, name, name_error(panel, r, msg(MCannotWrite)));
        ctx.changed = true;
        return Outcome::Ok;
    }

    // DOS 3.3 mkdir in native mode takes the source's name from the metadata: give it the new one
    dsk_tools::UniversalFile proto = copy_proto(node.uf, name), created, dir;
    if (dos33)
    {
        proto = dsk_tools::UniversalFile{};
        proto.fs   = dsk_tools::FS::None;
        proto.name = wide_to_utf8(name);
    }
    auto r = fs.mkdir(proto, created);
    if (!r)
        return fails_before_change(r) ? op_error(ctx, name, name_error(panel, r, msg(MCannotCreateDir)))
                                      : op_broken(ctx, name, name_error(panel, r, msg(MCannotCreateDir)));
    ctx.changed = true;
    if (!find_entry(panel, name, dir))
        return op_broken(ctx, name, msg(MCannotFindNewDir));
    fs.cd(dir);
    Outcome o = write_kids(ctx, node.kids);
    fs.cd_up();
    return o;
}

static void copy_item(HANDLE hPanel, PluginPanel &panel)
{
    sync_image(panel, true);
    intptr_t idx = -1;
    std::wstring oldName = current_item(hPanel, idx);
    idx = oldName == L".." ? -1 : name_index(panel, oldName);
    if (idx < 0)
        return;
    const dsk_tools::UniversalFile uf = panel.files[idx];
    auto &fs = *panel.filesystem;
    const bool dos33 = fs.get_fs() == dsk_tools::FS::DOS33;

    if (uf.is_dir && panel.cpmUsers)
    {
        show_error(msg(MCopyArea));
        return;
    }
    if (!check_writable(panel, true, uf.is_dir ? dsk_tools::FSCaps::MkDir : dsk_tools::FSCaps::Add,
                        uf.is_dir ? MOpDirectories : MOpAdding))
        return;

    static const UUID PluginUuid = PLUGIN_UUID;
    wchar_t buf[256] = {};
    if (!PSI.InputBox(&PluginUuid, nullptr, msg(MCopyTitle), msg(MCopyTo, { L"\"" + oldName + L"\"" }).c_str(),
                      L"DskTools.Copy", oldName.c_str(), buf, ARRAYSIZE(buf),
                      nullptr, FIB_BUTTONS | FIB_NOUSELASTHISTORY))
        return;
    std::wstring name = buf;
    auto first = name.find_first_not_of(L' ');
    auto last  = name.find_last_not_of(L' ');
    name = (first == std::wstring::npos) ? L"" : name.substr(first, last - first + 1);
    if (name.empty())
        return;

    OpContext ctx{ panel, true, true, msg(MCannotCopy) };
    if (name == oldName || _wcsicmp(name.c_str(), to_wide(uf.name).c_str()) == 0)
    {
        show_error(std::wstring(ctx.action) + L"\n" + oldName + L"\n" + msg(MCopySelf));
        return;
    }
    if (dos33 && name.size() > 30)
    {
        show_error(std::wstring(ctx.action) + L"\n" + name + L"\n" + msg(MInvalidName, { fs_name(dsk_tools::FS::DOS33) }));
        return;
    }
    if (panel.filesystem->get_fs() == dsk_tools::FS::CPM && !cpm_name_error(name).empty())
    {
        show_error(std::wstring(ctx.action) + L"\n" + name + L"\n" + cpm_name_error(name));
        return;
    }

    TreeNode node{ uf, {}, {} };
    Outcome o = read_node(ctx, node, oldName);
    if (o == Outcome::Cancelled || (o == Outcome::Skipped && !uf.is_dir))
        return;

    bool replace = false;
    dsk_tools::UniversalFile existing;
    if (uf.is_dir)
        o = find_entry(panel, name, existing)
          ? op_error(ctx, name, msg(existing.is_dir ? MDirExists : MFileWithNameExists))
          : Outcome::Ok;
    else
        o = make_room(ctx, name, replace);
    if (o == Outcome::Ok)
        o = write_node(ctx, node, name, replace);

    // DOS 3.3: the copy has the old name, so the source takes the new one
    if (o == Outcome::Ok && dos33 && !uf.is_dir)
    {
        auto r = fs.rename_file(uf, wide_to_utf8(name));
        if (!r)
            op_broken(ctx, name, error_text(r));
    }

    if (ctx.broken)
    {
        discard_changes(panel, true);
        update_panel_cursor(hPanel, oldName);
        return;
    }
    if (!ctx.changed)
        return;
    if (!commit_changes(panel, true))
    {
        update_panel_cursor(hPanel, oldName);
        return;
    }
    std::wstring shown = name;
    for (const auto &n : panel.wideNames)
        if (_wcsicmp(n.c_str(), name.c_str()) == 0)
            shown = n;
    update_panel_cursor(hPanel, shown);
}

// ── CP/M: F6 between two panels of one image ───────────────────
// FAR would move the files through a temporary folder (GetFiles + PutFiles + DeleteFiles):
// the attributes get lost, names that Windows or fsCPM::put_file cannot take come back
// changed (Cyrillic ones: put_file does not apply the charmap), and the disk needs room for
// a copy. Here only the user number (ST) of the directory entries changes instead
// (file_set_metadata("user")); data, names and attributes stay as they are.

// The other panel when it shows the same image file in DskTools, else nullptr
static PluginPanel *twin_panel(const PluginPanel &panel)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    PanelInfo pi = {};
    pi.StructSize = sizeof(PanelInfo);
    if (!PSI.PanelControl(PANEL_PASSIVE, FCTL_GETPANELINFO, 0, &pi)
        || !(pi.Flags & PFLAGS_PLUGIN) || !(pi.Flags & PFLAGS_VISIBLE)
        || memcmp(&pi.OwnerGuid, &PluginUuid, sizeof(UUID)) != 0)
        return nullptr;
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(pi.PluginHandle));
    if (it == g_Panels.end() || &it->second == &panel
        || name_key(it->second.hostPath) != name_key(panel.hostPath))
        return nullptr;
    return &it->second;
}

// Names of the selected items, or of the one under the cursor when none is selected
static std::vector<std::wstring> selected_names(HANDLE hPanel)
{
    std::vector<std::wstring> names;
    PanelInfo pi = {};
    pi.StructSize = sizeof(PanelInfo);
    if (!PSI.PanelControl(hPanel, FCTL_GETPANELINFO, 0, &pi))
        return names;
    for (size_t i = 0; i < pi.SelectedItemsNumber; ++i)
    {
        size_t size = static_cast<size_t>(PSI.PanelControl(hPanel, FCTL_GETSELECTEDPANELITEM, i, nullptr));
        if (!size) continue;
        std::vector<char> buf(size);
        FarGetPluginPanelItem gpi = { sizeof(FarGetPluginPanelItem), size, reinterpret_cast<PluginPanelItem *>(buf.data()) };
        if (PSI.PanelControl(hPanel, FCTL_GETSELECTEDPANELITEM, i, &gpi) && gpi.Item->FileName
            && wcscmp(gpi.Item->FileName, L"..") != 0)
            names.push_back(gpi.Item->FileName);
    }
    return names;
}

using AreaItems = std::vector<std::pair<dsk_tools::UniversalFile, std::wstring>>;

// The selected files of a CP/M panel (or the one under the cursor); a USER_nn among them is
// refused with a message
static AreaItems area_move_items(HANDLE hPanel, PluginPanel &panel)
{
    AreaItems items;
    bool areas = false;
    for (const auto &name : selected_names(hPanel))
    {
        intptr_t idx = name_index(panel, name);
        if (idx < 0)
            continue;
        if (panel.files[idx].is_dir)
            areas = true;
        else
            items.emplace_back(panel.files[idx], name);
    }
    if (areas)
        show_error(msg(MMoveArea));
    return items;
}

// "\"NAME\"" for one item, "N items" for more
static std::wstring area_items_text(const AreaItems &items)
{
    return items.size() == 1 ? L"\"" + items.front().second + L"\"" : items_text(items.size());
}

// Moves `items` into user area `target`: a file of the same name there is replaced after the
// overwrite question; then the image is saved and both panels re-read
static void move_items_to_area(HANDLE hPanel, PluginPanel &panel, const AreaItems &items, unsigned target)
{
    auto &fs = *panel.filesystem;
    OpContext ctx{ panel, true, true, msg(MCannotMove) };
    for (const auto &item : items)
    {
        const auto &uf = item.first;
        const auto &name = item.second;

        const dsk_tools::UniversalFile *existing = nullptr;
        for (const auto &f : panel.cpmAll)
            if (!f.is_dir && cpm_user(f) == target && _wcsicmp(to_wide(f.name).c_str(), to_wide(uf.name).c_str()) == 0)
                existing = &f;
        if (existing)
        {
            Outcome o = confirm_overwrite(ctx, name);
            if (o == Outcome::Cancelled)
                break;
            if (o == Outcome::Skipped)
                continue;
            auto r = fs.delete_file(*existing);
            if (!r)
            {
                if (fails_before_change(r))
                {
                    if (op_error(ctx, name, describe(msg(MCannotReplace), r)) == Outcome::Cancelled)
                        break;
                    continue;
                }
                op_broken(ctx, name, describe(msg(MCannotReplace), r));
                break;
            }
            ctx.changed = true;
        }

        auto r = fs.file_set_metadata(uf, { { "user", std::to_string(target) } });
        if (!r)
        {
            op_broken(ctx, name, error_text(r));
            break;
        }
        ctx.changed = true;
    }

    if (ctx.broken)
        discard_changes(panel, true);
    else if (ctx.changed)
        commit_changes(panel, true);

    // The moved files leave this panel; the other one may show their new area
    PSI.PanelControl(hPanel, FCTL_UPDATEPANEL, 1, nullptr);
    PSI.PanelControl(hPanel, FCTL_REDRAWPANEL, 0, nullptr);
    PSI.PanelControl(PANEL_PASSIVE, FCTL_UPDATEPANEL, 1, nullptr);
    PSI.PanelControl(PANEL_PASSIVE, FCTL_REDRAWPANEL, 0, nullptr);
}

// F6 with the same CP/M image in the other panel: moves the selected files into the user
// area shown there. Returns false to leave the key to FAR (not that case).
static bool move_to_twin_area(HANDLE hPanel, PluginPanel &panel)
{
    if (!panel.cpmUsers)
        return false;
    PluginPanel *twin = twin_panel(panel);
    if (!twin)
        return false;
    const unsigned target = twin->cpmUser;

    sync_image(panel, true);
    if (target == panel.cpmUser)
    {
        show_error(msg(MSameArea));
        return true;
    }
    if (!check_writable(panel, true, dsk_tools::FSCaps::Metadata, MOpUserArea))
        return true;
    AreaItems items = area_move_items(hPanel, panel);
    if (items.empty())
        return true;

    if (far_confirmation(L"Move")
        && ask({ msg(MMoveToArea, { area_items_text(items), std::to_wstring(target) }) },
               { msg(MMoveButton), msg(MCancel) }) != 0)
        return true;
    move_items_to_area(hPanel, panel, items, target);
    return true;
}

// Alt+F6 in a CP/M image: moves the selected files into the user area picked from a list
// (a list rather than a typed number or path: nothing can be mistyped)
static void move_to_chosen_area(HANDLE hPanel, PluginPanel &panel)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    static const UUID DialogUuid = AREA_DIALOG_UUID;

    sync_image(panel, true);
    if (!check_writable(panel, true, dsk_tools::FSCaps::Metadata, MOpUserArea))
        return;
    AreaItems items = area_move_items(hPanel, panel);
    if (items.empty())
        return;

    // The area of the other panel when it shows this image, else the root (or USER_01 there)
    PluginPanel *twin = twin_panel(panel);
    int choice = twin && twin->cpmUser != panel.cpmUser ? static_cast<int>(twin->cpmUser)
               : panel.cpmUser != 0 ? 0 : 1;

    std::vector<std::wstring> texts;
    texts.push_back(msg(MAreaRoot));
    for (unsigned u = 1; u <= CPM_MAX_USER; ++u)
        texts.push_back(cpm_user_dir(u));
    std::vector<const wchar_t *> list;
    for (const auto &t : texts)
        list.push_back(t.c_str());

    std::wstring prompt = msg(MMoveToAreaPrompt, { area_items_text(items) });
    PluginDialogBuilder builder(PSI, PluginUuid, DialogUuid, MMoveTitle, nullptr);
    builder.AddText(prompt.c_str());
    builder.AddComboBox(&choice, nullptr, 24, list.data(), list.size(), DIF_DROPDOWNLIST);
    builder.AddOKCancel(MMoveButton, MDialogCancel);
    if (!builder.ShowDialog())
        return;

    const unsigned target = static_cast<unsigned>(choice);
    if (target == panel.cpmUser)
    {
        show_error(msg(MSameArea));
        return;
    }
    move_items_to_area(hPanel, panel, items, target);
}

static bool is_shift_key(const INPUT_RECORD &rec, WORD key)
{
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown || rec.Event.KeyEvent.wVirtualKeyCode != key)
        return false;
    const DWORD state = rec.Event.KeyEvent.dwControlKeyState;
    return (state & SHIFT_PRESSED)
        && !(state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
}

static bool is_alt_key(const INPUT_RECORD &rec, WORD key)
{
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown || rec.Event.KeyEvent.wVirtualKeyCode != key)
        return false;
    const DWORD state = rec.Event.KeyEvent.dwControlKeyState;
    return (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))
        && !(state & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));
}

static bool is_plain_key(const INPUT_RECORD &rec, WORD key)
{
    return rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.wVirtualKeyCode == key
        && !(rec.Event.KeyEvent.dwControlKeyState
             & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
}

extern "C" intptr_t WINAPI ProcessPanelInputW(const struct ProcessPanelInputInfo *Info)
{
    if (is_plain_key(Info->Rec, VK_F6))
    {
        auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
        return it != g_Panels.end() && move_to_twin_area(Info->hPanel, it->second) ? TRUE : FALSE;
    }
    if (is_alt_key(Info->Rec, VK_F6))
    {
        // FAR does nothing with Alt+F6 (hard link) on a plugin panel
        auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
        if (it == g_Panels.end() || !it->second.cpmUsers)
            return FALSE;
        move_to_chosen_area(Info->hPanel, it->second);
        return TRUE;
    }

    const bool rename = is_shift_key(Info->Rec, VK_F6);
    if (!rename && !is_shift_key(Info->Rec, VK_F5))
        return FALSE;
    auto it = g_Panels.find(reinterpret_cast<intptr_t>(Info->hPanel));
    if (it == g_Panels.end())
        return FALSE;
    if (rename)
        rename_item(Info->hPanel, it->second);
    else
        copy_item(Info->hPanel, it->second);
    return TRUE;    // never let FAR turn it into F5/F6
}

extern "C" void WINAPI ClosePanelW(const struct ClosePanelInfo *Info)
{
    g_Panels.erase(reinterpret_cast<intptr_t>(Info->hPanel));
}

// ── Settings dialog ────────────────────────────────────────────

extern "C" intptr_t WINAPI ConfigureW(const struct ConfigureInfo * /*Info*/)
{
    static const UUID PluginUuid = PLUGIN_UUID;
    static const UUID DialogUuid = CONFIG_DIALOG_UUID;

    Settings edited = g_Settings;
    for (;;)
    {
        PluginDialogBuilder builder(PSI, PluginUuid, DialogUuid, L"DskTools", nullptr);
        builder.AddCheckbox(MCfgOpenOnEnter, &edited.openOnEnter);
        builder.AddText(MCfgMask);
        builder.AddEditField(edited.mask, 52, L"Masks");
        builder.AddText(MCfgFilesystems);
        builder.StartColumns();
        for (size_t i = 0; i < FS_COUNT; ++i)
        {
            if (i == FS_LEFT_COLUMN)
                builder.ColumnBreak();
            builder.AddCheckbox(FS_SWITCHES[i].label, &edited.openFs[i]);
        }
        builder.EndColumns();
        builder.AddSeparator();
        builder.AddCheckbox(MCfgAllowWrite, &edited.allowWrite);
        builder.AddCheckbox(MCfgBackup, &edited.backup);
        builder.AddOKCancel(MOk, MDialogCancel);
        if (!builder.ShowDialog())
            return FALSE;

        if (edited.mask.find_first_not_of(L' ') == std::wstring::npos)
            edited.mask = L"*";
        if (FSF.ProcessName(edited.mask.c_str(), nullptr, 0, PN_CHECKMASK))
            break;
        show_error(msg(MBadMask) + (L"\n" + edited.mask));
    }

    g_Settings = edited;
    save_settings();
    for (auto &p : g_Panels)
        p.second.infoValid = false;    // "Writing" in the info panel
    return TRUE;
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
