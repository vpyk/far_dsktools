#pragma once

#include <deque>
#include <string>
#include <vector>
#include <memory>
#include <set>
#include <plugin.hpp>
#include <dsk_tools/dsk_tools.h>
#include "cpm_geometry.hpp"

// Where the geometry of a raw CP/M image came from (find_cpm_geometry())
using GeometrySource = cpm_geometry::Source;

// ── Per-panel state ────────────────────────────────────────────
struct PluginPanel
{
    std::wstring                                  currentDir;   // "/" or "/A/B" (CurDir for FAR)
    std::vector<std::wstring>                     dirStack;     // displayed names of entered directories
    std::wstring                                  hostPath;
    std::wstring                                  title;        // " DskTools:<FS>:<image>"
    std::wstring                                  panelTitle;   // title + " [R/O]" when writing is refused (build_info())
    std::string                                   formatId;     // detect_fdd_type() result, kept to
    std::string                                   typeId;       // reload the image and to pick a writer
    std::string                                   fsId;
    std::wstring                                  newDirName;   // MakeDirectoryW result (Info->Name)
    std::wstring                                  destPath;     // GetFilesW: Info->DestPath after the copy dialog
    std::unique_ptr<dsk_tools::diskImage>         image;
    std::unique_ptr<dsk_tools::fileSystem>        filesystem;
    dsk_tools::Files                              files;        // what the panel shows
    std::vector<std::wstring>                     wideNames;

    // CP/M: user areas 1..31 are shown as virtual USER_nn directories, user 0 at the root
    bool                                          cpmUsers = false;
    unsigned                                      cpmUser  = 0;     // area shown in the panel
    dsk_tools::Files                              cpmAll;           // all areas, from dir()
    std::set<unsigned>                            cpmNewAreas;      // empty areas created by F7

    // Information panel (Ctrl+L), built after the image is loaded or saved
    bool                                          infoValid = false;
    std::deque<std::wstring>                      infoTexts;        // what infoLines point to
    std::vector<InfoPanelLine>                    infoLines;
    unsigned long long                            freeSize  = 0;    // OpenPanelInfo::FreeSize

    bool                                          backedUp  = false;    // <image>.bak made for this panel

    // CP/M: where the geometry came from (find_cpm_geometry()), and the range of block
    // numbers checked before fsCPM reads a file (cpmDsm 0: no check)
    GeometrySource                                geometry  = GeometrySource::Standard;
    bool                                          geometryWritable = true;
    unsigned                                      cpmDsm       = 0;
    unsigned                                      cpmDirBlocks = 0;
    unsigned                                      cpmLastBlock = 0;     // DSM, or the last block of a short file

    // The image file as this panel loaded or saved it last (sync_image())
    FILETIME                                      diskTime  = {};
    unsigned long long                            diskSize  = 0;
    unsigned                                      saveCount = 0;    // g_SaveCounts of the image then
};

// ── Plugin globals ─────────────────────────────────────────────
extern PluginStartupInfo  PSI;
extern FarStandardFunctions FSF;
