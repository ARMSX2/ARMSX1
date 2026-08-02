# RetroAchievements on ARMSX1 — implementation checkpoint

Softcore-only RetroAchievements support for the PS1 core, ported onto the lifted ARMSX2
Compose UI. Written as a resume point: everything below is concrete enough to pick up cold.

Last updated: 2026-07-31.

---

## 1. State of the tree

Nothing is mid-edit. Every file listed is complete and self-consistent.

### Added

| File | What it is |
|---|---|
| `third_party/rcheevos/**` | Vendored rcheevos v12.3.0 (see §2). |
| `frontend/achievements.h` | Public API of the RA module + threading contract. |
| `frontend/achievements.cpp` | The whole integration: rc_client lifecycle, PS1 memory reader, HTTP request queue, PS1 disc hashing (incl. a CHD cdreader), JSON snapshot, login/logout, options, persisted store. |
| `frontend/ra_ua.h` | User-agent macros + the `__has_include("ra_ua_secret.h")` hook (see §5). |

`frontend/ra_ua_secret.h` is deliberately **NOT** in the tree and is gitignored.

### Changed

| File | Change |
|---|---|
| `Makefile` | `RCHEEVOS_DIR` / `RCHEEVOS_INCLUDE_FLAGS` / `RCHEEVOS_COMPILE_DEFS`; `RCHEEVOS_SOURCES` appended to `C_SOURCES`; `frontend/achievements.cpp` appended to `CPP_SOURCES`; `$(RCHEEVOS_OBJS): BASE_CFLAGS += -fno-fast-math`. |
| `.gitignore` | `frontend/ra_ua_secret.h`, `ra_ua_secret.h`. |
| `frontend/main.cpp` | 6 surgical hooks: `#include "achievements.h"`; `armsx_ach_startup()` in `run()` after the pref path resolves; `armsx_ach_frame_update(session_stepping)` in `runFrame()`; `armsx_ach_session_started(session_.psx())` in `launchSession()` and after a successful disc swap; `armsx_ach_session_ended()` in `exitToLibrary()` and `shutdown()`; `armsx_ach_shutdown()` just before `shutdown()` in `run()`. |
| `frontend/android_jni.cpp` | JavaVM captured in `JNI_OnLoad`; `ScopedJniThread`; `ResolveAchievementsGlue`; `AchievementsHttpRequest`; `AchievementsPlaySound`; `EnsureAchievementsReady`; 13 new `Java_kr_co_iefriends_pcsx2_NativeApp_*` exports. |
| `android/.../kr/co/iefriends/pcsx2/NativeApp.java` | 13 RA stubs converted from Java bodies to `native`. |
| `android/.../com/armsx2/ui/achievements/AchievementsViewModel.kt` | `requestToggleHardcore()` made inert; `hardcore = false` in `parse()`. |
| `android/.../com/armsx2/ui/achievements/AchievementsScreen.kt` | Hardcore `SettingSwitchRow` removed. |
| `android/.../com/armsx2/ui/emulation/EmulationMenuViewModel.kt` | `requestToggleHardcore()` inert; Achievements tab action map reduced to one action; `actionCount(Achievements)` 2 → 1. |
| `android/.../com/armsx2/ui/emulation/EmulationMenuScreen.kt` | Hardcore `MenuSwitchRow` removed from the in-game achievements pane. |

### Build status

`./build.sh android` was **green** with all of the above at 03:07 (exit 0,
`android/app/src/main/jniLibs/arm64-v8a/libarmsx.so` 4.70 MB → 7.10 MB, all 13 JNI symbols
present in `llvm-nm -D`). Two later rebuild attempts failed **in another agent's in-flight
edits to `frontend/main.cpp`** (`resetFastForwardResampler`, then `applyAudioDriverSetting` —
neither is anything this work touches); `frontend/main.cpp` syntax-checks clean again as of
03:21 and a rebuild is running. Nothing in the RA work has ever failed to compile.

Zero warnings from `achievements.cpp`, `android_jni.cpp` or any rcheevos source under
`-Wall -pedantic`.

**Gradle was never run** — the coordinator owns the app build. The Kotlin/Java edits are
therefore compile-unverified. They are small and mechanical, but that is the one build step
still outstanding.

---

## 2. rcheevos vendoring — the expensive part

