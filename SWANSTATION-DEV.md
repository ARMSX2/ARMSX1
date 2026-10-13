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

## Cheats UI cleanup — 2026-10-13

The shared per-game panel now has a compact game/serial summary with available,
selected and (when running) actual core-active count chips. Search uses the
library-style search bar with an inline clear action. Repeated generated
Libretro source/game-title descriptions and redundant dividers were removed
from cheat switches; custom descriptions, author attribution and unsupported
code warnings remain. File paths, paste/import/reload and full source details
are behind an `Import & file tools` expander. The standalone catalogue browser
uses the same search bar, shorter counts and an expandable coverage/license
section. Automatic matching, region/release blocking, per-game selections and
hardcore restrictions are unchanged. Controller IDs remain registered for the
search, clear, browse and expanded actions.

The isolated dev APK built, verified and updated on Thor with permission.
All 113 JVM tests and four packaged cheat-device tests passed (device tests:
1.846 seconds). Actual UI inspection confirmed `SCUS-94154`, four available / zero
selected cheats, and search `100` reducing the displayed list to `100 Lives`;
clearing restored all four. No cheats were enabled during these checks. The
full imported catalogue and files remain intact; this is a presentation change.
APK SHA-256:
`F010464C3A3094624882D61E2D7432AA79FB199668F5270032D61ED29793BF0E`.
This UI follow-up is not committed or pushed yet.

At the user's follow-up request, the per-game database browser button and its
expanded content were moved below the cheat list, directly above `Import & file
tools`. The shared settings/pause-menu panel uses this same order. Matching and
confirmation logic are unchanged. This reordered APK rebuilt with 113 passing
JVM tests and was successfully installed on Thor with permission. Its SHA-256 is
`4D8F5EE0538019B32D2BCFD990403294374801804D1B3755EAA1DF8152F003F1`.

The next UI refinement removes generic scope/override banners from the Cheats
tab and hides override badges on cheat settings. The game context provider and
per-game storage are unchanged. Cheat-only selections no longer trigger the
pause menu's global-override notice; genuine non-cheat overrides still do.
That notice's reset now clears only the settings it names, preserving hidden
cheat selections rather than silently resetting them too. Region/release,
unsupported-code, catalogue mismatch and hardcore safety notices remain.

Three policy regression tests cover cheat-only exclusion, non-cheat visibility
and reset targets preserving cheats: all 116 JVM tests passed. The APK rebuilt,
verified and installed on Thor with permission. The Cheats tab was opened for
Crash Bandicoot 2 and its generic scope/override blocks were absent while its
four existing cheats / zero selected summary remained. Final APK SHA-256:
`0A29D5C2457F9C1B5B1E633E60402F7696F766BE7FCC26AAE69B518208958590`.
These UI refinements are still uncommitted and unpushed.

## Combined compatible cheat lists — 2026-10-13

Opening a game's Cheats panel now loads every unambiguous verified catalogue
format for its actual serial, instead of preferring a single GameShark list.
The header lists the matched formats and the combined-list count. Multiple
competing versions of the same format, title-only matches and uncertain
regions/revisions still require confirmation; incompatible releases remain
blocked. Games with only one source list still show that one list.

All selected catalogue bodies are read in one archive pass and merged in one
atomic file update. Identical complete code bodies are deduplicated. Different
code variants sharing a name get unique source suffixes (bounded to the core's
name limit); custom groups, existing names and enabled selections are preserved.
New groups, including names that were previously orphaned, start disabled.
Repeated imports are idempotent. Manual confirmed list loads use the same merge.
The browser remains above Import & file tools, and the previous override-noise
cleanup remains in place. No new source data or unsupported code families were
added by this change.

All 122 JVM tests passed, including real multi-format catalogue coverage,
cross-region separation, ambiguity protection, literal/source-labelled names,
deduplication, custom-selection preservation and repeated imports. All five
packaged-data tests passed on Thor in 1.972 seconds, including the real European
Warped GameShark/Game Buster pair and separate USA lookup. These are matching
and merge tests, not an assertion that every cheat effect works on every ROM.
The dev APK was verified and installed on Thor with permission; SHA-256:
`062457E46E8EA423C768B8F7BFC59DF7D0B4781E6B08D55E671D6633FF6BD882`.
This follow-up and the pending UI refinements remain uncommitted and unpushed.

