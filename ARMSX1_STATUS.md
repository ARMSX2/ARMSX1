# ARMSX1 — project status and handoff

Written 2026-07-31 03:20. Purpose: let a fresh engineer, or a future session with no
memory of this one, pick this up without re-deriving anything.

**This directory is NOT a git repository.** There is no version-control safety net and no
history to bisect. Be incremental and verify as you go.

---

## 0. What this project is

ARMSX1 is a PlayStation 1 emulator for Android. It is ARMSX2's (PS2 emulator) entire
Jetpack Compose UI, lifted wholesale, with the PCSX2 core swapped for a PS1 core (a `psxe`
fork). FSUI/ImGui was cut from the front-end entirely; the UI is Compose, driven over JNI.

- Native PS1 core: `psx/` (C99). Software rasteriser at `psx/dev/gpu.c`.
- Front-end: `frontend/` (C++20). `main.cpp` is the session/loop; no ImGui anywhere.
- In-process Android JNI host: `frontend/android_jni.cpp`.
- Android app: `android/app/src/main/`, Compose UI under `java/com/armsx2/`.
- JNI facade: `java/kr/co/iefriends/pcsx2/NativeApp.java`.

### THE dominant bug class in this port — read this first

`NativeApp.java` has ~133 methods. **Most are deliberately inert stubs** so the lifted PS2
UI compiles and runs. A stub returns a safe default and does nothing. The consequence:

> A lifted control renders, animates, toggles and persists — and reaches nothing.

Nearly every defect found in this session was an instance of this, on one side or the other:

| Symptom | Actual cause |
|---|---|
| Physical pads dead, touch fine | `setPadButtonForPort()` stub; touch called the real `setPadButton()` directly |
| Save/Load State "doesn't work" | `getGamePathSlot()` stub returned `""` → every slot read empty → Load permanently disabled |
| Pause didn't pause | `vmSetPaused` (PCSX2 `Host::OnVMPaused`) had **zero callers** |
| Fast-forward did nothing | ended at `speedhackLimitermode()`, an empty stub |
| FF still capped after that fix | `armsx_renderer_set_vsync()` existed with **zero callers** |
| OSD empty | `getFPS`/`getNominalFrameRate`/`getPresentedFrameCount` all stubs returning 0 |
| Display FPS cap dead | `setSetting("EmuCore/GS", "FrameLimitEnable")` + `speedhackLimitermode()` stubs |
| Renderer picker inert | in-game picker wrote `Settings.renderer` (PCSX2 `EmuCore/GS/Renderer`), never read |
| Custom drivers absent | `setCustomVulkanDriver` was an inert **Java** no-op; whole manager terminated in nothing |
| RA library sync silently empty | `RaLibrary.kt` asked RetroAchievements for console **21** (PS2) not **12** (PS1) |

**Rule: whenever something "does nothing", grep the call site AND confirm the method it
calls is `native`, not a stub. Then check the inverse — real natives with zero callers.**

Second rule, learned the hard way: **String stubs must return `""`, JSON-shaped `"{}"`,
arrays empty — NEVER null.** A null from these crashed the whole app on the achievements
screen. Preserve that contract.

---

## 1. Build

```bash
# Native (libarmsx.so). Stages into android/app/src/main/jniLibs/arm64-v8a/.
cd /Users/jpolo1226/Downloads/armsx
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/29.0.14206865"
export PATH="$ANDROID_HOME/cmake/3.30.5/bin:$PATH"   # cmake is NOT on a normal PATH
./build.sh android

# App
cd android
export ARMSX_SKIP_NATIVE_PREPARE=1   # else gradle re-runs build.sh behind your back
./gradlew :app:assembleGithubDebug   # -> app/build/outputs/apk/github/debug/app-github-debug.apk
```

Gotchas that cost time:

- **Flavor task names.** The app has a `store` flavor dimension (`github` = sideload APK with the
  in-app updater, `play` = updater-free, for an app bundle). `assembleDebug` is now an *aggregate*
  that builds both, and there is no `apk/debug/app-debug.apk` any more — the outputs are
  `apk/github/debug/app-github-debug.apk` and `apk/play/debug/app-play-debug.apk`. Installing from
  the old path installs whatever stale APK was left there before the split.

- **`cmake` lives inside the Android SDK**, not on PATH. Without it `build.sh` dies with a
  bare `cmake: command not found` *after* printing its NDK banner, which reads like success.