**Version: v12.3.0**, and this was *verified*, not assumed. ARMSX2's
`3rdparty/rcheevos/src/rc_version.h` declares 12.3.0, and `cdreader.c`, `hash.c`,
`rc_hash_internal.h`, `md5.c`, `md5.h` in ARMSX2's tree are **byte-identical** to
`https://raw.githubusercontent.com/RetroAchievements/rcheevos/v12.3.0/src/rhash/<file>`.
So ARMSX2 vendored upstream v12.3.0 unmodified — it just omits files.

Vendored into `third_party/rcheevos/` as:

* `include/*.h`, `src/*.c|h`, `src/rapi/*`, `src/rcheevos/*` — copied from
  `/Users/jpolo1226/Downloads/browser-util-macos-release/agents/armsx2-push-staging/3rdparty/rcheevos/`.
* `src/rhash/{cdreader.c, hash.c, md5.c, md5.h, rc_hash_internal.h}` — same source.
* **`src/rhash/hash_disc.c` — fetched from upstream**, because ARMSX2 does not have it:
  ```
  curl -L https://raw.githubusercontent.com/RetroAchievements/rcheevos/v12.3.0/src/rhash/hash_disc.c
  ```
  ARMSX2 compiles with `RC_HASH_NO_DISC`, so upstream's disc hashers were simply deleted from
  its tree — `rc_hash_psx()` is *declared* in `rc_hash_internal.h` there but defined nowhere.
  `hash_disc.c` is the file that contains `rc_hash_psx()`, i.e. the entire reason this port can
  identify a game. It is self-contained: `rc_cd_open_track` / `rc_cd_read_sector` /
  `rc_cd_find_file_sector` / `rc_hash_cd_file` are all `static` inside it and go through
  `iterator->callbacks.cdreader`.
* `LICENSE`, `README.md` copied alongside.
* `hash_rom.c`, `hash_zip.c`, `hash_encrypted.c` are **not** vendored and are not needed —
  they stay compiled out.

Build wiring (`Makefile`):

```
RCHEEVOS_INCLUDE_FLAGS := -I$(RCHEEVOS_DIR)/include
RCHEEVOS_COMPILE_DEFS  := -DRC_NO_THREADS=1 -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP
```

`RC_HASH_NO_DISC` is **deliberately absent** — that is the whole point. `RC_NO_THREADS` is
safe because `achievements.cpp` serialises every rc_client call behind one recursive mutex.
`RCHEEVOS_SOURCES` lists the 27 built `.c` files explicitly (no wildcard) so
`rc_libretro.c`, `rc_client_external.c`, `rc_client_raintegration.c` stay out.

`$(RCHEEVOS_OBJS): BASE_CFLAGS += -fno-fast-math` — the repo builds with `-ffast-math`, and
`src/rcheevos/memref.c` produces/compares `INFINITY`/`NaN` for float memrefs, which
`-ffinite-math-only` permits the compiler to assume away. Clang warned about it
(`-Wnan-infinity-disabled`); the override silences it *and* fixes the underlying semantics.

---

## 3. PS1 disc hashing

`HashDiscImage(path)` in `achievements.cpp`:

```
rc_hash_initialize_iterator(&it, path, NULL, 0);
if (path ends in .chd) override it.callbacks.cdreader with the ARMSX bridge
rc_hash_generate(hash, RC_CONSOLE_PLAYSTATION /* 12 */, &it);
```

* **.cue / .bin / .iso** use rcheevos' *own* default cdreader untouched. This is the code the
  RA hash database was generated with, so those formats are as close to guaranteed-correct as
  it is possible to be without a live test.
* **.chd** has no upstream support, so there is a bridge (`ChdOpenTrack` / `ChdReadSector` /
  `ChdCloseTrack` / `ChdFirstTrackSector`) over ARMSX's own `psx_disc_t` layer
  (`psx/dev/cdrom/disc.h`). libchdr always yields raw 2352-byte sectors, so the only thing the
  bridge has to decide is whether user data starts at offset 16 (mode 1) or 24 (mode 2 form 1);
  it decides exactly the way `cdreader_determine_sector_size()` does — read
  `first_track_lba + 16` and test for `"CD001"` at offset 25 — and always returns 2048 data
  bytes per sector, matching `raw_data_size`.

The hash is computed **on the emulation thread**, deferred to the first
`armsx_ach_frame_update()` after a session starts, so a slow CHD open never lands inside a boot.

### ⚠️ NOT VERIFIED

**The produced hash has not been compared against a known-good RetroAchievements hash for any
real game.** There is no PS1 disc image in this tree to test with, and no device run happened.
This is the single biggest open risk: an off-by-one in the CHD bridge, or an unexpected
sector layout, means every lookup silently misses and the UI shows "no achievements" forever
with no error anywhere.