## Tony Hawk's Pro Skater USA supplement — 2026-10-13

For the user-confirmed `SLUS-00860`, added two complete USA-listed patch groups
targeting score and the special meter, attributed to ViperByte and checked
against [the author's USA listings](https://gamehacking.org/game/90046?hacker=ViperByte).
They are in a separate USA Action Replay supplement, alongside the original
two-entry USA/Japan GameShark list. The combined list now contains four groups.
No `????` templates were filled by guessing, and the new entries are not assigned
to Japan or the European/beta/Greatest Hits listings. Per-source provenance and
the scope of the locally authored annotations are recorded in the supplement
notice; this does not clear the application's public-release audit.

All 1,664 previous packaged catalogue payloads were checked byte-for-byte against
the committed ZIP and remain unchanged. The total is now 1,665 catalogues /
16,911 complete groups. Existing custom cheats and enabled selections are kept;
new groups start disabled. The converter retains explicit per-entry author
attribution rather than falsely labelling this supplement's authorship Libretro.

All 123 JVM tests, thirteen Python conversion tests and six Android packaged-data
tests passed (device tests: 2.006 seconds). The verified dev APK installed on Thor
with permission. Actual UI resolved `SLUS-00860`, showed four cheats / zero selected
and both Action Replay/GameShark source chips, and displayed the two new entries
with author attribution. No new cheats were enabled, so their actual gameplay
effects on this ROM remain unverified. APK SHA-256:
`73F8FB79FEA6833BE6E9704287F796AECCE3485FEBF6A246F70744D2CD4F1ABC`.
This supplement and the preceding UI refinements are uncommitted and unpushed.

## Cheat-name prefix search — 2026-10-14

The per-game cheat filter now matches case-insensitive name prefixes rather than
letters occurring anywhere in a name. `F` immediately matches names starting
with F, and `Fu`, `Ful`, etc. narrow the same list on every keyboard edit.
Empty/cleared searches restore all entries; leading/trailing search whitespace
is ignored. Database catalogue searching is unchanged. No cheat files,
enablement, matching or source data were changed by this refinement.

Four new regression tests cover first-letter matching, progressive narrowing,
backspacing/clearing, case, spaces, numeric prefixes and source-labelled variants.
All 127 JVM tests passed. The APK verified and installed on Thor after the user
re-enabled wireless debugging. APK SHA-256:
`D5696908B6DCB95F948586E17A39D1BC3778C6D3D4FF6F555F3BF04BFB21DD1D`.
Actual Thor UI search `f` showed only `Full special meter`, excluding
`Infinite Time`, `No Sound FX` and `Maximum score`. Clearing the query restored
the unfiltered list. No cheat switches were changed during these checks.
This and the preceding follow-ups remain uncommitted and unpushed.

## Remove ViperByte supplement; local Tony Hawk research — 2026-10-14

At the user's request, removed the two additional ViperByte Tony Hawk patch
entries from the bundled inputs and regenerated the catalogue. This precaution
does not assert infringement. The source file has a recoverable local backup
outside this repository. The resulting bundled ZIP and index match the previously
committed 1,664-list / 16,909-complete-group data; no other bundled cheat payload
was changed. The original Libretro Tony Hawk entries remain present.

Added a read-only emulation-thread RAM snapshot diagnostic and an opt-in Android
research harness with isolated cards/states, serial verification, bounded test
commands and settings restoration. It does not expose a writable RAM pointer.
Memory analysis and observed effects produced two guarded data-field candidates:
score 999999 and full special timers. They do not reuse the removed instruction
patch sequences or enable the game's built-in cheat flag. See
`tools/cheat-research/README.md` for derivation and limitations, including the
explicit fact that this was not a clean-room/novelty exercise.

Actual user-ROM tests on Thor (`SLUS-00860`, Tony Hawk) verified the HUD score,
both score tapes, full special bar with built-in flag zero, natural timer decay
after disabling, a new Warehouse allocation/run, and a separate School layout.
Per-write guards left target data unchanged when no player was allocated; the
Warehouse-only candidate left School unaffected until its observed profile was
added. Other levels/skaters/revisions/multiplayer and long play are unverified.

The experimental listing lives OUTSIDE bundled supplement inputs. With the
user's update/test approval, backed up Thor's existing Tony Hawk cheat file,
removed ONLY the two attributed ViperByte additions, and added the explicitly
experimental alternatives to that private game's list. Kept both original
Libretro groups, normal game saves/cards and existing selections intact. Actual
Cheats UI showed four entries, zero selected, both experimental names and their
Warehouse/School limitation notes; no switches were enabled by that UI check.

Build succeeded; 127 JVM tests, 21 Python conversion/research checks, six Android
packaged-data tests and two opt-in research sessions passed. Both research
sessions ended with VM shutdown, settings restoration and an empty inactive RAM
snapshot. The APK signature, 93 JNI contracts and pinned core/ANGLE APK boundary
checks passed. Installed APK SHA-256:
`E8CB55820EFD96AE641C265B85BB55188D84EC97CBBADA67525DDFA4691BE2C2`.
No codes/game/RAM fixture was added to APK assets by the research harness. This
work and earlier UI follow-ups remain uncommitted and unpushed; no public-release
audit was cleared. All local removal backups are outside the repository.

## Tony Hawk Single Session fix — 2026-10-14

The old experimental pointer guards covered Career allocations only. Observed
Single Session Warehouse at `0x801F2694` and School at `0x801EE728`; added two
explicit guarded profiles to both score and special entries. No guards were
removed. Actual Single Session Warehouse/School tests showed score 999999 and
full special (timer fields 750; built-in flag zero); Career Warehouse regression
still passed. Five profile tests and the 998.224-second research session passed,
with shutdown, inactive RAM snapshot verification and settings restoration.

School unlock was earned/saved only in the research sandbox, not the user's card.
Updated Thor's two private research entries in place after backing up the file
outside the repo, preserving labels, selected names and both Libretro groups.
Data-only change: no APK rebuild, no bundled database change. Other levels,
skaters, Free Skate, revisions and multiplayer remain unverified. Uncommitted
and unpushed. See tools/cheat-research/README.md for evidence and limits.

## Source publication scope — 2026-10-14

The source commit accompanying this note includes the preceding pending cheat
UI/layout/search, combined-format matching and override-notice improvements,
the removal of the temporary ViperByte supplement, and the opt-in RAM research
tools and guarded Tony Hawk USA Career/Single Session alternatives. Earlier
"uncommitted/unpushed" notes above describe their historical state at test time.

Pre-commit checks passed: 22 Python conversion/research tests, the Gradle JVM
test task (127 existing validated tests), and 93 JNI/pinned-core contracts.
Experimental codes remain outside the bundled database. No test ROM, BIOS,
RAM dump, screenshot, installed cheat-file backup, keystore or APK is included
in this source commit. This is not an APK release or public-release audit clearance.

## Downloadable cheat catalogue — 2026-10-14

Added an explicit Download cheats action beside the database browser in both
the per-game and global Cheats UI. It fetches one data-only versioned package
from this repository's android-compose branch; users need the feature APK once,
not a new APK for later catalogue publications. It preserves per-game custom
files/selections, merges newly verified matches disabled, retains conservative
region/revision confirmation and persists the validated database for offline use.

Size limits, strict UTF-8/index/count validation, fixed archive names, unique
entries and component SHA-256 checks precede atomic installation. Failures keep
the prior database; corrupt caches fall back to the bundled catalogue. The UI
shows the downloaded revision and expandable current attribution notices. No
automatic downloads, APK/core replacements or arbitrary ZIP extraction occur.

`tools/build_cheat_update.py` generates the reproducible publication from seven
database/metadata/license assets, excluding experimental research and private
artifacts. The normal converter now regenerates it automatically. Publication
instructions and version-1 trust/merge limitations are in `cheats/README.md`.

Local validation: 134 JVM tests and 25 Python tests passed; main/test APKs built,
and 93 native JNI/pinned-core contracts passed. Isolated Android storage tests
cover offline restart, invalid replacement and corrupt-cache fallback; their
GitHub network test is explicit opt-in via `-e cheatDownloadNetwork true`.
