cmake_minimum_required(VERSION 3.25)
project(MyFarPlugin VERSION 1.0.0 LANGUAGES CXX)

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
    src/plugin.def
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

if(MSVC)
    target_compile_options(plug PRIVATE /W4)
else()
    target_compile_options(plug PRIVATE -Wall -Wextra)
endif()
