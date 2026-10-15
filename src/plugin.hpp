#pragma once

#include <string>
#include <vector>
#include <memory>
#include <set>
#include <plugin.hpp>
#include <dsk_tools/dsk_tools.h>

// ── Per-panel state ────────────────────────────────────────────
struct PluginPanel
{
    std::wstring                                  currentDir;   // "/" or "/A/B" (CurDir for FAR)
    std::vector<std::wstring>                     dirStack;     // displayed names of entered directories
    std::wstring                                  hostPath;
    std::wstring                                  title;        // " DskTools:<FS>:<image>"
    std::string                                   formatId;     // detect_fdd_type() result, kept to
    std::string                                   typeId;       // reload the image and to pick a writer
    std::string                                   fsId;
    std::wstring                                  newDirName;   // MakeDirectoryW result (Info->Name)
    std::unique_ptr<dsk_tools::diskImage>         image;
    std::unique_ptr<dsk_tools::fileSystem>        filesystem;
    dsk_tools::Files                              files;        // what the panel shows
    std::vector<std::wstring>                     wideNames;

    // CP/M: user areas 1..31 are shown as virtual USER_nn directories, user 0 at the root
    bool                                          cpmUsers = false;
    unsigned                                      cpmUser  = 0;     // area shown in the panel
    dsk_tools::Files                              cpmAll;           // all areas, from dir()
    std::set<unsigned>                            cpmNewAreas;      // empty areas created by F7
};

// ── Plugin globals ─────────────────────────────────────────────
extern PluginStartupInfo  PSI;
extern FarStandardFunctions FSF;