- **`build.sh` can exit 0 after a failed sub-step.** Always check the tail for errors AND
  that the `.so` timestamp actually advanced.
- **`build.sh` starts with `make clean`.** Two concurrent native builds genuinely corrupt
  each other. If more than one agent/person may build, serialise on a lock:
  `while ! mkdir /tmp/armsx-native-build.lock 2>/dev/null; do sleep 20; done` … `rmdir` it
  after, **even on failure**.
- Gradle 9 removed `Project.exec()` from task actions. `app/build.gradle`'s
  `prepareNativeBinaries` was rewritten to use injected `ExecOperations`, and now also puts
  the SDK's cmake on PATH itself.
- AGP 9 has built-in Kotlin — do **not** apply `org.jetbrains.kotlin.android`, only
  `org.jetbrains.kotlin.plugin.compose`.
- **`libvulkan` must NEVER become a `DT_NEEDED` dependency** — Vulkan is loaded dynamically
  so custom drivers can be injected. Verify with `llvm-readelf -d` after touching the
  Vulkan backend.

Device: Retroid Pocket 6 (Adreno 740) on adb. Crash Bandicoot at
`/storage/EAB4-B680/ROMs/PS1/Crash Bandicoot (USA)/Crash Bandicoot (USA).cue`.
**Launch games by tapping the cover in the library** — an `am start … VIEW` intent into the
running app kills the process (activity recreate → `killProcess`).

---

## 2. State as of this writing

Build is **green**, both native and app, and installed on the device.

### Recovery note (09:30) — five agents were killed mid-edit by a spend limit

They died between writing a header and finishing its caller. One real breakage resulted and
is fixed: `psx/state.c` called `psx_thumbnail_capture_png()` with 3 args (the declaration
takes 5) and a `psx_thumbnail_png_size()` that was never written. The capture already
reports width/height through out-params, so the second call was never needed.

**Three "regressions" reported against the 03:23 build were not regressions** — that APK
was built against a `.so` containing the agents' half-finished native work. After repairing
and rebuilding, all three were re-verified on hardware:

| Reported | Actual state after rebuild |
|---|---|
| RP6 controller not working | **Works.** `sendevent` BTN_SOUTH on `/dev/input/event7` → `ARMSX-PAD: key id=8 code=96 -> pad 96 port=0 KeyDown` → `setPadButton code=96` |
| Pause doesn't pause | **Works.** Two screenshots 3s apart while paused differ by **0 bytes** across 8.3 MB of raw pixels |
| Aspect ratio | **Works.** `aspect=1.3333 stretch=false dst=240,0 1440x1080` — exact 4:3 pillarbox |

Method note worth repeating: an early controller test showed nothing and looked like a
genuine failure. It was invalid — a tap had opened the Discord *Friends* dialog, and a
Compose dialog takes its own focused window and eats pad keys. **Always control-test a
negative.** `getevent` confirmed the injected event reached the input stack, which is what
proved the fault was elsewhere.

### 09:58 — the table above was WRONG. Two bad tests, and the real cause.

The user retested on hardware and input, pause and restart were all still broken. Both of
my "verifications" were measuring the wrong thing:

1. **"Pause works — 0 bytes differ over 3s."** A static image is equally consistent with a
   *dead or stalled* VM. The test cannot distinguish paused from broken. Worthless as
   written.
2. **"The `armsx-vm` thread is gone."** Also wrong — I had piped `top -H` through
   `tail -14`, which truncated the thread list. Reading `/proc/<pid>/task` directly shows
   `armsx-vm` alive and accumulating ~2.1s CPU per 3s wall. Emulation was running the
   whole time.

**Actual root cause of the dead input:** `analog_mode_default` defaulted to **true**, so the
emulated pad reported model **0x73 (DualShock)** at boot —
`core: pad boot mode=analog (analog_mode_default=true)` in logcat. Crash Bandicoot (1996)
predates the DualShock and mis-handles a pad that identifies as one, which kills **both**
the touch overlay and a physical pad, because the fault is in the pad *identity*, not the
input plumbing. The agent that added the setting predicted this exact symptom for pre-1997
titles and it was shipped default-on anyway.

**Fixed:** default is now digital in `frontend/config.c` (template + `psxe_cfg_load_defaults`)
and `Ps1Settings.kt`. The toggle remains for anyone who wants analog, and the live
toggle (pad code 200) is unaffected.

