# OpenGL wait diagnostics

The 0.3.21 report exported on 2026-11-01 at 09:05:44 came from an SM-A528B using OpenGL hardware drawing at 4×, with a PAL guest. Its late snapshots were approximately 50 FPS, but retained frames included long presentation and queue waits. A 50 Hz guest has a 20 ms budget; 60 FPS is not its target. The report did not retain the game title or prove where the driver blocked.

`gpu.present.gles.api` previously enclosed raster submission, scanout, window handling and `eglSwapBuffers`. It could not distinguish those causes. The opt-in native logger now also measures:

| Path | Work measured |
| --- | --- |
| `gpu.commands.upload` | CPU submission of draw/transfer command-buffer data |
| `gpu.present.gles.window` | Window acquisition/recreation, making the window current and swap-interval configuration, after pending raster commands have been flushed |
| `gpu.present.gles.swap` | `eglSwapBuffers`, including driver/buffer-queue waits |

Each path retains calls, elapsed and thread CPU time, maximum duration/time, thread ID and source function. Parent and child scopes overlap; do not add their times. These are host/API timings, not hardware GPU timestamps. No timer queries, readbacks or forced GPU completion are added to ordinary gameplay.

The scopes are disabled with the existing logger toggle. Vulkan and CPU native snapshots use the same extended timing schema, keeping prior indices stable. A device integration test verifies all three new paths, source/thread information and that disabling profiling stops their counters.

Retest the affected game on the original device at the original settings. A Thor test cannot establish that the SM-A528B's 4× workload is now within its budget.
