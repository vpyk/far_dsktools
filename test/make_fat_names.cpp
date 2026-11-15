// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Viktor Pykhonin
// Part of DskTools, a FAR Manager plugin for disk images of retro computers
// Builds a 1.44 MB FAT12 image with nested directories and names Windows cannot store,
// then lists it back recursively through dsk_tools.
#include <dsk_tools/dsk_tools.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace dsk_tools;

static void check(const Result &r, const char *what)
{
    if (!r) { std::printf("%s: %s %s\n", what, decode_error(r).c_str(), r.message.c_str()); std::exit(1); }
}

struct Mounted { std::unique_ptr<diskImage> img; std::unique_ptr<fileSystem> fs; };

static Mounted mount(const std::string &path, const DiskDefs &defs)
{
    std::string fmt, type, fsid;
    check(detect_fdd_type(path, fmt, type, fsid, false), "detect");
    Mounted m;
    m.img = prepare_image(path, fmt, type, defs);
    if (!m.img) { std::printf("prepare_image failed (%s)\n", type.c_str()); std::exit(1); }
    check(m.img->load(), "load");
    m.fs = prepare_filesystem(m.img.get(), fsid, defs);
    if (!m.fs) { std::printf("prepare_filesystem failed\n"); std::exit(1); }
    check(m.fs->open(), "open");
    std::printf("%s: %s %s\n", type.c_str(), fsid.c_str(), path.c_str());
    return m;
}

static void put(fileSystem &fs, const char *name, const std::string &text)
{
    UniversalFile uf{};
    uf.name = name;
    check(fs.put_file(uf, "FILE_BINARY", BYTES(text.begin(), text.end()), false), name);
}

static UniversalFile make_dir(fileSystem &fs, const char *name)
{
    UniversalFile d;
    check(fs.mkdir(std::string(name), d), name);
    return d;
}

// Replaces an 11-byte 8.3 directory name anywhere in the image buffer
static void patch_name(uint8_t *buf, size_t size, const char *from, const char *to)
{
    for (size_t i = 0; i + 11 <= size; i += 32)
        if (std::memcmp(buf + i, from, 11) == 0) { std::memcpy(buf + i, to, 11); return; }
    std::printf("entry not found: %s\n", from);
    std::exit(1);
}

static void list(fileSystem &fs, int depth)
{
    Files files;
    check(fs.dir(files, false), "dir");
    for (auto &f : files)
    {
        if (f.name == "..") continue;
        std::printf("%*s%s%s  %u\n", depth * 2 + 2, "", f.name.c_str(), f.is_dir ? "/" : "", f.size);
        if (f.is_dir) { fs.cd(f); list(fs, depth + 1); fs.cd_up(); }
    }
}

int main(int argc, char **argv)
{
    std::ifstream dd(argv[1]);
    std::stringstream ss; ss << dd.rdbuf();
    const DiskDefs defs = parse_diskdefs(ss.str());
    const std::string out = argv[2];
    const size_t size = 1474560;

    {   // Blank FAT12 1.44 MB floppy
        std::vector<uint8_t> img(size, 0);
        const uint8_t boot[] = { 0xEB, 0x3C, 0x90, 'D','S','K','T','O','O','L','S',
            0x00, 0x02, 0x01, 0x01, 0x00, 0x02, 0xE0, 0x00, 0x40, 0x0B, 0xF0, 0x09, 0x00,
            0x12, 0x00, 0x02, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x29, 1, 2, 3, 4,
            'T','E','S','T','N','A','M','E','S',' ',' ', 'F','A','T','1','2',' ',' ',' ' };
        std::memcpy(img.data(), boot, sizeof(boot));
        img[510] = 0x55; img[511] = 0xAA;
        for (int fat = 0; fat < 2; fat++)
        {
            uint8_t *f = img.data() + 512 * (1 + fat * 9);
            f[0] = 0xF0; f[1] = 0xFF; f[2] = 0xFF;
        }
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<char *>(img.data()), size);
    }

    {
        Mounted m = mount(out, defs);
        fileSystem &fs = *m.fs;
        put(fs, "README.TXT", "Root file\r\n");
        put(fs, "A_B.TXT",    "A_B.TXT: the real underscore\r\n");
        put(fs, "AQB.TXT",    "A?B.TXT: question mark, shown as A_B~2.TXT\r\n");
        put(fs, "COX.TXT",    "CON.TXT: reserved device name, shown as CON_.TXT\r\n");
        UniversalFile sub = make_dir(fs, "SUB");
        fs.cd(sub);
        put(fs, "NESTED.TXT", "SUB/NESTED.TXT\r\n");
        put(fs, "STARQ.TXT",  "SUB/STAR*.TXT: asterisk, shown as STAR_.TXT\r\n");
        UniversalFile deep = make_dir(fs, "DEEP");
        fs.cd(deep);
        put(fs, "DEEPEST.TXT", "SUB/DEEP/DEEPEST.TXT\r\n");
        fs.cd_up();
        fs.cd_up();

        uint8_t *base = nullptr;
        for (unsigned t = 0; t < 80; t++)
            for (unsigned h = 0; h < 2; h++)
                for (unsigned s = 1; s <= 18; s++)
                {
                    uint8_t *p = m.img->get_sector_data(h, t, s);
                    if (p && (!base || p < base)) base = p;
                }
        // get_sector_data may number sectors from 0: take the lowest address either way
        for (unsigned t = 0; t < 80; t++)
            for (unsigned h = 0; h < 2; h++)
            {
                uint8_t *p = m.img->get_sector_data(h, t, 0);
                if (p && p < base) base = p;
            }
        patch_name(base, size, "AQB     TXT", "A?B     TXT");
        patch_name(base, size, "COX     TXT", "CON     TXT");
        patch_name(base, size, "STARQ   TXT", "STAR*   TXT");
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(base), size);
    }

    Mounted m = mount(out, defs);
    list(*m.fs, 0);
    return 0;
}