**Also fixed:** the OSD was invisible because *every* stats row defaults to false. `osdShowFps`
and `osdShowHardwareInfo` (the renderer row) now default true, so "which renderer am I on"
is answerable without hunting through menus.

**Still unverified / open:** that button presses actually register in digital mode (the
screen animates, which only proves the VM is live); "restart kicks to library"; and whether
the display-mode picker now applies. Do not mark these done without a real test.

### Working and verified on hardware

- Boot logo, save/load state, physical controllers, analog sticks, pause, fast-forward,
  OSD, Vulkan present at correct 4:3, library with psx-covers art, memory cards.

### Landed, needs on-device verification

- ANGLE selection, custom Vulkan driver loading, unified renderer pickers, live
  aspect/stretch, audio settings, FF speeds, frame limiter / FPS cap, OSD subsystem stats,
  save-state thumbnails, PS2 settings purge.

### In flight when this was written

- **Hardware rasteriser GPU backend** — see `frontend/HW_RENDERER_DESIGN.md` §0.5.
- **librashader / CRT shaders** — see `frontend/SHADERS_STATUS.md`.
- **RetroAchievements softcore** — see `frontend/RETROACHIEVEMENTS_STATUS.md`.

---

## 2.4 SUPERSEDES §2.5 BELOW — GPU rasterizer session (12:15)

> **⚠ Two of this section's headlines are now stale. The live checkpoints are
> `frontend/HW_RENDERER_DESIGN.md` §0.5.4, §0.5.5 and §0.5.6 — §0.5.6 is the newest.**
> * §4.6's **automatic downgrade is landed and proven on hardware** (§0.5.6): a running average
>   of GP0(C0) readback bytes over 60 frames above 256 KB/frame seeds host VRAM from the render
>   target, logs loudly and hands the session to a CPU rasterizer. Fired on device from real
>   game traffic (`hwgl_c0_trip`) and from a forced trigger (`hwgl_force_downgrade`), and
>   control-tested *not* to fire on a normal session. The GPU→host readback it needed
>   (`gl_readback_rect()`) is §4.3's stall, now implemented.
> * The software shadow is **still on**. §4.3 row 1 (a textured draw sampling a region the GPU
>   drew into) is the one piece still missing before it can be dropped.
> * "1x is NOT pixel-identical" below was **fixed** (§0.5.4): 0.2129% → **0.0049%** on the boot
>   window, 0.2981% → **0.0003%** on the title, `blank` and `extra` driven to 0. The 0.47%
>   residue on gameplay is the documented multi-blend precision divergence, not a defect.
> * Scanout no longer reads the frame back at all when the present backend is OpenGL (§0.5.5,
>   the brokered seam): the rasterizer hands its resolve texture straight to the present layer.
>   Worth −0.6 ms of `emu` at 1x, −0.76 at 2x, −1.03 at 3x (−2.56 in the heaviest band), and
>   +7–32% uncapped throughput at 3x at matched workload.

§2.5 was written at 10:35 and is **wrong about the tree state**. Read
`frontend/HW_RENDERER_DESIGN.md` **§0.5.3**, which is the live checkpoint. Headlines:

- `frontend/gpu_hw_gl.c` is **complete** (2520 lines), in the `Makefile`, wired into
  `main.cpp`, and **works on hardware** — not "a header exists". §2.5 understated it badly.
- **The app is `com.nanodata.armsx`, not `com.armsx2`** (that is the PS2 app, also installed).
  It is **DEBUGGABLE**, so `run-as` works and `files/settings.toml` can be driven from adb.
  §0.5.1's "no measurements are possible" blocker was checking the wrong package.
- **Measured on the Retroid Pocket 6 (Adreno 740).** GLES 2x costs the CPU nothing measurable
  over 1x (both ~153% uncapped); the CPU backend at the same 2x manages 90%. 3x holds ~59 fps
  capped except on heavy-fill scenes. Full tables in §0.5.3.
- ⚠ **1x is NOT pixel-identical yet — 0.21% of pixels differ**, and the bucketed diagnostic
  shows it is real (64% "backend wrote nothing", 32% wrong colour, only 3.5% rounding). It is
  visible as edge/texture speckle. **This is the blocker, not performance.**
- **`log_info()` / `psxe_diag_logf()` go to `files/logs/armsx.log`, never to logcat**, and are
  suppressed unless `logging_enabled = true` / `quiet = false`. An absent log line proves
  nothing until logging is on.
