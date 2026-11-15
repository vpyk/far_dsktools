// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Viktor Pykhonin
// Part of DskTools, a FAR Manager plugin for disk images of retro computers
// Builds a Korvet CP/M image with files in several user areas, then reads it back.
#include <dsk_tools/dsk_tools.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace dsk_tools;

static void check(const Result &r, const char *what)
{
    if (!r) { std::printf("%s: %s %s\n", what, decode_error(r).c_str(), r.message.c_str()); std::exit(1); }
}

struct Mounted
{
    std::unique_ptr<diskImage> img;
    std::unique_ptr<fileSystem> fs;
};

static Mounted mount(const std::string &path, const DiskDefs &defs)
{
    std::string fmt, type, fsid;
    check(detect_fdd_type(path, fmt, type, fsid, false), "detect");
    Mounted m;
    m.img = prepare_image(path, fmt, type, defs);
    if (!m.img) { std::printf("prepare_image failed\n"); std::exit(1); }
    check(m.img->load(), "load");
    m.fs = prepare_filesystem(m.img.get(), fsid, defs);
    if (!m.fs) { std::printf("prepare_filesystem failed\n"); std::exit(1); }
    check(m.fs->open(), "open");
    std::printf("%s: %s %s %s\n", path.c_str(), fmt.c_str(), type.c_str(), fsid.c_str());
    return m;
}

static void put(fileSystem &fs, const char *name, const std::string &text, unsigned user)
{
    UniversalFile uf{};
    uf.fs = FS::None;
    uf.name = name;
    BYTES data(text.begin(), text.end());
    check(fs.put_file(uf, "FILE_BINARY", data, false), name);
    if (user == 0) return;
    Files files;
    check(fs.dir(files, false), "dir");
    for (auto &f : files)
        if (f.name == name && ((f.attributes >> 8) & 0xFF) == 0)
        {
            check(fs.file_set_metadata(f, {{"user", std::to_string(user)}}), "set user");
            return;
        }
    std::printf("not found after put: %s\n", name);
    std::exit(1);
}

int main(int argc, char **argv)
{
    const std::string diskdefs_path = argv[1], out = argv[2];
    std::ifstream dd(diskdefs_path);
    std::stringstream ss; ss << dd.rdbuf();
    const DiskDefs defs = parse_diskdefs(ss.str());

    const size_t size = 819200;   // Korvet: 2 x 80 x 5 x 1024
    { std::ofstream f(out, std::ios::binary); std::string blank(size, '\xE5'); f << blank; }

    {
        Mounted m = mount(out, defs);
        put(*m.fs, "README.TXT", "User 0 file\r\n", 0);
        put(*m.fs, "SAME.TXT",   "SAME.TXT from user 3\r\n", 3);
        put(*m.fs, "SAME.TXT",   "SAME.TXT from user 0\r\n", 0);
        put(*m.fs, "PROG3.COM",  std::string(5000, 'A'), 3);   // several records
        put(*m.fs, "NOTE15.TXT", "User 15 file\r\n", 15);

        // CP/M 3 directory label (ST = 0x20) in a free directory slot: must not appear as a file
        bool label = false;
        for (unsigned s = 0; s < 4 && !label; s++)
        {
            uint8_t *sec = m.img->get_sector_data(0, 1, s);
            for (unsigned e = 0; e < 1024 / 32 && !label; e++)
                if (sec[e * 32] == 0xE5)
                {
                    uint8_t *de = sec + e * 32;
                    std::memset(de, 0, 32);
                    de[0] = 0x20;
                    std::memcpy(de + 1, "TESTDISK   ", 11);
                    label = true;
                }
        }

        // Save the whole buffer: the lowest sector address is the start of the image buffer
        uint8_t *base = nullptr;
        for (unsigned t = 0; t < 80; t++)
            for (unsigned h = 0; h < 2; h++)
                for (unsigned s = 0; s < 5; s++)
                {
                    uint8_t *p = m.img->get_sector_data(h, t, s);
                    if (p && (!base || p < base)) base = p;
                }
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char *>(base), size);
    }

    Mounted m = mount(out, defs);
    Files files;
    check(m.fs->dir(files, false), "dir");
    for (auto &f : files)
    {
        BYTES data;
        check(m.fs->get_file(f, "raw", data), "get_file");
        std::string head(data.begin(), data.begin() + std::min<size_t>(data.size(), 22));
        std::replace(head.begin(), head.end(), '\r', ' ');
        std::replace(head.begin(), head.end(), '\n', ' ');
        std::printf("  user %2u  %-12s %6u  \"%s\"\n", (f.attributes >> 8) & 0xFF, f.name.c_str(), f.size, head.c_str());
    }
    return 0;
}
