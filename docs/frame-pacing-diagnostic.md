# Frame-pacing diagnostic v1

This is a diagnostic experiment for the reported Digimon World 3 USA
(SLUS-01436) battle stutter on an RG477V. It is not a confirmed fix. It is
based on the current `android-compose` source, not the older report build.

The APK is a separate app (`com.aaron.armsx1.pacingdiag`), named
**ARMSX1 Pacing Diagnostic**. It cannot replace normal ARMSX1. Use a separate
user-data folder; do not point it at the normal app's memory-card folder.
ROMs and BIOS can be selected again. If a battle save is needed, import a COPY
of its memory card. Do not uninstall the normal app or erase its data.

## Compare the same battle

1. Select hardware OpenGL, 2× internal resolution, normal 100% speed with the
   limiter enabled, and the same BIOS/game release as the original test.
2. Keep shaders, BFI, shader subframes, runahead and rewind off. Broad debug
   logging is not necessary and can perturb performance.
3. Open Settings → Video (or Quick menu → Display), then find
   **Frame pacing diagnostic A/B**. Start with **A: Baseline timer**.
4. Play the troublesome battle for about 30 seconds. Pause immediately and
   use Settings → App → Save diagnostic report now. Name it `battle-A.txt`.
5. Select **B: VSync + timer**, repeat the same scene/settings, then export
   `battle-B.txt`. B can help OR make things slower by waiting twice; return
   to A if it worsens performance. The selection resets to A after app restart.
6. Send both reports, noting which mode looked smoother and whether the OSD's
   emulation speed changed. A short video is still useful.

Vulkan is unchanged by the A/B selector. Its ordinary emulator timing remains
in the report, but this patch does not collect Vulkan scanout timestamps.

## What is measured

The `pacing-probe-v1` section distinguishes CPU swap-call duration and submission
intervals from Android EGL physical display-present timestamps. The latter are
collected only when the active driver advertises support. Composition-latch
timestamps are separately labelled and are not claimed to be physical scanout.
The report includes p50/p95/p99/max intervals, frame IDs, a bounded raw timestamp
tail, requested swap interval, supported swap-interval range, actual display
refresh and supported display modes. A successful swap-interval request does
not prove that a driver honoured it; the timestamp measurements are the check.

Unsupported, pending, invalid and expired timestamps are not invented or treated
as zero-duration frames. Queries are bounded and asynchronous; there is no
`glFinish`, GPU readback or extra emulated frame. Pauses, surfaces, mode/rate
changes reset cadence boundaries. Reports do not distinguish identical game
images from genuinely new animation frames, so smooth scanout alone cannot
prove the game's own animation rate.

Instrumentation has some overhead. Compare A and B in the same diagnostic APK.
Normal builds compile out the capture and ignore the diagnostic-mode JNI call.
The separate flag intentionally does not enable the broad logcat/heartbeat
diagnostic capture. Reports contain ROM paths/device details: review before
sharing publicly. No ROM, BIOS, screenshot or RAM data is captured by this patch.

## Build

From `android` in the pinned dependency checkout:

```powershell
.\gradlew.bat :app:assembleGithubDebug -Parmsx.pacingDiagnostics=true `
  -Parmsx.applicationId=com.aaron.armsx1.pacingdiag `
  '-Parmsx.appLabel=ARMSX1 Pacing Diagnostic' `
  -Parmsx.versionName=0.3.6-pacing-diag-v1 -Parmsx.versionCode=92183351
```

The Gradle guard rejects pacing diagnostics under the normal application's ID.
This debug build does not change the public-release audit/guards. No automatic
installation, GitHub push or public release is part of this experiment.

Reference APIs:
- https://registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_get_frame_timestamps.txt
- https://developer.android.com/games/sdk/frame-pacing