**How to verify (do this first):**
1. Take a PS1 game that definitely has achievements (e.g. *Crash Bandicoot*, *Final Fantasy VII*).
2. Get the reference hash from `https://retroachievements.org/game/<id>` → "Supported Game Files",
   or from RALibretro / RetroArch's log for the same file.
3. On device, call `NativeApp.getAchievementsHashForPath("<abs path>")` (it works with no VM)
   and compare. Or read `logcat -s ARMSX-RA` for the `identifying disc … (hash …)` line at boot.
4. Test **both** a `.cue`+`.bin` pair and a `.chd` of the same game — they must produce the
   *same* hash. If the `.cue` matches RA and the `.chd` does not, the bug is in the CHD bridge
   (`ChdReadSector`'s header offset), not in the hashing.

---

## 4. Wired vs stubbed — method by method

All 13 are real `native` methods now (`NativeApp.java`), exported from `android_jni.cpp`,
confirmed present with `llvm-nm -D`.

| Method | Status |
|---|---|
| `getAchievementsJSON()` | **Wired.** Full ARMSX2-compatible schema: `active, loggedIn, hardcore, userName, score, softcoreScore, avatarUrl`, the 8 option booleans, the 4 int options, `items[]` (id/title/description/points/unlocked/unlockedMask/primed/bucket/subsetId/type/unlockTime/measuredProgress/measuredPercent/rarity/iconUrl) and `subsets[]`. Falls back to the persisted store when there is no live client, so the library RA tab shows the account with no VM running. Never null; `"{}"`-shaped at worst. |
| `getAchievementsHashForPath(String)` | **Wired.** RA PS1 disc hash for an unmounted image. `""` on failure. |
| `getRichPresence()` | **Wired.** Refreshed every frame from `rc_client_get_rich_presence_message`. |
| `loginAchievements(u,p)` | **Wired.** Blocking; pumps its own HTTP so it works with no VM. `""` on success, message on failure. Persists username/display name/token/score/avatar. |
| `logoutAchievements()` | **Wired.** `rc_client_logout` + wipes the stored credentials. |
| `setHardcoreMode(bool)` | **Intentionally inert** (see §6). Logs a warning on `true`. |
| `isHardcoreMode()` | **Always false**, by construction. |
| `isHardcorePersisted()` | **Always false**, by construction. |
| `setAchievementsOption(k,v)` | **Wired.** notifications / leaderboardNotifications / overlays / lbOverlays / soundEffects persisted; encoreMode / spectatorMode / unofficialTestMode also pushed live into rc_client. `"hardcore"` is not a valid key. |
| `setAchievementsOptionInt(k,v)` | **Wired + clamped** (durations 3–30, notificationPosition 0–9, overlayPosition 0–8). |
| `setAchievementsUnlockSound(path)` | **Wired.** Stored; played via `NativeApp.playSound` on unlock. |
| `setAchievementsHostOverride(host)` | **Wired.** Persisted + `rc_client_set_host` live. |
| `clearAchievementsHostOverride()` | **Wired.** Passes `""` (not `NULL` — `rc_client_set_host` dereferences its argument). |

**Notifications — wired (2026-08-01).** `armsx_ach_set_notify_handler()` /
`NativeApp.onAchievementNotice(String,int)` → `com.armsx2.ui.WelcomeBanner`, the app-wide
transient banner `WindowImpl` already hosts above every screen. rc_client's callbacks park a line
in a mutex-guarded queue and `armsx_ach_frame_update()` drains it once per frame with `g_lock`
released — the callbacks themselves run on whichever thread was pumping HTTP, so they must never
touch JNI directly. Delivered: the game summary at boot, `ACHIEVEMENT_TRIGGERED`,
`GAME_COMPLETED`, `SUBSET_COMPLETED`, `SERVER_ERROR`, `DISCONNECTED` / `RECONNECTED`, a rejected
saved token, and a genuine (non-`RC_NO_GAME_LOADED`) load failure. The `notifications` and
`notificationsDuration` options are now **consumed**, not just stored.

**Not implemented (deliberately, out of scope):**
* Leaderboards — no submission, no trackers, no scoreboard UI. `rc_client` will still evaluate
  them; nothing is surfaced, and its five leaderboard events are unhandled.
* Challenge indicators and progress popups
  (`RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_*` / `..._PROGRESS_INDICATOR_*`). Those are
  persistent on-screen widgets; the banner is one transient slot, so a show/hide pair would flash
  it every time a challenge armed. The `overlays` / `lbOverlays` / `notificationPosition` /
  `overlayPosition` / `leaderboardsDuration` options remain **persisted and reported but
  unconsumed** — honest storage for a UI that does not exist on this core.
* Bundled unlock sounds. ARMSX ships none, so unlocks are silent unless the user imports one.

### Supporting architecture (for whoever resumes)

* **Store**: `<pref>/achievements.ini`, plain `key=value`. Deliberately not `settings.toml` —
  it holds the RA auth token, and `frontend/config.c` is read-only anyway (and contended by
  another agent). There is **no** `HardcoreMode` key.
* **Threading**: one `std::recursive_mutex` (`g_lock`) around every rc_client touch. Emulation
  thread runs `armsx_ach_frame_update`; UI thread runs `get_json` / options; a Java IO thread
  runs `login`.
* **HTTP**: `ClientServerCall` → `HttpEnqueue` spawns a detached `std::thread` that blocks in
  `kr.co.iefriends.pcsx2.HttpClient.doRequest` via JNI (`ScopedJniThread` attaches it), then
  parks the response. `HttpPoll()` — always on a thread holding `g_lock` — hands it back to
  rc_client. A **generation counter** drops responses that outlive the client they belonged to
  (otherwise: close game → reopen the RA screen → callback into freed rc_client arena).
* **Memory**: `ClientReadMemory` implements RA's PS1 map from `consoleinfo.c` —
  `0x000000–0x1FFFFF` = `psx->ram->buf` (main RAM), `0x200000–0x2003FF` = `psx->scratchpad->buf`
  (0x1F800000). Reads that straddle the boundary or exceed it return 0 bytes.
* **`EnsureAchievementsReady(env)`** runs at the top of every RA JNI entry point. It resolves
  the app files dir and calls `psxe_cfg_set_pref_path()` (a no-op once set, so `runVMThread`
  agrees with it), resolves the HttpClient/NativeApp glue, installs the handlers, and calls
  `armsx_ach_startup()`. This is what makes the library RA tab work with no VM. **Do not** let
  `psxe_cfg_get_pref_path()` be reached before this: it falls through to `SDL_GetPrefPath`,
  which on Android in-process resolves `org.libsdl.app.SDLActivity` glue that does not exist
  and aborts the process.
* `StoreLoad()` only latches "loaded" once a pref path actually exists — latching early would
  pin an empty map and silently lose the saved login forever.

---

## 5. RetroAchievements user agent — the human gate

RA identifies a client by the leading `Name/Version` token of the User-Agent, and **ARMSX1 has
no registered client yet**. The RA team issues one once the integration is finished.

Assembled in `armsx_ach_user_agent()` (`achievements.cpp`) from macros in `frontend/ra_ua.h`:

```
ARMSX1_RA_UA_NAME "/" ARMSX1_RA_UA_VERSION " (Android)"
```

Current placeholder → `ARMSX1-unregistered/0.0.0-placeholder (Android)`.

**What the user must do:** ask the RetroAchievements team to register ARMSX1 as a client and
issue a client name + version. Then create `frontend/ra_ua_secret.h` (already in `.gitignore`,
picked up by `__has_include`) containing exactly:

```c
#define ARMSX1_RA_UA_NAME    "TheNameRAIssued"
#define ARMSX1_RA_UA_VERSION "1.0.0"
```

and rebuild. Nothing else changes. **Do not** paste ARMSX2's (or anyone else's) token in —
that is impersonating a registered client and RA bans the account, not the build. A build
without the secret sends the visible placeholder, which is the intended, honest state.

