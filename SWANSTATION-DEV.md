# SwanStation in the normal dev Compose frontend

Integration on `android-compose`, 2026-10-12. The normal Compose library,
settings, controls, hotkeys and manager screens remain in place. Earlier pending
source work is preserved and included in the user's requested Git publication.
The original installed app is not replaced by this dev build.

## Source and distribution boundary

- `core/swanstation`: official libretro/SwanStation, pinned to
  `dd4805619bbbfc6e0d1776372cb16377088b448e`, with upstream GPL/dependency notices
  intact. The adapter in `native/swanstation` is GPL-3.0-or-later. A build-injected
  bridge provides disc/card/hash inspection. `core_texture_hooks.cmake` applies
  checked, reproducible changes to a build copy of texture_replacements.cpp;
  the pinned upstream checkout is unchanged. That build overlay is part of the
  modified engine's corresponding source, not an unmodified upstream binary.
- Android builds `libemucorepsx.so` and source-built `libswanstation.so`, not the
  old psxe/SDL engine. Historical sources remain for history, not APK input.
  No current DuckStation source or private-test binaries are imported.
- Official AOSP ANGLE binaries are pinned and hash-checked; see
  `native/swanstation/ANGLE-PROVENANCE.md`. Shader runtime is source-built from
  librashader-capi 0.12.0 with Vulkan/OpenGL, published sources/notices retained.
  rcheevos uses the existing MIT source dependency.
- The root frontend licence is proprietary. It and asset/dependency rights are
  NOT replaced or claimed to be relicensed. Local test functionality is not public
  release clearance: release tasks remain blocked pending a complete audit.
- The user confirmed written permission covering this frontend/SwanStation build
  on 2026-10-12. This records their confirmation, not a review of the permission's
  terms or a change to LICENSE. Bundled dependency notices and redistribution /
  corresponding-source obligations still need a release audit.
- BIOS/ROMs, credentials, signing keys and generated outputs must not be committed.

## Connected features

- Hardware Vulkan, system OpenGL ES and Vulkan-backed ANGLE GLES; software core
  with GLES presentation. Telemetry reports the actual active backend. Failed
  ANGLE initialization retries system GLES. Native Vulkan has an adrenotools
  custom-driver loader and system fallback; changes require a stopped session.
  Custom drivers are not injected into ANGLE.
- Common aspect/custom aspect, integer scaling, stretch and portrait placement.
  Clockwise 0/90/180/270-degree display rotation works in plain and shader paths
  on GLES, software presentation, ANGLE and Vulkan. Vulkan's rotated plain path
  uses an adapter-generated pass-through shader, not an unrotated image blit.
  Widescreen geometry correction also receives the core's aspect setting.
  Core PGXP, resolution scale, texture filtering, crop and downsampling mappings.
- Hardware texture dumping/replacements use SwanStation VRAM-write hashes.
  Per-title `dump/` and `replacements/` folders are created automatically. Dumps
  stop at 512 unique images or approximately 64 MiB per capture configuration;
  existing dumps are not overwritten. This is not the old psxe texture-pack
  format and is not supported by the software rasterizer. Replacement loading
  has been verified by core hit counters, not just folder creation.
- Deinterlacing choices are Auto and Force progressive, matching the available
  core option. Old Bob/Adaptive preferences fall back to Auto; compatibility
  overrides in the core can preserve a game's interlaced mode.
- Android performance hints dynamically load the platform API when available,
  report active frame work, and release sessions on disable/teardown. Affinity
  restricts the emulation thread using detected CPU frequency tiers and restores
  its original scheduler mask on closing. Neither promises a speed increase.
- Catalogue cheats: core parser validation, selected-group execution, master
  disable and actual RAM-effect checks. Enabling cheats disables achievements
  for that session, rather than awarding cheated progress.
- Numbered/autosave `.swanstate` files, previews, original-source companions,
  atomic writes, payload CRC, pinned-core/game identity and memory-card digest.
  Checked loads reject incompatible/corrupt states; explicit card-divergence
  override is separate. Save Manager understands the new namespace. Old `.pss`
  states are not silently reused.
- Card manager resolves actual per-title Slot 1/shared Slot 2 paths or last used
  cards. Old card import is explicit; no automatic overwriting migration.
- Bounded rewind snapshots (128 MiB maximum, invalidated by card changes), held
  and single-step rewind, and upstream single-instance run-ahead.
- Shader presets/parameters on Vulkan/OpenGL/ANGLE, including software GLES.
  Invalid preset creation falls back to plain output; runtime failures are
  logged. Arbitrary presets are not universally guaranteed. Screenshots/previews
  capture core output, not the final shader/OSD composition.
- Measured FPS, emulation/presentation/idle/worst-frame times, resolution, RSS and
  available RAM and aggregate process CPU utilization. Unavailable guest
  CPU/GPU/SPU/etc. counters and host GPU utilization are shown as `n/a`, never
  fabricated zeroes. Process CPU utilization can exceed 100% across CPU cores.
- RetroAchievements softcore host: official content hashing (including CHD), TLS
  transport restricted to the official host, asynchronous network worker,
  AndroidKeyStore-encrypted token persistence, account/list/subset/rich-presence
  data, real-frame evaluation and reset on state loads/rewind/reset. Positioned
  unlock/leaderboard notes and bounded challenge/tracker overlays are connected.
  Custom app-private unlock sounds and a volume-controlled default tone are
  connected. Indicators clear on reset/unload/logout. No password is persisted
  or logged. Local UI tests do not establish live service/unlock compatibility.
