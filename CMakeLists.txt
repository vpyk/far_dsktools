# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Viktor Pykhonin
# Part of DskTools, a FAR Manager plugin for disk images of retro computers

cmake_minimum_required(VERSION 3.25)

# The version lives in src/version.hpp alone (GetGlobalInfoW takes it from there)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.hpp" _version_lines REGEX "^#define PLUGIN_VERSION_")
foreach(_part MAJOR MINOR BUILD)
    string(REGEX MATCH "PLUGIN_VERSION_${_part} +([0-9]+)" _match "${_version_lines}")
    if(NOT _match)
        message(FATAL_ERROR "src/version.hpp: PLUGIN_VERSION_${_part} not found")
    endif()
    set(_version_${_part} "${CMAKE_MATCH_1}")
endforeach()
project(DskTools VERSION ${_version_MAJOR}.${_version_MINOR}.${_version_BUILD} LANGUAGES CXX)
message(STATUS "DskTools ${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── FAR3 SDK path ──────────────────────────────────────────────
set(FAR_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FarPluginSDK"
    CACHE PATH "Path to FAR3 SDK (directory containing Headers.c)")
if(NOT EXISTS "${FAR_SDK_PATH}/Headers.c/plugin.hpp")
    message(FATAL_ERROR "FAR3 SDK not found: ${FAR_SDK_PATH}/Headers.c/plugin.hpp")
endif()

# ── Plugin library ─────────────────────────────────────────────
add_library(plug SHARED
    src/plugin.cpp
    src/cpm_geometry.cpp
    src/plugin.def
    src/version.rc
)

target_include_directories(plug PRIVATE
    "${FAR_SDK_PATH}/Headers.c"
    src
)

set_target_properties(plug PROPERTIES
    PREFIX ""
    OUTPUT_NAME "DskTools"
)

# ── dsk_tools (retro disk image support) ───────────────────
set(ENABLE_DSK_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dsk_tools)
target_link_libraries(plug PRIVATE dsk_tools)
# Third-party headers: no /W4 warnings from them in plugin code
set_target_properties(dsk_tools dsk_tools_core PROPERTIES SYSTEM TRUE)

# Embed dsk_tools diskdefs (CP/M and FAT geometries) — same approach as fddconv
set(DSK_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dsk_tools)
file(READ ${DSK_TOOLS_DIR}/src/diskdefs DISKDEFS_CONTENTS)
configure_file(
    ${DSK_TOOLS_DIR}/utils/diskdefs_embed.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/diskdefs_embed.h
    @ONLY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DSK_TOOLS_DIR}/src/diskdefs)
target_include_directories(plug PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# ── Language files ─────────────────────────────────────────────
# PSI.GetMsg takes strings by index: every .lng must have the labels //[Mxxx] of
# enum LngId (src/lang.hpp) in the same order, one string after each
set(LNG_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DskToolsEng.lng
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DskToolsRus.lng
)
file(STRINGS src/lang.hpp LNG_IDS REGEX "^ *M[A-Za-z0-9_]+,")
list(TRANSFORM LNG_IDS REPLACE "^ *(M[A-Za-z0-9_]+),.*" "\\1")
list(LENGTH LNG_IDS LNG_COUNT)
foreach(lng ${LNG_FILES})
    file(STRINGS ${lng} labels ENCODING UTF-8 REGEX "^//\\[M[A-Za-z0-9_]+\\]")
    list(TRANSFORM labels REPLACE "^//\\[(M[A-Za-z0-9_]+)\\].*" "\\1")
    file(STRINGS ${lng} texts ENCODING UTF-8 REGEX "^\"")
    list(LENGTH texts text_count)
    if(NOT labels STREQUAL LNG_IDS OR NOT text_count EQUAL LNG_COUNT)
        message(FATAL_ERROR "${lng} does not match enum LngId in src/lang.hpp")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/lang.hpp ${LNG_FILES})

# Help (F1 on the panel shows the topic @Contents of the .hlf in FAR's help language)
set(HLF_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DskToolsEng.hlf
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DskToolsRus.hlf
)

# FAR looks for them next to the DLL
add_custom_command(TARGET plug POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LNG_FILES} ${HLF_FILES} $<TARGET_FILE_DIR:plug>)

if(MSVC)
    target_compile_options(plug PRIVATE /W4)
else()
    target_compile_options(plug PRIVATE -Wall -Wextra)
endif()
