# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Toolchain: MSVC 2022 Build Tools (19.44, Windows SDK 10.0.26100) — CMake finds the installation itself via `vswhere` (here it is on `D:`, which works without extra settings). `CMakePresets.json` sets the generator and architecture; no machine-specific paths. FAR3 SDK headers are in-tree (`third_party/FarPluginSDK`); `-DFAR_SDK_PATH=...` overrides.

```
cmake --preset x64 && cmake --build --preset x64    # main build for testing
cmake --preset x86 && cmake --build --preset x86
```

Output: `build/<preset>/Release/DskTools.dll` (x64 → 64-bit FAR, x86 → 32-bit FAR) plus `DskToolsEng.lng`, `DskToolsRus.lng` (copied there by a post-build step). Copy all three to FAR3 `Plugins/DskTools/` — without the `.lng` files the plugin has no texts. The DLL bitness must match FAR's, otherwise FAR silently skips it.

## Architecture

FAR3 file system plugin (плагин файловой системы) — a shared library exporting C-linkage functions. FAR loads the DLL, calls the exports, and the plugin presents a virtual file system panel.

**Plugin lifecycle:**
1. FAR calls `GetGlobalInfoW` — identity (GUID, title, version)
2. FAR calls `SetStartupInfoW` — receives `PluginStartupInfo` (Far API function pointers) and `FarStandardFunctions`
3. FAR calls `GetPluginInfoW` — menu registration, command prefix
4. User triggers plugin → FAR calls `OpenW` — returns a handle
5. FAR calls `GetOpenPanelInfoW` / `GetFindDataW` / `FreeFindDataW` — panel content
6. FAR calls `ClosePanelW` on panel close
7. FAR calls `ExitFARW` on shutdown

**Key files:**

| File | Purpose |
|------|---------|
| `src/plugin.cpp` | All 19 FAR3 exports + internal state |
| `src/plugin.hpp` | `PluginPanel` struct per-panel state, extern globals |
| `src/plugin.def` | MSVC linker export list — **required**, otherwise zero exports |
| `src/uuid.hpp` | Plugin GUID as `{ DWORD, WORD, WORD, BYTE[8] }` — not a string |
| `src/lang.hpp` | `enum LngId` — message ids, the order of strings in the `.lng` files |
| `src/DskToolsEng.lng`, `src/DskToolsRus.lng` | Interface texts (UTF-8 with BOM), each string preceded by its label `//[Mxxx]` |
| `src/version.hpp` | `MAJOR.MINOR.BUILD` macros for `MAKEFARVERSION()` |
| `CMakeLists.txt` | SDK path via `FAR_SDK_PATH` (default `third_party/FarPluginSDK`), links `dsk_tools` static library (marked `SYSTEM`: no `/W4` warnings from its headers) |
| `CMakePresets.json` | `x64` (main) and `x86` presets, MSVC 2022 |
| `third_party/FarPluginSDK/` | FAR3 SDK C/C++ headers (`Headers.c/plugin.hpp`), build 6666, unmodified |
| `third_party/dsk_tools/` | dsk_tools static library (GPL-3.0), built as subdirectory |
| `TODO.md` | Open tasks, incl. the pre-release `MinFarVersion` decision — keep it updated |
| `test/` | Test images and their generators: `cpm_users.kdi` (CP/M user areas), `fat_names.img` (nested FAT dirs, Windows-invalid names) |
| `.gitattributes` | `*.kdi`, `*.img` binary; FAR SDK headers stored byte for byte (`-text`) |

