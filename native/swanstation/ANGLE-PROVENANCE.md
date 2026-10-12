# ANGLE input for local dev tests

Official AOSP input: https://android.googlesource.com/platform/packages/modules/ANGLE

Pinned submodule revision: `9a0834cc8a5f3427dae2f6e87de4dfae50cb781c`.
Only `lib/arm64/libEGL_angle.so` and `lib/arm64/libGLESv2_angle.so` are staged.
Build-SwanCore.ps1 checks the revision and exact SHA-256 of each input.
Nothing is imported from the current-DuckStation private build.

These binaries passed Vulkan-backed GLES tests on AYN Thor, Android 13/API 33.
Their ELF LOAD alignment is 4 KiB; compatibility with 16 KiB-page Android devices
is NOT established. Initialization failures fall back to system GLES. ANGLE uses
its own system Vulkan loader; the native-Vulkan custom driver selection is not
injected into ANGLE.

The pinned prebuilt checkout has no root NOTICE file. This provenance record is
not a substitute for complete source/dependency notices or distribution rights.
Keep public release blocked until these, frontend/assets rights, and device
compatibility have been audited. Do not claim universal Android support.
