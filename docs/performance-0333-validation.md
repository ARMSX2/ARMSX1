# ArmSX1 0.3.33 performance and diagnostics changes

## Changes

- Audio route discovery runs before acquiring the monitor shared with playback/DSP.
- Finite fast-forward tempo follows measured sustainable guest audio production. Bounded input-queue waits are excluded from capacity estimates; normal locked-tempo playback remains 1x. Reports include selected/requested and measured production tempo as well as the actual stretch ratio.
- Audio refill after pauses/starvation is capped at 2x instead of releasing the emulation thread without pacing. Recovery resets rate-controller history.
- The GPU submission queue packs up to 64 draw packets (previously 16) into a 2,048-word buffer. Transfer, readback, state and presentation ordering boundaries still flush it. Software JNI releases its VRAM critical pin every 16 packets, retaining the previous GC bound inside the larger JNI call. Queue-batch counters are distinct from physical driver dispatch counts.
- Hardware-register reports count reads/writes, access widths and the last guest PC, including CD shadow-latch paths. The hottest 16 addresses include subsystem/source paths. These are cumulative access counts, not per-register wait-time measurements. Fixed native counter storage is about 96 KiB per CPU; recording is gated by profiling.
- ARM64 blocks allow up to 64 instructions, safe non-memory/non-GTE branch delay slots, HI/LO moves and multiply/divide, and supported GTE calls through the shared JNI-free C++ geometry helper. Event/run-budget prefix guards, RAM/alignment/MMIO/CU2 checks and interpreter fallbacks remain. GTE arithmetic is shared, not replaced with a new SIMD implementation.
- Older 512-word GPU save-state scratch buffers expand on load. New host batch counters stay outside save states. CPU state layout is unchanged.
- The existing session timeline and detailed late-frame, CPU/GPU, presentation, pacing and audio logger changes are retained.

## Local verification (2026-11-03)

- Core release JVM suite: 488 tests, no failures/errors/skips.
- Compose app JVM suite: 195 tests, no failures/errors/skips.
- Actual emitted AArch64 executed in a local Unicorn harness: 8,140 cases with seed 24301 and 2,140 cases with seed 9247. Across those runs, 170,239 guest instructions and 28,034 GTE helper calls matched the scalar pipeline/ABI checks. The latter run includes 5,000 event-budget prefix calculations against a scalar clock oracle. Native hardware-register counter checks passed.
- ARM64 callbacks deliberately clobber caller-saved registers and flags; complete CPU state, RAM, code generations, stack and callee-saved registers are checked. Geometry arithmetic uses the existing shared helper and is not an independent GTE numerical comparison.
- Android native libraries built successfully. Android audio/native timing instrumentation sources compile; they were not run because the user requested local verification only.
- One earlier recorder test timed out while simultaneous builds were busy; the complete subsequent 488-test run passed. No production recorder behavior was changed to hide that timeout.

Commands for the native harness are in `tools/arm64_validation/README.md`. Standard checks are `psxlab/gradlew -p psxlab -Poptimized=true :app:testReleaseUnitTest` and `android/gradlew -p android -PpsxlabRuntimeTest=true :app:testGithubDebugUnitTest :app:assembleGithubDebug :app:compileGithubDebugAndroidTestJavaWithJavac`.

No device performance, audible fast-forward quality or driver/physical presentation improvement is claimed from these local checks. Installing on Thor was separately authorized; it is an in-place update with no uninstall and no automated gameplay run. The Pixel remains untouched.