**Current state:** Disk image browser plugin using [dsk_tools](https://github.com/Ptr314/dsk_tools). Opens CP/M, DOS 3.3, Agat, FAT, ProDOS, RT-11 and other retro disk images by Enter/Ctrl+PgDn on the file (`AnalyseW` → `OpenW(OPEN_ANALYSE)`), via `dsk:<path>` in FAR command line, or via F11 menu (opens the file under cursor). Errors are shown with `PSI.Message`; nothing is shown when `AnalyseW` does not recognize a file. Supports directory navigation, file extraction (`GetFilesW`) and writing (`PutFilesW`, `DeleteFilesW`, `MakeDirectoryW`, F6 both ways) for raw images.

## Critical FAR3 API details

- **`.def` file is mandatory** — MSVC does not export `extern "C"` functions without `__declspec(dllexport)` or a `.def` file. All 19 exports must be listed.
- **`PluginMenuItem.Guids` must not be `nullptr` when `Count > 0`** — FAR3 dereferences `Guids[i]` in `uuid::str()` during cache save without a null check. Crash: `STATUS_ACCESS_VIOLATION` at `uuid::str+0x61`.
- **`PluginPanelItem` has flat fields** (`FileName`, `FileAttributes`), not a nested `FindData` sub-struct (FAR2/far2l pattern).
- **`GlobalInfo::Version` and `::MinFarVersion` are `struct VersionInfo`**, not `DWORD`. Use `MAKEFARVERSION()` macro.
- **`PluginInfo` menus use `struct PluginMenuItem`** (`Guids`, `Strings`, `Count`), not raw `const wchar_t*` pointers.
- **`GetOpenPanelInfoW` returns `void`**, not `intptr_t`.
- **Memory**: plugin allocates `PluginPanelItem[]` with `new[]` and a `new[]` copy of every `FileName`, frees both in `FreeFindDataW`. The list must stay valid until `FreeFindDataW`, not until the next `SetDirectoryW`: Alt+F7 (`ScanPluginTree`) keeps it while it enters subdirectories and calls `GetFindData` again. No `PSI.Alloc`/`PSI.Free` — those don't exist in FAR3 FSF.
- **Selection after `GetFiles`/`PutFiles`** (`FileList::PluginGetFiles`, `PluginPutFilesToAnother` → `PluginClearSelection`): on return `1` FAR deselects **all** items; on anything else (`0`, `-1`) only the items whose `PPIF_SELECTED` the plugin **cleared**. So the plugin clears the flag on every item it processed successfully and returns `TRUE` only when all items are done — failed/skipped/cancelled ones stay selected.
- **Delete confirmation**: FAR asks nothing before `DeleteFiles` on a plugin panel (`FileList::PluginDelete`); the plugin asks itself unless `OPM_SILENT` (plugin→plugin move), following FAR's Options > Confirmations read through `PSI.SettingsControl(FarGuid)`, root `FSSF_CONFIRMATIONS`: `Delete` ("Do you wish to delete…") and `DeleteFolder` (once more for each selected non-empty folder / CP/M area: Delete/All/Skip/Cancel). F6 out of the image deletes without asking, as FAR's own move does.
- **Copy dialogs**: copying *into* a plugin FAR shows its own copy dialog, then `PutFiles`; copying *out of* a plugin it calls `GetFiles` with the other panel's path without a dialog. After `PutFiles` with `Move` FAR does **not** delete the sources — the plugin must. Plugin→plugin copies go through a temp folder: `GetFiles(OPM_SILENT)` + `PutFiles` (+ `DeleteFiles` for move).
- **Handle values**: `OpenW` returns `nullptr` on failure (not `INVALID_HANDLE_VALUE`). Panel handles count up from 1 — `(HANDLE)-1`/`-2` are `PANEL_STOP`/`PANEL_PASSIVE`, and returning `PANEL_STOP` from `OPEN_ANALYSE` aborts opening.
- **Analyse flow**: `AnalyseW` runs `detect_fdd_type` (extension + size) and stores the result in `g_Analysed` under a handle; `OpenW(OPEN_ANALYSE)` consumes it, `CloseAnalyseW` drops it if FAR does not open it.
- **Localization**: every user-visible text is `msg(MXxx)` (`PSI.GetMsg`) or `msg(MXxx, { args })` with `%1`, `%2`… placeholders; `items_text(n)` picks the Russian plural form (`MItemsOne/Few/Many`). FAR picks the `.lng` by its interface language (`.Language=` line), falling back to English. To add a text: a new id in `lang.hpp` and a `//[Mxxx]` + `"text"` pair at the same position in **both** `.lng` files — CMake fails the configure step if labels or string counts differ from the enum. Hotkeys (`&`) must be unique within each dialog in each language. Not translated: `GetGlobalInfoW` texts (called before `SetStartupInfoW`), filesystem names, error texts from dsk_tools (`decode_error`, `Result::message`) — they come in English; Windows errors come in the system language.
- **`PSI.Message` does not break lines on `\n`** — each `Items[]` element is one dialog line (`Items[0]` is the title). `show_error()` splits text on `\n` into separate items.
- **No `OPIF_REALNAMES`** — files live inside the image, so FAR must fetch them via `GetFilesW` (F3/F4/F5 extract into `DestPath`). With the flag FAR accesses non-existent host paths. `GetFilesW` returns `TRUE`/`FALSE`, not a count.
- **Root `CurDir` must be empty** in `GetOpenPanelInfoW`, otherwise `..` at the image root does not return to the host panel. Internally `currentDir` stays `"/"`.
- **Handle arithmetic**: use `intptr_t` for the handle counter, cast to/from `HANDLE` with `reinterpret_cast`. Direct `HANDLE` arithmetic is illegal in C++.
- **UUID format**: `PLUGIN_UUID` must be a braced `GUID` struct initializer, not a string like `L"{...}"`. The plugin GUID is `{A800155E-D7FA-4A2F-B529-75F2F0AE2CEF}` — never change it: FAR keys settings, menu items and macros to it.

## dsk_tools integration

Plugin opens disk images via:
1. `detect_fdd_type(utf8Path, formatId, typeId, fsId, false)` — auto-detect format
2. `prepare_image(path, formatId, typeId, DiskDefs{})` → `image->load()` — load raw sectors
3. `prepare_filesystem(image.get(), fsId, DiskDefs{})` → `filesystem->open()` — mount FS
4. `filesystem->dir(files, false)` — list directory
5. `filesystem->get_file(uf, "FILE_BINARY", data)` — extract file as `BYTES` (`std::vector<uint8_t>`). Use `FILE_BINARY`: every filesystem lists it in `get_save_file_formats()`, DOS 3.3 rejects other strings ("Writing to this format is not supported")

`PluginPanel` holds `unique_ptr<diskImage>`, `unique_ptr<fileSystem>`, cached `Files` and parallel `vector<wstring> wideNames` (displayed names; `GetFindDataW` copies them into each item). File index for `GetFilesW` is stored in `PluginPanelItem::UserData.Data` via `reinterpret_cast<void*>(static_cast<intptr_t>(i))`.

**Names and navigation** — `build_wide_names()` gives every listing safe, unique names (`sanitize_name()`: Windows-invalid characters → `_`, reserved device names → `CON_`, trailing dots/spaces, case-insensitive clashes → `~2`). The panel shows them and extraction writes them, so FAR and the plugin always agree. Consequently a directory is entered by its `UniversalFile` found through the displayed name (`set_dir()`, `panel.dirStack`), never by `find_file(name)`. `list_dir()` drops the `..` entry that DOS 3.3/FAT/ProDOS/Onix/Sprite OS `dir()` adds (FAR adds its own via `OPIF_ADDDOTS`).

**Extraction** (`GetFilesW`) — `OpContext` holds the mode and the "all" answers; `extract_item()` dispatches to a file, a recursive directory (`extract_dir`: `cd` → `list_dir` → recurse → `cd_up`) or a CP/M area. Overwrite/error prompts are skipped in `OPM_SILENT/FIND/VIEW/EDIT/QUICKVIEW`; Cancel returns `-1`. `Move` (F6) extracts, then deletes from the image; refused while the image is read-only.

**Copy dialog** — interactive F5/F6 out of the image (`OpMode` without `SILENT/FIND/VIEW/EDIT/QUICKVIEW/DESCR`) goes through `copy_dialog()`: built with `PluginDialogBuilder` (`DlgBuilder.hpp` from the SDK), which sizes the dialog by the translated texts; laid out like FAR's own copy dialog — the existing-files choice is a drop-down list (`DIF_DROPDOWNLIST`, list items without `&`, the hotkey is on the label before it), own GUID `COPY_DIALOG_UUID`, history `Copy` shared with FAR. `resolve_dest()` makes a relative path relative to the path FAR offered; a single file to a path that is not a folder is copied under that name; `Info->DestPath` is set to the real folder. The existing-files choice (Ask by default every time, not remembered — as in FAR's own dialogs) maps to `OpContext::overwriteAll/skipExisting`. Questions and errors stay `PSI.Message`, F7 stays `PSI.InputBox` — FAR's standard helpers for those.