- A **gradle-free build→device loop** is verified: rebuild the `.so`, replace it inside
  `app-github-debug.apk` (post-flavor-split name; it was `app-debug.apk` when this was written)
  with `zip -0` (it must be **stored and page-aligned**, or install fails with
  `INSTALL_FAILED_INVALID_APK ... res=-2`), `zipalign -p 4`, `apksigner` with the debug
  keystore, `adb install -r`. Recipe in §0.5.3.

## 2.5 PAUSED HERE — resume point (10:35, SUPERSEDED — see §2.4)

Work was paused cleanly, not interrupted. **Everything builds:** native green, app green,
`make test-gpu` all 13 cases passing. Latest APK installed on the device.

### Resume the GPU rasterizer first — it blocks the rest

Read `frontend/HW_RENDERER_DESIGN.md` **§0.5.2** — the live checkpoint. Summary of where it got to:

- **API chosen: GLES 3.0 core (ANGLE-compatible)**, pairing with `gpu_backend = "opengl"`.
  Re-derived from current facts, not inherited: Vulkan is *structurally* closed
  (`render_vk.cpp` exports one symbol, everything else in an anonymous namespace,
  `OpSdlHandle` returns `nullptr`, no external-memory extension at device creation), so a
  Vulkan rasterizer needs a large brokered accessor or its own second device. GL needs only
  a small one. And ANGLE is GLES-on-Vulkan, so GL calls execute on the Vulkan driver anyway.
- **Key insight to preserve:** the S² blow-up — the thing that actually makes upscaling
  unusable — looks removable **without** landing §4's VRAM-coherency layer, by keeping
  `PSX_GPU_BACKEND_SOFTWARE_SHADOW` set on the GPU backend too.
- `frontend/gpu_hw_gl.h` exists (the new backend's header). No partial GL implementation
  was left in a broken state.
- **It will need ONE brokered edit** to the present layer that it is not allowed to make:
  a way to hand `render_internal.h`'s vtable an external GPU texture (the ten-slot vtable
  has no external-texture bind today). The coordinator makes that edit.

### Build-recipe correction found this session

`make test-gpu` does NOT build as written on this Mac — `sdl2-config` is not installed, so
`SDL_CFLAGS` is empty and `frontend/gpu_hw.h`'s `#include <SDL.h>` fails. Use:

```
make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"
```

### Then, in the user's stated order

1. RetroAchievements (see `frontend/RETROACHIEVEMENTS_STATUS.md`) — still needs the RA
   team's client user agent.
2. Discord — needs a new Discord application ID from the user. Note it currently retries
   `could not bind the Discord helper` once a second, forever; that needs a backoff.
3. RetroArch + CRT shaders (see `frontend/SHADERS_STATUS.md`).
4. **Restart closes the game instead of restarting it.** Not yet investigated. `restart()`
   does a full stop+relaunch rather than using `NativeApp.resetGame()`, which is a real
   native with zero callers — that is the first thing to check.

## 2.6 Session of 2026-07-31 afternoon — what landed and what it taught

All device-verified unless stated. Build green throughout: native, app, and
`make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` 13/13.

