# Renderer efficiency validation — 0.3.28

Validated on AYN Thor on 2026-11-01. The SM-A528B from the original 0.3.21 report was unavailable.

## Changes checked

- Software GP0 clears use contiguous wrapped row spans; empty clears preserve the scanout revision.
- OpenGL skips redundant window-context binding.
- Vulkan waits once, rather than twice, on the preceding submission before recording the next frame. Required resource, producer and presentation synchronization remains intact.
- Opt-in native logs distinguish command uploads, window handling and `eglSwapBuffers`.

The discarded command-buffer orphaning experiment is **not** in this APK.

## Tests

427 core and 193 frontend unit tests passed. All 23 unique device tests passed after correcting the new timing test's assumptions about static boot screens and its initially silent startup window.

The device checks include draw/transfer ordering against independent software pixels; native and HD raster validation; movie filtering/crop/mode changes across software/OpenGL/Vulkan; 1×/3×/5× shared-frame allocation checks; eight SPU streaming regressions; native timing paths/thread IDs; log export; twelve renderer lifecycle sessions; shader toggles/subframes; and restoring a static picture after BFI.

### Owned Crash Bandicoot 2 startup

Each renderer ran a fresh 30-second session with isolated settings and memory cards. The final five seconds supplied the timing sample, after nonzero guest audio and changing pictures had started. The offscreen output was 640×480; internal scales were 1× and 4×. Actual display APIs were asserted, rather than inferred from requested settings.

| Renderer setting | Internal scale | Guest-frame completion rate | Audio underruns / refills / dropped frames |
| --- | --- | --- | --- |
| Software | 1× | 59.830 FPS | 0 / 0 / 0 |
| Software | 4× | 59.978 FPS | 0 / 0 / 0 |
| OpenGL | 1× | 60.010 FPS | 0 / 0 / 0 |
| OpenGL | 4× | 59.778 FPS | 0 / 0 / 0 |
| Vulkan | 1× | 59.789 FPS | 0 / 0 / 0 |
| Vulkan | 4× | 59.828 FPS | 0 / 0 / 0 |

These short samples have frame-boundary variation. Completions include retained static frames; they are not physical display timestamps. Each full session consumed 394–400 changed image buffers and generated over 1.1 million nonzero audio samples. HD uses the engine's shared HD drawing path, including when selected under Software.

### Focused measurements

- 1,000 software 320×240 clears: approximately **58 ms before → 9 ms after** on Thor. This measures that operation only, not total emulator CPU or FPS improvement.
- The Vulkan regression observed **two** fence-wait API calls for one shared frame before the fix and **one** afterwards.
- Across twelve software/OpenGL/Vulkan restarts, no session workers remained alive. PSS was 63,701 KiB after warm-up and 64,020 KiB at the final round (+319 KiB); native heap allocation fell. This finite test does not prove the absence of every possible leak.

## Update artifact

- Package: `com.nanodata.armsx`, non-debuggable, PSX Lab engine only.
- Version: `0.3.28` / `93486624`.
- APK: `ArmSX1-0.3.28-renderer-efficiency.apk`, 63,284,751 bytes.
- SHA-256: `2284f81328f08805b57024446e54426773e3c1a85d2a90be3c7c2625c508fefb`.
- Release certificate matched the installed 0.3.27 APK; update identity and 16 KiB ZIP alignment checks passed.
- Installed over Thor's existing application. Player memory-card hashes stayed unchanged. Test roots were isolated, the launcher-less instrumentation helper was removed, and the app was returned to the library. Pixel was not used.

## Remaining confirmation

Retest the affected game on the SM-A528B at its original 4× OpenGL settings and physical output resolution. These tests establish correctness and remove identified overhead; they do not establish that the original scene's long GPU waits have disappeared. A fresh logger report can distinguish upload, window and swap waits, alongside CPU/frame/audio data.
