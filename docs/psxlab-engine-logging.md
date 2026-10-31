# PSX Lab engine diagnostics in ArmSX1

In App settings, **Enable diagnostic logging** starts local capture immediately. It does not ask for a destination. Reproduce the problem, then use **Export diagnostic report** to choose the report file. Export also works after capture is switched off. Advanced engine logging remains a separate preference; app capture temporarily ensures at least INFO detail and restores the advanced preference when disabled.

Capture uses the original PSX Lab backend. Its private `files/logs/psxlab` directory contains two rotating JSONL files, each capped at 4 MiB, and a session checkpoint. A 2,048-record queue feeds one background writer; emulation producers never wait for disk. Full queues and oversized records are dropped and counted. Storage errors appear in the live summary. A report includes log tails (512 KiB per file), the live backend snapshot, settings, engine thread stacks, recent Java crashes, Android process-exit traces and process-filtered logcat. Logs can contain local configuration paths and device/controller metadata; review before sharing.

Every structured record has a monotonic `time_ns`, unique session ID, frame correlation ID where applicable, thread name, OS TID, severity, logical path and source file/function. A new game launch resets session identity and completion-interval history. Severity values are 0 Trace, 1 Debug, 2 Info, 3 Warn, 4 Error and 5 Fatal. Detailed capture is normally off. Trace can produce substantially more records and overhead.

Per-frame records include:

- CPU emulation wall and active-thread CPU time, guest clocks, and start/end program counters.
- Requested pacing sleep, measured pacing wait, overshoot and audio buffer occupancy.
- CPU-to-GPU presentation-request queue latency and scanout/presentation worker wall/active CPU time, with both thread IDs. Queue latency includes earlier queued raster work; raster batch durations are available at Trace.
- Renderer-specific upload, conversion and presentation API spans, display state, nominal field budget, completion interval and API acceptance result.
- The largest measured phase as a **candidate** path to investigate, with its source location.

Audio snapshots every approximately 500 ms include source/DSP/written/played frame counters, input/ready/in-flight queues, total buffered frames, underrun totals and deltas, refill and drop counts, stretch ratio, cumulative DSP CPU time and producer blocked time, worst producer gap, AudioTrack sample rate/buffer/performance mode and routed output. Producer gaps, new underruns, refill events, slow DSP work (>8 ms), slow writes (>20 ms) and audio exceptions have separate records. Normal DSP/write spans require Trace; thread registration remains available at Info.

Debug adds device flushing, disc reads and GPU synchronization spans. Slow disc reads (>5 ms) and GPU fences (>8 ms) escalate to Warn even at Info. Trace additionally records CPU chunks and raster batches. These thresholds flag investigation candidates; an expected blocking audio write or vsync wait is not automatically a defect.

## Reading timing evidence

`wall_ns` includes scheduling and waiting. `cpu_ns` measures the calling thread's CPU consumption; it does not measure GPU hardware execution. Pacing wait includes the host-side pacing calculation and sleep. Presentation spans include the Java/native API call, including any internal fence, acquire, draw and swap/present waits. They do not individually split those native stages. `presentation_api_accepted` means the API accepted the operation; it does not prove the image reached the panel.

Completion intervals are GPU-worker/API completion intervals, not physical screen timestamps. No hardware GPU timestamps, SurfaceFlinger display feedback or audio waveform recording are claimed by this logger. Audio played frames are Android playback-head counters, not proof of audible speaker output. Correlate an underrun delta with producer gaps, queue levels, DSP time, CPU work and GPU synchronization in the same time window before changing code. A large phase alone is not a proven root cause. Logging itself can affect measurements.

## Verification

`EngineLogTest` covers disabled capture, filtering/escaping, bounded rotation and oversized records, correlated frame phases and storage failure isolation. `LoggerIntegrationTest` runs real BIOS/game fixtures on an authorized device and verifies frame/audio records, pause/resume, a recoverable launch error, capture without a picker, export while enabled/disabled, and failed-export recovery. Its test-only non-exported provider avoids requiring a document picker during automation; production export still uses Android's document picker.

## Validation: 0.3.12-engine-logging (92183368)

Installed in place on Thor using the release package and existing signing certificate. 344 core and 170 host JVM checks passed. Real Warped BIOS/intro capture/export checks passed independently for OpenGL, Vulkan and software, including pause/resume, recoverable launch-error stacks, export while capture is disabled and failed-provider recovery. Exported records confirmed the actual three renderer paths, distinct CPU/GPU/audio/DSP TIDs, no writer errors or dropped log records, and zero audio underruns/refills/drops during these short runs. This is a logging integration check, not a completed-level performance or audible-quality assessment. An additional eight-cycle AudioTrack shutdown test passed while reading both the old and new diagnostics.

All native libraries are byte-identical to the previously tested 0.3.11-frame-meter APK. This change does not modify guest clocks, DSP algorithms or pacing policy. Capture was disabled during cleanup and the app returned to the library.

APK SHA-256: `449babc09ee6ced5c67e5a476c9300c3478f8b5038198435a9e716e83e65cba9`.
