# ARMSX1 — RetroArch (slang) shader chains: STATUS

Status date: 2026-07-31. Third session. **The feature is implemented and wired end to end.**

Previous versions of this file said "no source file has been modified yet". That has been
false since session 2; it is now false by a very long way. What follows is the real state.

---

## 1. TL;DR

| Piece | State |
|---|---|
| librashader vendored (0.12.0, arm64) | DONE (session 2) |
| `frontend/render_shaders.{h,cpp}` | DONE (session 2), **compile-fixed + gated** (session 3) |
| Build wiring (`Makefile`, `build.sh`) | **DONE (session 3)** |
| Vulkan present seam (`render_vk.cpp`) | **DONE (session 3)** |
| JNI (3 entry points) | **DONE (session 3)** |
| `NativeApp` stubs → `native` | **DONE (session 3)** |
| `Settings.applyTo()` live push | **DONE (session 3)** |
| Bundled CRT presets | **DONE (session 3)** — ARMSX-original, 4 presets |
| First-run install of the bundled pack | **DONE (session 3)** |
| On-device verification | see §9 |

Shaders are **off by default** (`Settings.shaderChainEnabled = false`).

---

## 2. Where the shader sits relative to internal-resolution upscaling

**THE DECISION: the chain reads the frame at NATIVE PlayStation resolution and writes at
the aspect-corrected DISPLAY size, regardless of `[video] internal_scale`.**

Two facts force it:

1. A CRT shader's scanline count comes from `SourceSize.y`. Feed it a 2x frame and it draws
   480 scanlines instead of 240; at 3x, 720. Those land on a fractional number of screen
   pixels and smear into moire. A CRT chain is simply *wrong* on an already-upscaled image
   — RetroArch's own answer is "use 1x with CRT shaders".
2. The mask/scanline geometry must be generated at DISPLAY pixel density or the presenter's
   own scale-up smears it. So the chain's *output* is the letterbox rect, not the frame.

```
frame_image (nativeW*S x nativeH*S)
  --(linear downsample by S, only when S > 1)--> chain input (nativeW x nativeH)
  --(librashader, viewport = the letterbox rect)--> chain output (dst_w x dst_h)
  --(1:1 blit)--> swapchain at (dst_x, dst_y)
```

Per renderer setting:

* `renderer = "software"` (S == 1): the downsample is the identity and is **skipped
  entirely**. The chain samples `frame_image` in place. Zero added cost.
* `renderer = "hardware"` at 2x/3x: the frame is downsampled to native first. This is not
  throwing the upscale away — a 3x render box-filtered to 1x is **supersampling**, so the
  CRT shader gets a cleaner, anti-aliased native image than the software rasteriser could
  ever produce. It is still a poor CPU trade on the CPU rasteriser, so the log names
  `internal_scale = 1` as the cheaper route to the same picture.

The scale hint reaches the shader layer from `frontend/config.c` via
`armsx_shader_set_source_scale()`. It is a HINT, not a contract: `SourceDivisor()` walks it
down until it divides the real frame exactly and leaves a plausible display size, so a
rasterizer that failed to install degrades to 1x instead of corrupting geometry. The chosen
divisor is logged on every change.

**Deliberately NOT offered:** a "run the chain on the upscaled frame" mode. It is only right
for sharpen/FSR-style chains, it is the setting people mis-set and then report a CRT preset
as broken, and a new `[video]` key with no Kotlin field behind it is silently dropped by
`Ps1SettingsStore` the first time anything is saved (three settings have been lost that way
in this port). If it is ever wanted it needs all four places, not just the native file.

---

## 3. How librashader is built and linked

**Built:** `third_party/librashader/build-android.sh`, by hand, NOT from `build.sh`
(`build.sh` runs under the shared `/tmp/armsx-native-build.lock` after `make clean`; a
multi-minute Rust build inside that critical section blocks every other agent).
`cargo build --release --target aarch64-linux-android --no-default-features --features
runtime-vulkan`. `--no-default-features` is mandatory: `default = ["runtime-all"]` pulls in
D3D9/11/12 + Metal + desktop GL.

**Linked: IT IS NOT.** `liblibrashader_capi.so` (13 MB) is staged into
`android/app/src/main/jniLibs/arm64-v8a/` by `build.sh` and `dlopen()`ed by SONAME on first
use. `libarmsx.so` has **no DT_NEEDED entry** for it. Reasons, in order:

1. A hard DT_NEEDED on an optional 13 MB feature means a missing librashader makes
   **libarmsx.so itself** fail to load — every JNI method vanishes and the app dies at
   `NativeApp.initialize` with an error pointing nowhere near shaders.
2. The static archive is 79 MB and this Makefile puts `-flto` in `BASE_CFLAGS`.
3. dlopen gives the degrade path this feature is required to have.

