// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Viktor Pykhonin
// Part of DskTools, a FAR Manager plugin for disk images of retro computers

#include "cpm_geometry.hpp"

#include <algorithm>

namespace cpm_geometry {

// The formulas of fsCPM::fill_dpb()
unsigned Geometry::dsm() const
{
    const unsigned held = last_block();
    return dsmLimit != 0 && dsmLimit < held ? dsmLimit : held;
}

unsigned Geometry::last_block() const
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
    if (g.dsmLimit != 0)
        def.int_params["dsm"] = g.dsmLimit;
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
    g.dsmLimit  = int_param(def, "dsm", 0);
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
    g.dsmLimit  = dsm;      // some disks count fewer blocks than their tracks hold
    if (spt * 128 != sec * g.seclen || off >= g.tracks || g.blocksize < g.seclen)
        return false;
    // A disk formatted with more tracks than the block states (Vector: 82 of a side, the
    // block says 80): the tracks of the file, whole cylinders, up to 4 more a side. Saving
    // then writes the whole file back; dsmLimit keeps the blocks to the disk's
    const uint64_t cylinder = static_cast<uint64_t>(sec) * g.seclen * (sides + 1);
    if (fileSize > g.image_size() && (fileSize - g.image_size()) % cylinder == 0
        && (fileSize - g.image_size()) / cylinder <= 4)
        g.tracks = static_cast<unsigned>(fileSize / cylinder) * (sides + 1);
    // A file longer still is not the disk the block describes: PK8000 disks of 800 Kb keep a
    // block of a 400 Kb disk there (40 tracks a side, DSM 195) and number their blocks in
    // two bytes, which that DSM would read as one. (One 256 bytes longer would also be
    // taken by the raw loader for one with a header, which shifts every sector.) A shorter
    // file (an empty end cut off) must hold the directory at least: the loader fills the
    // rest with E5
    if (fileSize > g.image_size()
        || fileSize < static_cast<uint64_t>(g.data_offset()) + g.dir_blocks() * g.blocksize)
        return false;
    // Block numbers of one byte or two follow from DSM: the same on both sides, or fsCPM
    // reads the directory wrong. Against what the tracks hold: "dsm" only lowers it, so a
    // DSM of one width there and of the other here is not this disk's
    if ((g.last_block() < 256) != (dsm < 256))
        return false;

    out.geometry      = g;
    out.exm           = exm;
    out.al0           = b[0x19];
    out.al1           = b[0x1A];
    out.sameAsLibrary = g.exm() == exm && g.al0() == b[0x19] && g.al1() == b[0x1A];
    return true;
}

// One field of a name: printable, padded with spaces or, as some programs write it, with
// zero bytes (fsCPM skips those); a zero is followed by padding only. True if not blank.
static bool field_valid(const uint8_t *f, size_t n, bool &blank)
{
    bool padding = false;
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t c = f[i] & 0x7F;
        if (c == 0)
            padding = true;
        else if (c < 0x20 || c == 0x7F || (padding && c != ' '))
            return false;
        else if (c != ' ')
            blank = false;
    }
    return true;
}

static bool name_valid(const dsk_tools::CPM_DIR_ENTRY &e)
{
    bool blank = true, extBlank = true;
    return field_valid(e.F, sizeof(e.F), blank) && field_valid(e.E, sizeof(e.E), extBlank) && !blank;
}