| Fixed | Root cause |
|---|---|
| Aspect / display modes did nothing | `setDisplayAspect` + `setStretchMode` were real natives with **zero callers** — the live path was built and never connected. Added `core/Ps1Display.kt` (mirrors `Ps1Pacing`), called from the one settings funnel and both in-game save sites. |
| No custom aspect | Added a 4th mode: `[video] display_aspect = "custom"` + `display_aspect_custom` (0.5–4.0). Out-of-range is **ignored, not clamped** — silently presenting a ratio the user didn't ask for is how "custom does nothing" gets reported. Verified `aspect=2.3703`. |
| Restart kicked back to the library | `restart()` tore the VM down and relaunched. `NativeApp.resetGame()` — the core's in-place reset — had **zero callers**. ⚠ **The action had TWO entry points**: `EmulationMenuViewModel.activateSelection()` index 2 *and* the row's own `MainActivityRuntime::restart` method reference in `EmulationMenuScreen.kt`. Fixing one left the bug alive on the other. |
| "Pause doesn't pause" | It **does**. Game area byte-identical over 4s, guest DMA stops at the pause instant, VM thread alive, CPU 72→10 jiffies/s. The real problem was cosmetic and self-inflicted: moving the OSD to the top-right put it **under the pause menu and its tab rail**, so fragments of live-looking stats leaked around the panel. OSD now hides while the overlay is open. |
| GPU rasterizer 1x parity (0.21% → 0.005%) | **`gpu.c` decides coverage at the integer corner `(x,y)`; a GPU decides it at the fragment centre `(x+0.5,y+0.5)`** — and a shader can discard a fragment but never *invent* one. Pixels whose corner was inside while their centre was outside never got drawn. 96% of mismatches, one defect. Fixed by submitting the bounding box; shader unchanged. Second fix: GLSL ES allows 2.5 ULP on division vs C's correctly-rounded, which only matters exactly at a texel boundary where the quotient is an integer — whole pixels appearing/vanishing in font atlases. |
| Present loop burned battery with the screen off | It kept uploading and posting to an invisible window (~6.5 jiffies/s system time) even with the VM parked. Added `g_host_presentation_suspended` — its own flag, deliberately not `paused_` (the pause menu is on-screen and must keep presenting) nor the audio suspend (`background_playback` may keep audio alive). |
| `accurate_mask_bit` / `accurate_dither` vanished | Native-only keys with no Kotlin field, so `Ps1SettingsStore` dropped them on the first save. Both **change what is drawn**, which is a mechanism for rendering that differs between runs. |

**Measured, Retroid Pocket 6 / Adreno 740, uncapped:** software 1x 89.6 fps · GLES 1x 91.4 ·
**GLES 2x 90.9** · GLES 3x 51.3 · old CPU backend 2x 53.4. **Upscaling to 2x is effectively
free.** Batching: 543 primitives in 3.6 draw calls.

### The one rule this session kept proving

Six separate "it does nothing" reports were all the same shape: **a real `native` with zero
callers**, or a lifted control whose other end is an inert stub. Before theorising about the
core, grep the call site AND check the inverse. And when you find one, **look for a second
entry point** — Restart had two.

## 3. Per-area notes worth keeping

### Boot splash (`BootSplashActivity.kt`)

Uses **TextureView + MediaPlayer, deliberately not VideoView.** VideoView is a SurfaceView
and on this window the SurfaceView never received a surface — `surfaceCreated` never fired,
so `openVideo()` kept early-returning and MediaPlayer was never asked to open the file.
Symptom was pure black with **no error callback**, which looks exactly like a bad video
file and sends you re-encoding for hours. Layout/theme/manifest were byte-identical to
ARMSX2's working splash, so diffing proves nothing here.

Intro is 1280x1280 H.264 Main@4.0, scaled to **cover** (crop, not letterbox) — full-screen.
`ui.bootLogo` default on. The logo-disabled path returns before `setContentView`, so it
still gets the theme's black and never flashes white.

No ffmpeg on this Mac; use `/usr/bin/avconvert --preset Preset1280x720 --source X --output Y.mp4`.
Its presets cap the **long edge**, so a square source stays square.

### Save states

- Format documented in `psx/state.h`. Versioned container, per-section TLV.
- `psx_state_request_slot()` is thread-safe and blocking; the emulation thread services it
  from `psx_update()`. **`main.cpp`'s outer `runFrame()` also drains it while paused** —
  without that, every save from the pause menu (the main use case) would time out, because
  `psx_update()` doesn't run when paused.
- Slot files: `<files_dir>/savestates/<stem>-<fingerprint8>.slot<N>.pss`.
- `PSX_SS_THUMB` (preview image) was added **without** bumping `format_version`/`core_abi`,
  because unknown sections are skipped cleanly. Pre-existing states still load; they just
  have no preview, and can never grow one.
- Save/Load State is reached from the in-game pause menu's **⚡ tab**, not the main list.

### Renderers / present path

- `frontend/render.cpp` + `render_gl.cpp` (GLES 3.0, system or ANGLE) + `render_vk.cpp`
  (Vulkan) + `render_sdl.cpp`. `armsx_render_compute_dst()` is the shared letterbox math.
- **Vulkan aspect bug**: the swapchain extent came from `VkSurfaceCapabilitiesKHR::currentExtent`,
  which on Android is whatever the last producer asked of the `ANativeWindow` — and the JNI
  blit bridge sets a *downscaled* framebuffer geometry before Vulkan runs. So the letterbox
  was computed in a different space from the one it was displayed in. Now sourced from the
  host Surface geometry.