**Writing** — 140 Kb Agat/Apple II images in ProDOS order (`.po`) are held in DOS 3.3 order by dsk_tools; `create_writer()` without a file name writes them back in the order of the file, which the in-place save relies on — do not pass a file name there. Writing is allowed when `read_only_reason()` is empty: filesystem has `FSCaps::Add` (CP/M, DOS 3.3, FAT, RT-11), format is `FILE_RAW_MSB` and the file size equals the image buffer (no header), file not R/O. HFE/MFM are not saved on purpose: their writers re-encode every track. Every operation edits the in-memory image, then `commit_changes()` saves at once (`save_image()`: `create_writer()->write(buffer)` → `<image>.dsktools-tmp` → `ReplaceFileW`; not `Writer::write(file_name)`, which uses `std::ofstream` and non-Unicode paths). If saving fails the image is reloaded (`discard_changes()` → `mount_image()`), dropping the changes. The same happens when a library write fails halfway (`op_broken()`, `OpContext::broken`): only errors reported before the image is touched (`fails_before_change()`: invalid name, already exists, no space, directory not empty) give Skip/Cancel; any other error stops the operation and leaves the disk image as it was before it. The plugin checks for an existing file itself (`find_entry()`): DOS 3.3 `put_file` neither detects nor replaces duplicates, so there it deletes first. Directories are deleted bottom-up (libraries delete only empty ones). With `Move`, `PutFilesW` deletes host sources only after a successful save. `OpContext` is shared by extraction and writing.

