# PSX Lab media, audio and presentation fixes

The Android host continues to use the PSX Lab CPU, GPU, GTE, CD-ROM and SPU. The
shader runtime only processes completed images. No SwanStation core is packaged.

## Disc loading

`ArmSxBackend.openMedia` recognizes staged CUE files and mounts the referenced BIN
tracks through `CueDisc`, rather than trying to parse the CUE text as ISO sectors.
Track indexes, stored/generated pregaps, leadout and CDDA reach the CD controller.
Binary AUDIO, MODE1/2048, MODE1/2352, MODE2/2048, MODE2/2336 and MODE2/2352 tracks
are supported. Nonbinary audio files and invalid/missing tracks produce an error.
The ISO boot reader still expects the PlayStation data filesystem in track 1.

`PbpDisc` reads unencrypted PS1 PSISOIMG data, raw or deflated 16-sector blocks,
and embedded multi-disc tables. Launch currently selects the first embedded disc;
the reader's indexed constructor supports other discs, but there is no new menu for
switching embedded PBP discs. PSP applications and encrypted PSN containers are
rejected explicitly. PBP verification uses generated compressed, raw, multi-disc
and corrupt fixtures; no supplied commercial PBP was available for a full-game test.

## Audio and widescreen

Fast-forward respects the existing mute choice and fast-forward volume. Audible
fast-forward uses the existing SoundTouch processor with a tempo multiplier while
Android output remains 44.1 kHz. Normal-speed processing remains the same. Uncapped
fast-forward uses a bounded 16x audio tempo; it cannot follow unlimited CPU speed.

Aspect ratio, stretch and integer scaling configure the native software display,
OpenGL display, Vulkan upload path and HD shared-image path. Changing them invalidates
cached static pictures. The widescreen hack adjusts GTE horizontal perspective
projection around its existing centre in Java, native GTE and native CPU paths;
it is independent of the display aspect ratio. Games can still have their own
visibility culling and 2D elements, which this projection adjustment does not rewrite.

## Shared shader presentation

Software, OpenGL and Vulkan game rendering can feed the same Vulkan postprocessing
stage. Software keeps its CPU rasterizer; OpenGL keeps its integer/HD rasterizer.
OpenGL native frames use a shared hardware buffer where the driver supports it,
with a CPU transfer fallback. Shader processing does not emulate another game frame.
With shaders disabled, software/OpenGL return to their ordinary display path and
the optional runtime is not loaded on a fresh launch. A usable Vulkan driver is
required for shaders even when software or OpenGL game rendering is selected.

The bridge adapts this repository's `frontend/render_shaders.cpp`. It loads the
pinned librashader 0.12.0 C API on demand. Chain compilation uses a worker; the
GPU worker owns rendering, parameters and teardown. Queue access is serialized
with compilation. Shader source/lookup textures must be accessible local files.
Unreadable or invalid presets fall back to a plain image and report their status.

HD inputs are downsampled to native game dimensions for the CRT-oriented shader
path, then processed at the aspect-correct display size. This means a general
sharpening shader is not a replacement for HD game rasterization.

Build the optional Android runtime on Windows with:

```powershell
./tools/Build-PsxShaderRuntime.ps1
```

This extracts the vendored C API crate, uses its Cargo.lock and builds only the
Vulkan runtime with the Android NDK and writes shared libraries under
`psxlab/backend/src/main/jniLibs/arm64-v8a`. The pinned ARM64 shader
and matching Android C++ runtime binaries are tracked there so a fresh checkout can
package shaders without first installing Rust. The script regenerates those binaries.
The APK includes notices;
the corresponding librashader source crates are under `third_party/librashader/source`.

## Black-frame insertion and shader subframes

The saved choice is separate from eligibility. At 2x, a 60 Hz game needs a display
near 120 Hz and a 50 Hz game needs one near 100 Hz. Multipliers 3 and 4 require their
corresponding refresh rates. The engine checks the actual display refresh supplied
by the host, not just the requested mode. Settings and diagnostics explain inactivity.

BFI posts real black buffers after each game image on software, OpenGL or Vulkan
presentation. Shader subframes render the same input again with distinct
`TotalSubFrames`/`CurrentSubFrame` values. A subframe-aware preset is needed for a
visible difference. Both features are off by default and suspend during pause and
fast-forward. Pausing or disabling BFI restores the ordinary picture, including
static HD scenes. Guest FPS counts emulated fields, not black or shader presents.

## Verification and logging

Core JVM tests cover CUE layout/ownership, CDDA controller routing, PBP decompression,
GTE projection and refresh eligibility. Android tests exercise media mounting and
original executable execution, file-descriptor cleanup, real-game fast-forward audio,
native CPU/GTE parity, and aspect changes on all three presenters at 1x/3x.

`OffscreenPresentationTest` reads real posted buffers through ImageReader without
flashing the physical display. It checks BFI restoration, shader parameters, distinct
subframe colours, image orientation, renderer handoff, disable and invalid-preset
fallback. Hardware coverage is Thor/Adreno 740, not a guarantee for every driver.

Each launch resets the last CPU/audio/PC diagnostics before opening media, preventing
failed launches from inheriting a previous session's status. Reports include display
geometry, actual refresh, presentation eligibility, shader status and audio tempo/gain.
Frame records label requested presentation counts separately from API acceptance and
include paths/timings for BFI and shader subframes. API completion is not a physical
scanout timestamp or proof that every submitted buffer reached the panel.