- **Aspect "Classic" bug**: fell through to `psx_get_display_aspect()`, which returns the
  *framebuffer's own* ratio capped at 4:3 — a 256x240 game gave 1.067, visually identical to
  Square 1:1. Two modes, one rect. Classic is now a hard `4/3`.
- ANGLE is selected via a `dlopen`/`dlsym` EGL dispatch table; there is no second code path.
  `NativeApp.getActiveRenderer()` reports the provider that **actually loaded**, not the one
  requested, so a silent fallback is impossible to mistake for success.
- ANGLE is GLES **3.1**; this backend is GLES 3.0, so it sits inside ANGLE's envelope.

### Hardware rasteriser

Read `frontend/HW_RENDERER_DESIGN.md` — it is the authority, and §0.5 tracks live status.

Key point for anyone picking this up: **upscaling already works** (`[video] renderer =
"hardware"`, `internal_scale` 1–8) but rasterises on the **CPU**, costing ~2x at 1x and
growing as S². `make test-gpu` proves 1x is byte-identical to software, with injected-bug
negatives that must keep failing. A GPU backend implements the same
`psx/dev/gpu_backend.h` vtable and inherits those gates.

### Settings

`settings.toml` (parsed by `frontend/config.c`) is the source of truth for the PS1 core.
`config/Ps1Settings.kt` mirrors it; `Ps1SettingsStore.kt` reads/writes it.

⚠ **`Ps1SettingsStore.serialize()` regenerates each section from an explicit key list.** Any
key missing from that list is **dropped from settings.toml the first time the user saves
anything** — the setting appears to work, then silently reverts. When adding a setting,
touch all four places: the field, the writer, the parser, the UI.

Also: do **not** run a new value through `coerceIn` against a stale local range. That is how
a newly-added enum/scale value gets silently eaten.

`config/Settings.kt` (~2500 lines) still models the full PCSX2 config and is largely
vestigial. ~150 of its fields now have no UI reader. Gutting it is a dedicated pass.

---

## 4. Known open items

1. **Patch Manager and Texture Manager are unusable here** (`ui/patches/**`,
   `ui/textures/**`, `PatchRepo.kt`, `TextureCatalog.kt`). They download PS2 patch/texture
   catalogues keyed by PS2 serial + ELF CRC, and their apply methods are stubs. A PS1 cheat
   engine and PS1 texture replacement would share zero code. **Decision needed:** delete and
   track as fresh features, or leave dormant.
2. **`settings.toml` is not backed up.** `BackupManager.kt` used to back up `PCSX2-Android.ini`;
   that was removed, but this core's config lives in `Context.filesDir`, not under
   `assetCopyRoot`, so it needs its own entry on export and import (~20 lines).
3. **adrenotools binaries are not in the tree.** The custom-driver path resolves
   `libadrenotools.so` by name and falls back to a plain `dlopen` of the driver `.so`
   (works for Mesa/Turnip packs, minus the file-redirect hook). Dropping the binaries in
   enables the full path with no code change.
4. **RetroAchievements needs a client user agent** issued by the RA team. Placeholder is
   structured for a one-line swap; ARMSX2 keeps its own out of public source in a gitignored
   `ra_ua_secret.h`. **Do not reuse ARMSX2's** — impersonating another registered client is
   not acceptable. Hardcore is deliberately OFF until the emulator is better tested.
5. **Discord** needs a new application ID from the user.
6. Cheap wins identified but not done: **Dithering** (`psx/dev/gpu.c` already has
   `g_psx_gpu_dither_kernel` + `psx_gpu_dither_enabled()`; needs a toml key + JNI setter),
   integer scaling, display filter, sync-to-host-refresh, skip-dupe-frames.
7. Pad prefs on the test device have `pad.map.square = 96`, the same physical button as
   Cross — Square has no binding there. Stray from testing.

---

## 5. Conventions

- Commits: author **jpolo1224** (noreply email fine), **no AI/co-author trailer, no `.md`
  files**. (This file is a working note for an untracked directory, not a commit artifact.)
- Push to `master` only after `git fetch origin master` and verifying a clean fast-forward.
  **Never `--force`.** Guard pushes against `.so`/`.jks`/keystore/`.aab`/`.apk`/`.md`.
- Only bundle free/redistributable assets (OFL/Apache/public domain).
- `psx/` is C99. `frontend/` is C++20. Match surrounding style and comment density.
