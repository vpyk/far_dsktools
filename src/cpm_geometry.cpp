#include "cpm_geometry.hpp"

#include <algorithm>

namespace cpm_geometry {

// The formulas of fsCPM::fill_dpb()
unsigned Geometry::dsm() const
{
    return (tracks - boottrk) * heads * sectrk * seclen / blocksize - 1;
}

unsigned Geometry::exm() const
{
    return dsm() < 256 ? (blocksize >= 1024 ? blocksize / 1024 - 1 : 0)
                       : (blocksize >= 2048 ? blocksize / 2048 - 1 : 0);
}

unsigned Geometry::dir_blocks() const
{
    return (maxdir * 32 + blocksize - 1) / blocksize;
}

static uint16_t al_word(unsigned dirBlocks)
{
    return dirBlocks >= 16 ? 0xFFFF
                           : static_cast<uint16_t>(((1u << dirBlocks) - 1u) << (16u - dirBlocks));
}

uint8_t Geometry::al0() const { return static_cast<uint8_t>(al_word(dir_blocks()) >> 8); }
uint8_t Geometry::al1() const { return static_cast<uint8_t>(al_word(dir_blocks()) & 0xFF); }

dsk_tools::DiskDef to_diskdef(const Geometry &g, const std::string &name)
{
    dsk_tools::DiskDef def;
    def.name = name;
    def.int_params = { { "seclen", g.seclen }, { "heads", g.heads }, { "tracks", g.tracks },
                       { "sectrk", g.sectrk }, { "blocksize", g.blocksize },
                       { "maxdir", g.maxdir }, { "boottrk", g.boottrk } };
    def.str_params["sides"] = g.sides;
    if (!g.charmap.empty())
        def.str_params["charmap"] = g.charmap;
    def.skewtab = g.skewtab;
    return def;
}

static unsigned int_param(const dsk_tools::DiskDef &def, const char *key, unsigned fallback)
{
    auto it = def.int_params.find(key);
    return it == def.int_params.end() ? fallback : it->second;
}

// With the defaults of prepare_image() and fsCPM::fill_dpb()
bool from_diskdef(const dsk_tools::DiskDef &def, Geometry &g)
{
    g = Geometry();
    g.blocksize = int_param(def, "blocksize", 0);
    g.seclen    = int_param(def, "seclen", 0);
    g.heads     = int_param(def, "heads", 2);
    g.tracks    = int_param(def, "tracks", 0);
    g.sectrk    = int_param(def, "sectrk", 0);
    g.maxdir    = int_param(def, "maxdir", 64);
    g.boottrk   = int_param(def, "boottrk", 0);
    if (!g.blocksize || !g.seclen || !g.heads || !g.tracks || !g.sectrk || g.boottrk >= g.tracks)
        return false;
    auto s = def.str_params.find("sides");
    if (s != def.str_params.end())
        g.sides = s->second;
    auto c = def.str_params.find("charmap");
    if (c != def.str_params.end())
        g.charmap = c->second;
    g.skewtab = def.skewtab;
    return true;
}

std::vector<std::pair<std::string, Geometry>> cpm_diskdefs(const dsk_tools::DiskDefs &defs)
{
    std::vector<std::pair<std::string, Geometry>> out;
    Geometry g;
    for (const auto &kv : defs)
        if (from_diskdef(kv.second, g))
            out.emplace_back(kv.first, g);
    return out;
}

static unsigned word(const uint8_t *p) { return p[0] | (p[1] << 8); }

bool read_boot_dpb(const uint8_t *b, size_t bootSize, uint64_t fileSize, BootDpb &out)
{
    if (bootSize < 32)
        return false;
    unsigned sum = 0x66;
    for (int i = 0; i < 31; ++i)
        sum += b[i];
    if ((sum & 0xFF) != b[31])
        return false;

    const unsigned len   = b[0x0A];
    const unsigned sides = b[0x0B];
    const unsigned sec   = word(b + 0x0C);
    const unsigned trk   = word(b + 0x0E);
    const unsigned spt   = word(b + 0x10);
    const unsigned bsh   = b[0x12];
    const unsigned blm   = b[0x13];
    const unsigned exm   = b[0x14];
    const unsigned dsm   = word(b + 0x15);
    const unsigned drm   = word(b + 0x17);
    const unsigned off   = word(b + 0x1D);
    if (len > 3 || sides > 1 || sec == 0 || sec > 64 || trk == 0 || trk > 255
        || bsh < 3 || bsh > 7 || blm != (1u << bsh) - 1 || drm >= 2048)
        return false;

    Geometry g;
    g.seclen    = 128u << len;
    g.heads     = 1;
    g.tracks    = trk * (sides + 1);
    g.sectrk    = sec;
    g.blocksize = 128u << bsh;
    g.maxdir    = drm + 1;
    g.boottrk   = off;
    if (spt * 128 != sec * g.seclen || off >= g.tracks || g.blocksize < g.seclen)
        return false;
    // The image must hold the whole disk. A file 256 bytes longer is taken by the raw
    // loader for one with a header, which would shift every sector
    if (fileSize < g.image_size() || fileSize == g.image_size() + 256u)
        return false;
    // Block numbers of one byte or two follow from DSM: the same on both sides, or fsCPM
    // reads the directory wrong
    if ((g.dsm() < 256) != (dsm < 256))
        return false;

    out.geometry      = g;
    out.sameAsLibrary = g.exm() == exm && g.al0() == b[0x19] && g.al1() == b[0x1A] && g.dsm() <= dsm;
    return true;
}

static bool name_valid(const dsk_tools::CPM_DIR_ENTRY &e)
{
    bool blank = true;
    for (uint8_t c : e.F)
    {
        c &= 0x7F;
        if (c < 0x20 || c == 0x7F)
            return false;
        if (c != ' ')
            blank = false;
    }
    for (uint8_t c : e.E)
    {
        c &= 0x7F;
        if (c < 0x20 || c == 0x7F)
            return false;
    }
    return !blank;
}

// The block numbers of one entry, as fsCPM::load_file() reads them
static bool entry_blocks_valid(const dsk_tools::CPM_DIR_ENTRY &e, unsigned dsm, unsigned dirBlocks)
{
    const bool wide = dsm >= 256;
    for (unsigned i = 0; i < 16; i += wide ? 2 : 1)
    {
        const unsigned al = wide ? e.AL[i] | (e.AL[i + 1] << 8) : e.AL[i];
        if (al == 0 || al == 0xE5 || al == 0xE5E5)
            continue;
        if (al > dsm || al < dirBlocks)
            return false;
    }
    return true;
}

static size_t entry_count(const dsk_tools::UniversalFile &uf)
{
    return uf.metadata.size() / sizeof(dsk_tools::CPM_DIR_ENTRY);
}

static const dsk_tools::CPM_DIR_ENTRY &entry(const dsk_tools::UniversalFile &uf, size_t i)
{
    return *reinterpret_cast<const dsk_tools::CPM_DIR_ENTRY *>(uf.metadata.data() + i * sizeof(dsk_tools::CPM_DIR_ENTRY));
}

Score score_directory(const std::vector<dsk_tools::UniversalFile> &files, unsigned dsm, unsigned dirBlocks)
{
    Score s;
    for (const auto &uf : files)
        for (size_t i = 0; i < entry_count(uf); ++i)
        {
            const auto &e = entry(uf, i);
            const bool ok = e.ST <= 31 && name_valid(e) && e.XL <= 31 && e.XH <= 63 && e.RC <= 0x80
                         && entry_blocks_valid(e, dsm, dirBlocks);
            (ok ? s.good : s.bad)++;
        }
    return s;
}

bool blocks_valid(const dsk_tools::UniversalFile &uf, unsigned dsm, unsigned dirBlocks)
{
    for (size_t i = 0; i < entry_count(uf); ++i)
        if (!entry_blocks_valid(entry(uf, i), dsm, dirBlocks))
            return false;
    return true;
}

bool score_image(const std::string &utf8Path, const Geometry &g, Score &score)
{
    try
    {
        dsk_tools::DiskDefs one;
        one["candidate"] = to_diskdef(g, "candidate");
        auto img = dsk_tools::prepare_image(utf8Path, "FILE_RAW_MSB", "TYPE_CPM:CANDIDATE", one);
        if (!img || !img->load())
            return false;
        auto fs = dsk_tools::prepare_filesystem(img.get(), "FILESYSTEM_CPM_RAW", one);
        if (!fs || !fs->open())
            return false;
        std::vector<dsk_tools::UniversalFile> files;
        if (!fs->dir(files, false))
            return false;
        score = score_directory(files, g.dsm(), g.dir_blocks());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// The same layout of the disk, however the diskdef puts it
static bool same_layout(const Geometry &a, const Geometry &b)
{
    return a.seclen == b.seclen && a.heads * a.tracks == b.heads * b.tracks && a.sectrk == b.sectrk
        && a.blocksize == b.blocksize && a.maxdir == b.maxdir && a.skewtab == b.skewtab
        && a.boottrk * a.heads == b.boottrk * b.heads
        && (a.heads == 1 || b.heads == 1 || a.sides == b.sides);
}

bool find(const std::string &utf8Path, const uint8_t *boot, size_t bootSize, uint64_t fileSize,
          const Geometry *standard, const dsk_tools::DiskDefs &library, Found &out)
{
    out = Found();
    const bool detected = standard != nullptr;

    // A file dsk_tools does not know needs live entries; a known one may be empty
    BootDpb dpb;
    Score score;
    if (read_boot_dpb(boot, bootSize, fileSize, dpb) && score_image(utf8Path, dpb.geometry, score)
        && score.valid() && (detected || score.good > 0))
    {
        const Geometry &g = dpb.geometry;
        out.source   = Source::Boot;
        out.name     = "boot-" + std::to_string(g.seclen) + "-" + std::to_string(g.sectrk) + "-"
                     + std::to_string(g.tracks) + "-" + std::to_string(g.blocksize) + "-"
                     + std::to_string(g.maxdir) + "-" + std::to_string(g.boottrk);
        out.geometry = g;
        out.writable = dpb.sameAsLibrary;
        return true;
    }
    if (!detected)
        return false;
    if (score_image(utf8Path, *standard, score) && score.valid())
        return true;

    std::vector<Geometry> tried { *standard };
    int bestRank = -1;
    for (const auto &kv : cpm_diskdefs(library))
    {
        const Geometry &def = kv.second;
        if (def.image_size() != fileSize)
            continue;
        std::vector<std::pair<std::string, Geometry>> variants;
        if (def.heads == 1 || def.sides == "int")
            for (unsigned sys = 0; sys <= 8; ++sys)
            {
                Geometry v = def;
                v.tracks  = def.tracks * def.heads;
                v.heads   = 1;
                v.sides   = "int";
                v.boottrk = sys;
                variants.emplace_back(kv.first + "-sys" + std::to_string(sys), v);
            }
        else
            variants.emplace_back(kv.first, def);

        for (const auto &v : variants)
        {
            if (v.second.boottrk >= v.second.tracks
                || std::any_of(tried.begin(), tried.end(), [&](const Geometry &t) { return same_layout(t, v.second); }))
                continue;
            tried.push_back(v.second);
            if (score_image(utf8Path, v.second, score) && score.good > 0 && score.valid() && score.rank() > bestRank)
            {
                bestRank     = score.rank();
                out.source   = Source::Guessed;
                out.name     = v.first;
                out.geometry = v.second;
            }
        }
    }
    if (out.source != Source::Guessed)
        out.source = Source::Damaged;
    out.writable = false;
    return true;
}

}
