// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Viktor Pykhonin
// Part of DskTools, a FAR Manager plugin for disk images of retro computers

#ifndef DSKTOOLS_VERSION_HPP
#define DSKTOOLS_VERSION_HPP

// The one place of the plugin's version: CMakeLists.txt reads these three numbers for
// project(), version.rc for the VERSIONINFO resource (an include guard, not #pragma once,
// for the resource compiler)
#define PLUGIN_VERSION_MAJOR 0
#define PLUGIN_VERSION_MINOR 9
#define PLUGIN_VERSION_BUILD 0

// The oldest FAR the plugin runs on, fixed apart from the SDK headers (build 6666), so that
// updating them does not raise it. Every declaration the plugin uses from plugin.hpp is as in
// 6666 since build 3604 (2013-08: the Instance field of GlobalInfo and the other export
// structures, which GetGlobalInfoW writes); 3800 is the stable build it was tested on.
#define PLUGIN_MIN_FAR_BUILD 3800

#endif