- Vulkan retries an out-of-date acquire once and rebuilds the swapchain after
  out-of-date/suboptimal presentation. Partial initialization tears down core
  helper objects and host-owned resources before GLES fallback. Thor tests cover
  explicit surface loss/rebinding; forced driver/negotiation failures remain
  distinct, untested cases.

## Remaining limitations / verification

- Actual RetroAchievements account login, service game matching and live unlocks
  need a user account test. Hash determinism and transport checks alone do not
  establish service compatibility. Hardcore and arbitrary service-host overrides
  remain disabled. Synthetic local overlay events do not award achievements or
  contact the service.
- Custom Vulkan-driver boot has not been tested with a supplied driver. ANGLE's
  ELF LOAD alignment is 4 KiB; 16 KiB-page devices and other Android/GPU
  combinations are not validated.
- Detailed guest counters, old engine-specific line-detection filters and launch
  arguments remain unavailable. Preserving a settings screen does not make those
  engine-specific options functional; line detection is a read-only unavailable
  row, not an active control. Replacement packs must match SwanStation's
  hash/PNG format, not the old engine's pack format.
- All-game/device compatibility, disc swaps and surface recreation are not
  established by a bounded Thor smoke test. Vulkan uses conservative single-frame
  synchronization. Real multi-disc transitions, multi-subimage containers and
  adversarial surface/driver failures need coverage beyond same-disc reinsertion
  and the bounded surface-rebinding test.
- Public distribution remains blocked by frontend/assets/dependency provenance.
  This is a private dev build, not release certification.

## Build and checks

Initialize both pinned submodules:

```
git submodule update --init core/swanstation third_party/angle-prebuilt
```

Windows requires JDK 17, Android SDK, NDK 29.0.14206865, CMake 3.30.5, Cargo and
Rust's `aarch64-linux-android` target. Initial Cargo fetching needs network/cache
access. Build-SwanCore.ps1 and Build-SwanShaders.ps1 stage the required engine,
ANGLE, librashader and libc++ libraries. Optional `ARMSX_SHADER_WORK_DIR` selects
an external Rust build cache.

From `android`:

```
.\gradlew.bat :app:assembleGithubDebug :app:assembleGithubDebugAndroidTest :app:testGithubDebugUnitTest --offline --console=plain -Parmsx.applicationId=com.aaron.emucorepsx.devcompose -Parmsx.appLabel=EmuCorePSX-Dev
```

APK: `android/app/build/outputs/apk/github/debug/app-github-debug.apk`.
Only this isolated package may be updated under current device-test approval.
Signing is debug-only; no private key/password is used. Original apps/saves
remain untouched. Run `tools/Test-SwanIntegration.ps1 -Apk <absolute APK path>`
for JNI argument-count, pinned-input and packaged-library boundary checks.
This is not a licence or complete ABI/type audit.

Device tests use user-supplied app-private `swan-smoke` fixtures, never ROM/BIOS
APK assets. The matrix boots system GLES, software, Vulkan and ANGLE twice each,
with BIOS skipping off; checks image/audio/input, shader image and parameters,
cheat memory effect/master disable, states/previews, rewind/run-ahead, card paths,
CHD identity/hash, rotation, surface recreation, actual new PNG dumps/replacement
loads, clean closing and stale session/surface callbacks. BIOS tests
filter PS1/fake/oversize files. Transport tests reject unsafe hosts without
sending credentials and check positioned indicator lifecycle/private sound paths.
JVM regression suite contains 104 tests; consult the latest
generated reports for final results.

Pre-integration backups remain outside the repository in
`C:\Users\aaron\ARMSX1-Swan-Integration-Backup-20261012`. No reset/checkout or
unrelated work is discarded. The user subsequently requested committing all
pending source changes and pushing the current `android-compose` branch to its
configured team remote, `https://github.com/ARMSX2/ARMSX1.git`. Builds, local
configuration, keys, credentials and game fixtures are excluded.

## Verification — 2026-10-12

- 104 JVM tests passed; zero failures/errors in 20 report suites.
- Final installed APK: all six Thor instrumentation tests passed in 117.249
  seconds, including eight renderer launches, shaders/plain rotation, surface
  rebinding, fresh texture PNG dumps and actual replacement loads, positioned
  indicator lifecycle and private sound-path checks. No live achievement is
  awarded by these local tests. Log: `swan-rest-installed-device.log`.
- Shared presentation math: thirteen cases passed on the ARM64 device executable.
- All 92 JNI argument-count contracts, both submodule pins, required shader /
  libc++ libraries and exact ANGLE packaged bytes passed. No old engine, SDL or
  current-DuckStation libraries, signing keys or game fixtures were packaged.
- APK v2 signature verified. SHA-256:
  `7197CC0F7BE9191E50B03168376227692C25C35EE0C829E15C3D4C9ECB452B1B`.
- The isolated `com.aaron.emucorepsx.devcompose` app is updated on Thor. Original
  applications and their saves are untouched. No custom Vulkan driver was
  available in this app's imported-driver directory, so no driver was downloaded
  or substituted for that missing test.
- Release guard was exercised and rejected assembleGithubRelease before build.
  Upstream core and ANGLE submodules are clean.

## Git publication boundary

Publishing this source branch does not clear the APK public-release audit.
The existing `.github/workflows/nightly-android.yml` still targets the old release
build flow; the release guard intentionally rejects its assembleGithubRelease
task. Updating CI for this adapter and clearing distribution checks are separate
work. No automatic release workaround or signing-secret change is included.