---

## 6. How hardcore is kept off

Five independent layers; each one alone is sufficient:

1. `rc_client_set_hardcore_enabled(g_client, 0)` in `ApplyClientOptions()` is the **only**
   call to that function anywhere in `frontend/` and its argument is a literal `0`
   (`grep -rn rc_client_set_hardcore_enabled frontend/` proves it).
2. `armsx_ach_hardcore_active()` returns a hardcoded `false` — not read from the store, not
   derived from the client, so no future settings edit can flip it.
3. `armsx_ach_set_option()` has no `"hardcore"` key; the store has no hardcore entry, so
   nothing can persist one.
4. `getAchievementsJSON()` emits `"hardcore": false` unconditionally.
5. JNI: `setHardcoreMode(true)` logs `"ignored: … softcore only"` and returns.
   `isHardcoreMode()` / `isHardcorePersisted()` return `JNI_FALSE`.

UI: **both** hardcore switches are removed (the achievements screen's `SettingSwitchRow` and
the in-game menu's `MenuSwitchRow`), and **both** `requestToggleHardcore()` view-model methods
are inert, so the confirm dialogs can never open. `confirmToggleHardcore()` /
`cancelToggleHardcore()` and the dialogs are left in place (unreachable) so re-enabling later
is a small, obvious diff. `EmulationMenuViewModel.actionCount(Achievements)` was dropped 2 → 1
and its action map collapsed to `openAchievements()` so controller nav does not land on a
row that no longer exists.

Consequence: `unlockedMask` bit 2 (hardcore) can never be set, which the Kotlin progress
counters (`AchievementsProgress.kt`, `PlayTime.kt`) already handle correctly.

---

## 7. Console ID — cross-check with RaLibrary.kt

`achievements.cpp` uses `RC_CONSOLE_PLAYSTATION` (= 12), with a
`static_assert(kConsoleId == 12)` guarding it.

Another agent independently changed `java/com/armsx2/RaLibrary.kt` from **21**
(`RC_CONSOLE_PLAYSTATION_2`, lifted from ARMSX2) to **12**, and renamed its cache
`ra_ps2_catalog.json` → `ra_psx_catalog.json`. Verified at
`RaLibrary.kt:41 private const val CONSOLE_ID = 12` and `:45 CATALOG_FILE = "ra_psx_catalog.json"`.

**No conflict — the two agree.** Note the cache rename means any device with the old
`ra_ps2_catalog.json` will simply re-fetch; nothing needs migrating.

---

## 8. Exact next step

1. **Finish the native build.** A rebuild is in flight. It was green before another agent's
   in-flight `frontend/main.cpp` edits broke the file twice; `main.cpp` compiles again.
   Confirm exit 0, no `error:`, and that
   `android/app/src/main/jniLibs/arm64-v8a/libarmsx.so` has a fresh timestamp.
2. **Build the APK** (coordinator's job) — the Kotlin/Java edits have never been through
   Gradle.
3. **Verify the hash** (§3). Nothing else matters if this is wrong.
4. **On-device smoke test:**
   * Library → RetroAchievements → sign in. Watch `adb logcat -s ARMSX-RA`.
     Expect `startup, user agent '…'`, then `RetroAchievements transport ready`.
     Success writes `<filesDir>/achievements.ini` with `Username`/`Token` —
     `adb shell run-as <pkg> cat files/achievements.ini`.
   * Kill and relaunch: the RA screen should still show the account (store fallback), and the
     next game boot should log `signed in from saved token`.
   * Boot a game with achievements. Expect `identifying disc <path> (hash <32 hex>)` then
     `loaded '<title>' (id N), achievements=yes`. The screen's list populates.
   * Unlock one: expect `unlocked: <title> (N pts)` and the count moving on retroachievements.org.
   * Confirm **no** hardcore switch anywhere and that the account page shows the unlock as
     softcore.

## 9. Dead ends already ruled out — do not redo these

* **Do not look for `hash_disc.c` in the ARMSX2 tree or anywhere on this machine.** It does not
  exist locally (`find /Users/jpolo1226 -name hash_disc.c` → nothing). Upstream is the only source.
* **Do not use the default cdreader for `.chd`.** rcheevos' `cdreader_open_track_iterator`
  dispatches only on `.cue` / `.gdi` and otherwise falls through to raw-bin; a CHD hits that
  path and produces garbage.
* **Do not confuse `psx_cdrom_get_disc_fingerprint()` with the RA hash.** It is a local FNV-1a
  over basename + TOC for save-state matching. It is not, and can never be, an RA hash.
* **Do not call `psx_disc_close()`.** It is declared in `psx/dev/cdrom/disc.h` and **has no
  implementation anywhere**. Use the `DestroyDisc()` helper in `achievements.cpp`, which also
  handles `psx_disc_destroy()` unconditionally dereferencing a null `destroy` after a failed open.
* **Do not pass `NULL` to `rc_client_set_host()`.** It dereferences the argument
  (`rc_client.c:6903`). Pass `""` to restore the default host.
* **Do not compile rcheevos with the repo's default `-ffast-math`.** See §2.
* **Do not use a temporary rc_client for login** the way PCSX2's `Achievements::Login` does.
  One persistent client plus a short-burst pump loop is simpler here and works with no VM,
  because rcheevos is built `RC_NO_THREADS` and everything is already serialised.
