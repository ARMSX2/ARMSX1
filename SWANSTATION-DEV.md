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
- The 1,664-list offline cheat database is available from the main-menu Cheats
  screen even before selecting or launching a game. Search and catalogue previews
  require no import/download and never write cheat files or arm codes. Loading /
  enabling cheats still happens in the explicit per-game screen, with custom
  codes preserved and ambiguous region/revision matches requiring confirmation.
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
JVM regression suite contains 113 tests; consult the latest
generated reports for final results.

Pre-integration backups remain outside the repository in
`C:\Users\aaron\ARMSX1-Swan-Integration-Backup-20261012`. No reset/checkout or
unrelated work is discarded. The user subsequently requested committing all
pending source changes and pushing the current `android-compose` branch to its
configured team remote, `https://github.com/ARMSX2/ARMSX1.git`. Builds, local
configuration, keys, credentials and game fixtures are excluded.

## Verification — 2026-10-12

- 104 JVM tests passed; zero failures/errors in 20 report suites.
- Last Thor-installed renderer APK (before the standalone database browser):
  all six instrumentation tests passed in 117.249
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

## Standalone cheat database follow-up

The no-game Cheats entry now opens the bundled catalogue instead of hiding it
behind “No game selected.” It supports live search, paginated game lists and
read-only cheat-name previews. It does not restore a separate app's private
imported files or enable cheats globally. All 1,663 original bundled lists remain
included. Existing per-game custom codes and selections are not changed by browsing.

The debug and instrumentation APKs built successfully; 105 JVM tests passed,
including preview-name checks against every actual bundled catalogue. The new
packaged-asset instrumentation test compiled but has not run: no Android device
was connected for this follow-up. The updated APK has not been installed on Thor,
and these follow-up source changes have not been committed/pushed.

On 2026-10-13 that standalone-browser APK was installed on Thor at the user's
request. The following newer USA catalogue build is separate from that installation.

## Warped USA catalogue — 2026-10-13

Added a separate `Crash Bandicoot - Warped (USA) (GameShark)` supplement, matched
to `SCUS-94244`, with six USA-listed groups: lives, powers, gems, sapphires,
crystals and platinum relics. European `SCES-01420` catalogues remain separate.
This supplement uses existing CC BY-SA Libretro code bytes checked against
the original authors' USA listings; see `tools/cheat-supplements/NOTICE.md`.
No assertion is made that every group in the European file works on USA copies.

The generator reproducibly adds the supplement and includes its attribution in
the packaged notice. All 1,663 previous catalogue payloads were checked byte for
byte against the committed ZIP and remain unchanged. The total is now 1,664
catalogues / 16,909 supported complete groups. New imports still start disabled
and preserve existing custom groups.

The APKs rebuilt successfully. All 106 JVM tests and eight Python conversion
tests passed, including USA-versus-Europe matching and packaged-data consistency.
The new Android asset tests compiled but have not run, and Warped's actual in-game
cheat effects have not been tested on the user's ROM. This newer APK has not been
installed, committed or pushed yet.

## Catalogue-wide discovery — 2026-10-13

This follow-up fixes discovery across the available data, not just Warped:

- Exact serial lookup is indexed. Title/serial searches handle common separators,
  diacritics, scraped underscore/ampersand names, metadata aliases and numeral
  variants. Title aliases are suggestions, never proof of automatic compatibility.
- Candidate lists open automatically when no unambiguous list can be imported.
  World/revision ambiguities still require confirmation. Confirmed different
  countries, demo/retail releases or explicit discs cannot be loaded for a known
  game ID. Existing custom groups and enablement are preserved.
- On opening Cheats for a selected library game, its actual disc ID is read on IO
  if needed. SAF stages only that selected game through the existing preparation
  helper, then cleans up. Positive IDs are cached separately; library settings
  keys and whole-library fast scanning are unchanged. Cancellation stops the lookup.
- The generator corrects discoverable aliases and excludes demo/retail and disc
  mismatches. `tools/audit_ps1_cheats.py` reports coverage; missing code lists are
  not presented as a failed import or silently filled from another region.

The source still has 1,664 catalogues / 16,909 complete supported groups. There
are linked lists for 2,734 of 10,187 known disc/region IDs; 7,453 have no linked
catalogue. Those IDs include distinct regional editions and discs, not 10,187
unique game titles. This is NOT full-library cheat coverage. More matching code
data is required to fill the genuine gaps; the user's original imported database
is no longer available. No complete collection was scraped from an unlicensed
source or imported from current DuckStation.

113 JVM tests and twelve Python conversion tests passed. Tests check discovery
for every linked catalogue serial, regional separation, aliases, demo/disc safety
and formatted search. Three packaged-data tests passed on Thor in 1.877 seconds
for the preceding discovery/search build. The actual UI also displayed the
candidate list for an unidentified library copy of 007 - The World Is Not Enough.

The final on-demand-ID build passed compilation, JVM and APK checks. Thor initially
disconnected before installation, then reconnected at the user's request. Both
the final app APK and its instrumentation APK installed successfully on Thor.
All four packaged-data tests passed in 1.867 seconds, with no skipped identity
test: the selected-game lookup read `SCUS-94154` from the Crash2 CHD's contents,
not its filename. USA Warped selection and catalogue-wide discovery also passed.
The app cold-launched successfully after testing. APK SHA-256:
`70D4306A88A91C1F822E2F1C6989AA22B49F8A20ABB2D56A121D3E6D952DD428`.
Device results are recorded in `swan-all-cheats-identity-device.log`. These
follow-ups accompany the Thor fix below on `android-compose`. Individual in-game cheat effects across all titles,
revisions and regions have not been established by these lookup tests.

## Thor sideways Vulkan picture — 2026-10-13

Reproduced the user's report on the top screen: Compose controls were upright,
but Crash Bandicoot 2's game picture was rotated sideways with display rotation
set to zero. Thor exposes a portrait-native panel and a landscape logical window.
The Vulkan host incorrectly used `currentTransform` for frames that were not
pre-rotated. It now explicitly requests supported identity presentation so
Android handles physical-panel rotation. This does not change the user's game
rotation, display selection, internal resolution or orientation preferences.
Unsupported identity presentation fails explicitly rather than drawing sideways.
The Android transform contract is described in
[AOSP swapchain implementation](https://android.googlesource.com/platform/frameworks/native/+/85b74b7e07/vulkan/libvulkan/swapchain.cpp).

Android reports `SUBOPTIMAL` for this intentional transform difference. Successful
suboptimal presents no longer rebuild the swapchain every frame; actual surface
changes and `OUT_OF_DATE` still rebuild it. Final-device logs show one swapchain
creation instead of repeated per-frame creations: 1280x720 surface, panel
transform 2, presentation transform 1. Actual game startup is upright at 3x,
approximately 59.8 FPS / 100% speed in the captured frame. The final screenshot
is `build/thor-orientation-final.png`; this is a startup observation, not a
long-duration performance claim. OpenGL, ANGLE and software paths are unchanged.

Build and signature checks passed, with 113 JVM tests, six native surface-policy
cases and thirteen shared presentation cases. The native cases ran on Thor.
Four cheat-device checks passed after installing the first orientation build;
the final follow-up only avoids the repeated suboptimal native swapchain rebuild.
The final APK was installed with the user's permission in the existing isolated
EmuCorePSX Dev package. SHA-256:
`4045CA1CC3119DA188CC733BFFDC34221634E30FFCD93C008E6D6E7A5879D02C`.
Source changes target the team repository's `android-compose` dev branch;
no APK release is published by this source update.