// The block numbers of one entry, as fsCPM::load_file() reads them: only 0 is an empty slot,
// 0xE5 is block 229
static bool entry_blocks_valid(const dsk_tools::CPM_DIR_ENTRY &e, unsigned dsm, unsigned dirBlocks, unsigned lastBlock)
{
    const bool wide = dsm >= 256;
    bool hole = false;
    for (unsigned i = 0; i < 16; i += wide ? 2 : 1)
    {
        const unsigned al = wide ? e.AL[i] | (e.AL[i + 1] << 8) : e.AL[i];
        if (al == 0)
        {
            hole = true;
            continue;
        }
        // CP/M fills the slots of an entry in order. Numbers of two bytes read as one leave
        // a zero after every block, as the high bytes below 256 are
        if (hole && !wide)
            return false;
        if (al > lastBlock || al < dirBlocks)
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

Score score_directory(const std::vector<dsk_tools::UniversalFile> &files, unsigned dsm, unsigned dirBlocks,
                      unsigned lastBlock)
{
    Score s;
    for (const auto &uf : files)
        for (size_t i = 0; i < entry_count(uf); ++i)
        {
            const auto &e = entry(uf, i);
            const bool ok = e.ST <= 31 && name_valid(e) && e.XL <= 31 && e.XH <= 63 && e.RC <= 0x80
                         && entry_blocks_valid(e, dsm, dirBlocks, lastBlock);
            (ok ? s.good : s.bad)++;
        }
    return s;
}

bool blocks_valid(const dsk_tools::UniversalFile &uf, unsigned dsm, unsigned dirBlocks, unsigned lastBlock)
{
    for (size_t i = 0; i < entry_count(uf); ++i)
        if (!entry_blocks_valid(entry(uf, i), dsm, dirBlocks, lastBlock))
            return false;
    return true;
}

long long last_block_in_file(const Geometry &g, uint64_t fileSize)
{
    if (fileSize < g.data_offset())
        return -1;
    return static_cast<long long>((fileSize - g.data_offset()) / g.blocksize) - 1;
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
        score = score_directory(files, g.dsm(), g.dir_blocks(), g.last_block());
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
    // The parameter block has no character set: that of dsk_tools' type, of the same machine
    BootDpb dpb;
    Score score;
    const bool hasDpb = read_boot_dpb(boot, bootSize, fileSize, dpb);
    if (hasDpb && standard)
        dpb.geometry.charmap = standard->charmap;
    auto boot_fits = [&](const Geometry &g)
    {
        return score_image(utf8Path, g, score) && score.valid() && (detected || score.good > 0);
    };
    bool bootFits = hasDpb && boot_fits(dpb.geometry);
    bool bootWritable = dpb.sameAsLibrary;
    // A block of a disk with numbers of one byte, over a directory that reads right only
    // with numbers of two: PK8000 disks keep the block of a 400 Kb disk (40 tracks a side,
    // DSM 195) on an 800 Kb Orion disk. Its layout holds, the tracks and DSM do not: the
    // tracks of the library's disk of that layout, its DSM, the block's EXM and AL.
    if (hasDpb && dpb.geometry.last_block() < 256 && !(bootFits && score.bad == 0))
        for (const auto &kv : cpm_diskdefs(library))
        {
            const Geometry &d = kv.second;
            Geometry w = dpb.geometry;
            w.tracks   = d.tracks * d.heads;
            w.dsmLimit = 0;
            if (d.seclen != w.seclen || d.sectrk != w.sectrk || d.blocksize != w.blocksize
                || d.maxdir != w.maxdir || d.skewtab != w.skewtab || d.data_offset() != w.data_offset()
                || (d.heads != 1 && d.sides != "int") || w.last_block() < 256 || w.image_size() < fileSize)
                continue;
            if (boot_fits(w) && score.bad == 0)
            {
                dpb.geometry = w;
                bootFits     = true;
                bootWritable = w.exm() == dpb.exm && w.al0() == dpb.al0 && w.al1() == dpb.al1;
                break;
            }
        }
    if (bootFits)
    {
        const Geometry &g = dpb.geometry;
        out.source   = Source::Boot;
        out.name     = "boot-" + std::to_string(g.seclen) + "-" + std::to_string(g.sectrk) + "-"
                     + std::to_string(g.tracks) + "-" + std::to_string(g.blocksize) + "-"
                     + std::to_string(g.maxdir) + "-" + std::to_string(g.boottrk)
                     + "-dsm" + std::to_string(g.dsm())
                     + (g.charmap.empty() ? "" : "-" + g.charmap);
        out.geometry = g;
        out.writable = bootWritable;
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
