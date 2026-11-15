# FAR Manager 3 plugin SDK headers

C/C++ headers (`Headers.c/`) of the FAR Manager 3 plugin SDK, API version 3.0.0 build 6666 (`VS_RELEASE`), unmodified.
Pascal headers are not needed and omitted.

Upstream: https://github.com/FarGroup/FarManager (`plugins/common/unicode`); the same headers ship in the `PluginSDK` folder of FAR distributions.

Checked against upstream release `ci/v3.0.6744.5019` (2026-10): its headers are API build 6682 and differ only by the new `FMVT_NEXT` macro variable type, which the plugin does not use. The plugin's `MinFarVersion` does not depend on these headers (`PLUGIN_MIN_FAR_BUILD` in `src/version.hpp`), so they may be updated; this file is the only one of the folder not from upstream.