**CP/M user areas** — dsk_tools lists all areas flat and keeps the ST byte in `UniversalFile::attributes >> 8`. For `FS::CPM` the plugin (`panel.cpmUsers`) keeps the full listing in `cpmAll` and shows user 0 at the root plus a virtual directory `USER_nn` (two digits) for every other used area 1..31; `set_cpm_dir()` handles `SetDirectoryW` for them, `CurDir` is `/USER_nn`. Entries with ST > 31 (CP/M 3 label/date stamps) are hidden. `fsCPM::put_file` writes to user `attributes >> 8` when `uf.fs == FS::CPM` (else user 0), and `dir()` skips CP/M 3 label/date stamp entries (both upstream since `acc251b`). Test image: `test/cpm_users.kdi` (generator `test/make_cpm_users.cpp`).

**Disk definitions** — geometries for CP/M, FAT and RT-11 (korvet, orion, vector, gmd-7012, irisha-*, pc-*, st-*, iskra-226, prodos-800, dx, mx-*, my-800, mz-*) live in `third_party/dsk_tools/src/diskdefs` (cpmtools format). CMake embeds the file into `<build dir>/diskdefs_embed.h` via the library's `utils/diskdefs_embed.h.in` template; `OpenW` calls `parse_diskdefs(embedded_diskdefs())`. Without the right diskdef, `prepare_image()` returns `nullptr` for CP/M/FAT types.

**File dates** — `UniversalFile::date` (`FileDate`: local time without a zone, year 0 = no date, hour -1 = no time) is filled by filesystems with `FSCaps::Date`: FAT, ProDOS, RT-11, Sprite OS. `to_filetime()` converts it through the local time zone (`TzSpecificLocalTimeToSystemTime`) into the UTC `FILETIME` FAR expects, so FAR shows the stored wall-clock time unchanged; no date gives a zero `FILETIME` (empty column). It goes to `PluginPanelItem::LastWriteTime` and to extracted files (`SetFileTime` before closing) and folders (after their contents). `StartPanelMode` is `'3'` (Full: name, size, date, time) for filesystems with dates, `'4'` (Wide) otherwise. Writing into an image passes the host file's/folder's last write time back as local time (`host_date()`) in `UniversalFile::date` to `put_file` and `mkdir(UniversalFile)`. FAT (1980..2107) and RT-11 (1972..2099) keep it (upstream since `c8d61f9`), falling back to the current time outside that range or without a date; other filesystems ignore it.

**Character encoding** — dsk_tools returns `UniversalFile::name` in UTF-8 (CP/M uses the diskdef's `charmap`). `to_wide()` tries CP_UTF8 first, then falls back to CP_OEMCP and Latin-1.

dsk_tools is a **git submodule** (`https://github.com/Ptr314/dsk_tools`, pinned at `c8d61f9`: 1.4.2 plus the file date fix), kept **unmodified** — fixes go upstream as patches (`git format-patch`, kept in `patches/` until accepted), made against the `dev` branch: upstream retargets pull requests from forks to `dev`. Patches for upstream end with an `Assisted-By:` trailer, not `Co-Authored-By:`. While a fix is waiting, it may be applied to the submodule working tree for testing, but the submodule pointer is never committed in that state. Fresh clone: `git clone --recurse-submodules` (or `git submodule update --init`). To update: `git -C third_party/dsk_tools pull --ff-only origin master`, rebuild, then commit the new submodule pointer in the main repo. Since 1.4.0 the library is split into `dsk_tools_core` + `dsk_tools` (public link), the plugin links only `dsk_tools`.

MSVC 2015 is no longer supported (dsk_tools does not build with it without local patches).

dsk_tools CLI tools disabled via `ENABLE_DSK_TOOLS=OFF`.

## Dependencies

Release build links `MSVCP140.dll`, `VCRUNTIME140.dll` (x64 also `VCRUNTIME140_1.dll`), `api-ms-win-crt-*.dll`, `KERNEL32.dll`. Requires the VC++ Redistributable 2015–2022, version 14.44 or newer, of the same bitness as the DLL.

## Debugging FAR plugin load issues

- Run FAR with `/debug` flag to see plugin loading errors
- Check FAR crash logs in `%APPDATA%\Far Manager\Profile\CrashLogs\`
- Verify DLL exports with `dumpbin /EXPORTS` (VS tool) — all 19 must be present
- Check DLL dependencies with `dumpbin /DEPENDENTS` — missing runtime DLLs silently prevent loading
- The `.def` file must be in `add_library()` for CMake to pass it to the linker