Verified: `llvm-readelf -d` on the prebuilt shows NEEDED = `libc++_shared.so`, `libdl.so`,
`libm.so`, `libc.so` — **no `libvulkan`**. librashader takes `PFN_vkGetInstanceProcAddr`
from the caller, so an adrenotools custom driver stays substitutable.
`build-android.sh` refuses to stage a build that gained a `libvulkan` NEEDED.

**Licence:** MPL-2.0 OR GPL-3.0-only; ARMSX takes **MPL-2.0**. `third_party/librashader/NOTICE`
+ `LICENSES/MPL-2.0.txt` + `third_party/librashader/source/*.crate` (the exact published
tarballs the binary was built from) satisfy §3.2 source availability and §3.4 notice.

**The hand-written header was verified against the crate, not trusted.** All 14 entry point
signatures and all 6 `#[repr(C)]` struct layouts in
`third_party/librashader/include/librashader_min.h` were checked against the extracted
`librashader-capi-0.12.0` source, and all 14 symbols confirmed present in the prebuilt `.so`
with `llvm-readelf --dyn-symbols`. `LIBRASHADER_CURRENT_VERSION == 5`,
`LIBRASHADER_CURRENT_ABI == 2`; the loader refuses a mismatched ABI rather than reading a
differently-shaped struct (silent memory corruption, not a clean failure).

---

## 4. Build wiring (session 3)

* `Makefile`: `ARMSX_ENABLE_SHADERS ?= 1`, **forced to 0 when `ARMSX_ENABLE_VULKAN` is 0**
  (librashader has no GLES runtime, and its C API is typed in Vulkan handles).
  `frontend/render_shaders.cpp` added to `CPP_SOURCES` unconditionally — it compiles to
  inert stubs without the define, exactly like `render_gl.cpp` / `render_vk.cpp`.
* `build.sh android`: passes `ARMSX_ENABLE_SHADERS`, and stages the prebuilt `.so` into
  jniLibs. If the prebuilt is missing it **builds with shaders compiled out** rather than
  shipping a feature that can only fail at runtime. Turning shaders off removes a stale
  `.so` from jniLibs so it is not 13 MB of dead weight in the APK.

Header gating: `render_shaders.h` includes `<vulkan/vulkan.h>` only under
`ARMSX_ENABLE_VULKAN`, and a translation unit that only needs the control surface defines
`ARMSX_RENDER_SHADERS_NO_VK` first (`config.c`, `android_jni.cpp` both do). That is what
keeps the desktop build and `make test-gpu` compiling on a machine with no Vulkan SDK.

All four configurations were syntax-checked directly with the NDK clang:
VK on/shaders on, VK on/shaders off, VK off/shaders off, and VK off/shaders **on**
(which must self-disable via the `#undef` at the top of `render_shaders.cpp`).

---

## 5. The Vulkan present seam — what was actually inserted

Four additive edits to `frontend/render_vk.cpp` (no longer owned by another agent):

1. `#include "render_shaders.h"` **after** `<vulkan/vulkan.h>` — deliberately, because
   `render_shaders.h` includes it too but without `VK_USE_PLATFORM_ANDROID_KHR`.
