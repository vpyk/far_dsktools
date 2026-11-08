// CP/M disk geometry the plugin finds itself, when dsk_tools' detection gets it wrong:
// from the parameter block in the boot sector, or by trying layouts on the directory.
// No FAR API here, so that it can be tested apart from the plugin.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsk_tools/dsk_tools.h"

namespace cpm_geometry {

// The geometry of a raw image as a diskdef describes it to fsCPM
struct Geometry
{
    unsigned seclen    = 0;
    unsigned heads     = 0;
    unsigned tracks    = 0;
    unsigned sectrk    = 0;
    unsigned blocksize = 0;
    unsigned maxdir    = 0;
    unsigned boottrk   = 0;     // cylinders: tracks of every side
    std::string sides  = "int";
    std::string charmap;
    std::vector<unsigned> skewtab;

    unsigned image_size() const { return heads * tracks * sectrk * seclen; }
    unsigned data_offset() const { return boottrk * heads * sectrk * seclen; }   // block 0
    // The values fsCPM::fill_dpb() derives from it
    unsigned dsm() const;
    unsigned exm() const;
    unsigned dir_blocks() const;
    uint8_t  al0() const;
    uint8_t  al1() const;
};

dsk_tools::DiskDef to_diskdef(const Geometry &g, const std::string &name);
// False when the diskdef is not a CP/M one (no block size) or is incomplete
bool from_diskdef(const dsk_tools::DiskDef &def, Geometry &g);

// The CP/M geometries of the diskdefs (those with a block size), keyed by name
std::vector<std::pair<std::string, Geometry>> cpm_diskdefs(const dsk_tools::DiskDefs &defs);

// The disk parameter block some CP/M systems (Orion, Korvet, MicroDOS) keep in the boot
// sector: bytes 8..30 hold the sector size, sides, sectors and tracks and then the DPB,
// byte 31 is a check sum ($66 plus the sum of bytes 0..30)
struct BootDpb
{
    Geometry geometry;      // as fsCPM reads it: one head over all the tracks, so that
                            // OFF can count the tracks of one side, as the DPB does
    bool     sameAsLibrary; // EXM, AL0/AL1 as fsCPM derives them, and its DSM not past the
                            // disk's: writing would allocate as the disk's own system does
};
bool read_boot_dpb(const uint8_t *boot, size_t bootSize, uint64_t fileSize, BootDpb &out);

// How a directory read with some geometry looks: live entries that make sense against
// those that cannot be right (name bytes, extent, record count, block numbers)
struct Score
{
    int good = 0;
    int bad  = 0;
    bool valid() const { return bad * 10 <= good; }    // an empty directory is valid
    int  rank() const  { return good - 10 * bad; }
};
Score score_directory(const std::vector<dsk_tools::UniversalFile> &files, unsigned dsm, unsigned dirBlocks);

// How the image's directory reads with geometry `g`; false if it does not open
bool score_image(const std::string &utf8Path, const Geometry &g, Score &score);

enum class Source
{
    Standard,   // dsk_tools' detection, the directory reads right
    Boot,       // the parameter block in the boot sector
    Guessed,    // a diskdef layout that the directory fits
    Damaged,    // dsk_tools' detection, no layout fits the directory
};

struct Found
{
    Source      source   = Source::Standard;
    std::string name;               // a diskdef name for Boot and Guessed
    Geometry    geometry;
    bool        writable = true;    // Boot: sameAsLibrary; Guessed, Damaged: never
};

// The geometry of a raw image: the parameter block of its boot sector when the directory
// reads right with it, else `standard` (dsk_tools' detection, nullptr if it did not
// detect the image) when that does, else the CP/M diskdefs of `library` of the file's
// size, with the system tracks also counted by sides (one head over all the tracks).
// False: no geometry for an image dsk_tools did not detect.
bool find(const std::string &utf8Path, const uint8_t *boot, size_t bootSize, uint64_t fileSize,
          const Geometry *standard, const dsk_tools::DiskDefs &library, Found &out);

// Every block of the file's directory entries lies outside the directory and not past
// `lastBlock` (DSM, or the last block a short image file holds): fsCPM reads a block past
// the end of the image without a check. `dsm` tells one-byte block numbers from two-byte.
bool blocks_valid(const dsk_tools::UniversalFile &uf, unsigned dsm, unsigned dirBlocks, unsigned lastBlock);

// The last block a file of `fileSize` bytes holds whole; -1 when it does not reach block 0
long long last_block_in_file(const Geometry &g, uint64_t fileSize);

}