2. `EnsureFrameImage()`: added `VK_IMAGE_USAGE_SAMPLED_BIT`. Vulkan's mandatory format table
   guarantees optimal-tiling SAMPLED for `R8G8B8A8_UNORM`, so this cannot narrow device
   support. Without it the S == 1 fast path could not sample the frame in place.
   *(Session 2's comment claimed this bit already existed. It did not — it was aspirational.)*
3. `armsx_render_create_vk()` success path: `armsx_shader_vk_attach(&shader_device)`.
4. `OpShutdown()`: `armsx_shader_vk_detach()` **first**, before `vkDeviceWaitIdle` /
   `vkDestroyDevice` — the chain owns images, views, framebuffers, pipelines and descriptor
   pools created against that device.
5. `OpPresent()`: the existing `vkCmdBlitImage` is now the `else` of
   `if (!armsx_shader_vk_render(&chain_frame))`. Nothing else changed — same clear, same
   barriers, one submit, one present.

The seam is **layout-transparent**: both images come back in the layouts they went in
(`TRANSFER_SRC_OPTIMAL` / `TRANSFER_DST_OPTIMAL`), including on the mid-frame failure path,
so the caller's barrier to `PRESENT_SRC_KHR` is correct whether the chain drew or not.

`armsx_shader_note_backend()` is called from `render.cpp`'s `armsx_renderer_create()` with
the backend that **actually came up**, not the requested one — so "you enabled a shader chain
on a backend that cannot run one" is said out loud once.

---

## 6. Real vs stubbed — the dominant defect class

This port's most common bug is a control that renders, toggles and reaches nothing; the
inverse (a real native with zero callers) is just as common. Both were checked here.

**Now REAL and CALLED:**

| Native | Declared | Called from |
|---|---|---|
| `NativeApp.setShaderChain(boolean, String)` | **new**, `native` | `Settings.writeGsToNative()` under `if (emitSink == null)` |
| `NativeApp.shaderPresetParams(String)` | was a stub returning `""` | `ShaderParams.read()` |
| `NativeApp.setShaderChainParams(String, String[], float[])` | was an empty stub | `ShaderParams.push()` / `pushEffective()` |

`setShaderChain` **did not exist at all** before this session — the preset path had no route
to the core. `Settings` was writing it to `setSetting("EmuCore/GS", "ShaderChainPreset", …)`,
and `NativeApp.setSetting` is itself an empty stub in this port, so the entire EmuCore INI
path is dead here. Those `put()` calls are retained ONLY because the settings export and the
fresh-install INI recovery read them back; the live path is the new native setter.

`shaderPresetParams` returns **`"[]"`** on every failure path, never `null` and never `""` —
the Kotlin caller feeds it straight to `JSONArray()`, where both throw. The old stub's `""`
logged a spurious parse warning on every call.

---

## 7. Bundled CRT presets and their licence

`android/app/src/main/assets/shaders/armsx-crt/` — **ARMSX-original, Moon-owned**:

* `armsx-crt.slang` — one-pass CRT: brightness-dependent Gaussian scanline beam, aperture /
  slot / shadow phosphor masks evaluated in physical output pixels, sharp-bilinear
  horizontal filter, barrel curvature, rounded corners, vignette, linear-light compositing
  with in/out gamma. 12 parameters in 4 caption-headed groups.
* `Sharp Scanlines.slangp`, `Aperture Grille.slangp`, `Slot Mask.slangp`, `Curved TV.slangp`
* `LICENSE.txt`

**Why original rather than bundled from libretro:** `slang-shaders` is a mixed-licence work
(mostly GPL-2.0-or-later) and **ARMSX is proprietary** (`LICENSE`: "ARMSX PROPRIETARY
LICENSE"). Redistributing GPL shader source inside a proprietary app is defensible — they
are data interpreted at runtime, not linked code — but it obliges shipping each licence
text, preserving every header and re-auditing on every update, for a handful of presets
whose only job is to make the app look right on first run. Writing them removes the question
entirely. Third-party shaders remain fully supported: `ShaderRepo` still downloads the full
libretro collection on request and imports any `.zip`/folder. That is the user's own copy
under their own terms; ARMSX does not redistribute it.

Installed on first sight by `ShaderRepo.installBundled()`, called from `scanShaderPresets()`
and the manager's refresh (both already on `Dispatchers.IO`). Idempotent — one stat of a
`.armsx-bundled` stamp once written. A user who deletes the pack keeps it deleted; only a
`BUNDLED_VERSION` bump brings it back.

---

## 8. Degradation, and where it is logged

Every failure returns `false` from `armsx_shader_vk_render()` and the caller's ordinary blit
runs. Nothing is ever silent — all of these log through `armsx_render_log("shaders", …)`,
which reaches `files/logs/armsx.log`:

* librashader `.so` absent / `dlopen` fails
* a missing entry point, or an ABI that is not 2
* preset unreadable, or fails to compile (with librashader's own message)
* a chain error mid-frame (marks the chain failed, restores layouts, falls back)
* a non-Vulkan present backend with a chain enabled (said once)
* the chosen input resolution and divisor, on every change

---

## 9. Verification

Build: `./build.sh android` green under the shared lock; `libarmsx.so` timestamp advanced.
`make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` 13/13.
`llvm-readelf -d bin/libarmsx.so` — **no `libvulkan`, no `librashader`** in NEEDED.

On-device (Retroid Pocket 6, Adreno 740, `com.nanodata.armsx`, debuggable so `run-as`
works). See §10 for the measured cost.

**How to test by hand:**
1. `[video] gpu_backend = "vulkan"` in `files/settings.toml` — the chain needs Vulkan.
2. Launch a game by TAPPING ITS COVER in the library (an `am start … VIEW` intent kills the
   process).
3. Pause > Graphics > Shader chain: toggle on, pick `Armsx CRT` > `Slot Mask`.
4. `run-as com.nanodata.armsx cat files/logs/armsx.log | grep shaders`
   Expect: `librashader loaded (ABI 2, API 5)`, `chain loaded: … (compiled in N ms)`,
   `chain input 320x240 (native), output WxH`.

---

## 10. Measured cost — on-device, Adreno 740

Measured by linking **the real `frontend/render_shaders.cpp`** into a harness
(`lrs_frame.cpp`, in the session scratchpad) and driving it exactly as `render_vk.cpp` does:
headless VkDevice, source in `TRANSFER_SRC_OPTIMAL`, target in `TRANSFER_DST_OPTIMAL`, one
`armsx_shader_vk_render()`, one `vkQueueSubmit`. Not a reimplementation — the actual
barriers, downsample, chain call and output blit. 320x240 source, 1920x1080 target, 4:3
letterbox 1440x1080. Best of 60 runs after 10 warmup, wall-clock submit+wait.

| Configuration | Frame | Δ vs plain blit |
|---|---|---|
| No chain (plain `vkCmdBlitImage`) | 0.196 ms | baseline |
| Sharp Scanlines, internal_scale 1 | 0.796 ms | **+0.60 ms** |
| Slot Mask, internal_scale 1 | 0.845 ms | **+0.65 ms** |
| Curved TV, internal_scale 1 | 0.864 ms | **+0.67 ms** |
| Slot Mask, internal_scale 4 | 0.907 ms | **+0.71 ms** |

**~0.65 ms, i.e. about 4% of a 16.7 ms frame at 60 Hz.** The internal-scale-4 row costs
~0.06 ms more than scale 1 — that is the 1280x960 → 320x240 downsample blit, and it is the
entire price of feeding the chain a native-resolution image from an upscaled frame.

Chain build (shader compile through glslang + SPIRV-Cross + pipeline creation) is **~110 ms
for the first preset and ~15 ms for each one after** — a one-off stall when the user picks a
preset, not a per-frame cost.

Correctness was checked by reading the target back: all presets produce a real picture
(≈100% of sampled pixels lit). "Curved TV" reads 95% rather than 100%, which is the
curvature and rounded corners blacking out the corners — i.e. positive confirmation that
the geometry path runs.

### Degradation was control-tested, not assumed

| Case | Result |
|---|---|
| Preset whose shader fails to compile | logged with librashader's own message and line number, **0 chain frames, 70 plain blits**, 0.204 ms (= baseline), picture correct |
| Preset file that does not exist | logged `IOError … NotFound`, **0 chain frames, 70 plain blits**, 0.200 ms, picture correct |

Both log exactly once (the `failed` flag stops a retry every frame) and neither produces a
black screen.

### What was verified where

* `lrs_probe` (device): dlopen, all 14 entry points bound, ABI 2 / API 5, all four presets
  parse, 12 controls + 4 captions each, per-preset parameter overrides applied.
* `lrs_chain` (device): headless VkDevice, `libra_vk_filter_chain_create` succeeds on
  Adreno 740 for every preset; `set_param` works; `set_param` on a caption is tolerated.
* `lrs_frame` (device): the table above.
* `glslc` (host, NDK `shader-tools`): both stages of `armsx-crt.slang` compile standalone.

The probes are still on the device at `/data/local/tmp/armsxshader/` and are re-runnable:

```sh
adb shell "cd /data/local/tmp/armsxshader && LD_LIBRARY_PATH=. \
  ./lrs_frame '/data/local/tmp/armsxshader/armsx-crt/Slot Mask.slangp' 1"
```

---

## 11. Known follow-ups (not blockers)

* **`ui/emulation/EmulationMenuScreen.kt` carries a now-FALSE user-visible notice**
  ("Shader presets can be downloaded and selected, but the ARMSX1 core does not render them
  yet") plus the comment above it saying the natives are still stubs, around lines 968–993.
  That file was owned by another agent this session and was deliberately not touched. It
  must be deleted now that the chain renders.
* `ShaderChainSection.kt` has an unused `import com.armsx2.ShaderParams` (dead import).
* The chain requires Vulkan. If the GL/ANGLE path ever becomes the default on this device,
  the honest options are to keep saying so in the log or to port librashader's GL runtime
  with `glsl_version = 0` (ARMSX2 does this, at the cost of a full GL state-cache repair).

---

## 12. Dead ends — do not retry

* **Do not target GL/GLES/ANGLE.** librashader has no GLES runtime; its GL runtime emits
  desktop GLSL and uses desktop-only entry points. ANGLE's GLES 3.1 does not close that gap.
* **Do not link the static archive.** 79 MB into an `-flto` link.
* **Do not add librashader as a DT_NEEDED of `libarmsx.so`.**
* **Do not build librashader with default features.**
* **Do not put `cargo build` inside `build.sh android`.**
* **Do not route shader settings through `NativeApp.setSetting("EmuCore/GS", …)`.** Empty
  stub; the whole EmuCore INI path is dead in this port.
* **Do not write a `.slangp` writer for preset saving.** `ShaderParams.savePreset` already
  emits the correct RetroArch simple-preset form (`#reference` + `Locale.US` `%.6f` lines).
* **Do not try to generate `librashader.h` with cbindgen.** Not installed, not in the crate.
