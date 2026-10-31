# Experimental hardware backend threading

The Compose frontend still uses the pinned SwanStation core. This is an independent
adapter implementation, not a transplant of current DuckStation code.

In Video settings, **Threaded hardware backend (experimental)** enables overlap
between CPU emulation and the OpenGL, ANGLE or Vulkan hardware backend. It is on
by default, supports global and per-game settings, and applies at the next game
start. Selecting it disables the earlier GL-submission-only option, and vice versa.
The Software renderer keeps its existing software-rasterizer threading option.

## Ownership and synchronization

- The emulation thread owns guest GPU registers, FIFO decoding, interrupts, timing,
  vertex decoding and PGXP lookups. These still need access to CPU-side state.
- A separate `GPU_HW` backend instance owns driver resources, vertex-buffer uploads,
  draw execution, shader/pipeline work and hardware VRAM operations. The emulation
  thread never supplies a pointer into its reusable vertex or upload buffer.
- The worker receives immutable register/batch snapshots and owned vertex/pixel
  packets. Actual driver buffer offsets and private caches belong to the backend;
  they are not overwritten by CPU snapshots. Uniforms upload after backend mapping,
  including when Vulkan rolls over its command buffer.
- The ordered queue permits at most 256 queued commands and 16 MiB of charged
  packet data. The executing command, one producer packet, staging buffers and
  driver allocations are additional memory. Backpressure stops unlimited CPU lead.
- Hardware VRAM reads, state save/load, reset, settings/cache changes and destruction
  synchronize. With software readbacks enabled, reads use the
  existing ordered software mirror; the hardware stream stays ordered separately.
- The worker owns the EGL context and presenter even for Vulkan, so a software
  fallback can still present safely. Eligible hardware sessions queue whole-frame
  rendering/presentation asynchronously. Guest timing events exist only on the
  CPU-side GPU instance.
- Disk state serialization uses the pinned core format. Internal rewind/runahead
  states use RAM VRAM snapshots instead of passing GPU-texture objects across owners;
  that can increase their cost. The worker remains alive until core teardown finishes.

`core_pipeline_hooks.cmake` makes checked build copies after the existing display,
texture, statistics and state-media hooks. It never edits the pinned core checkout.
Explicitly disabling the option retains the original renderer factories and presentation.

## Validation and limits

`tools/Test-SwanGpuPipeline.ps1` compiles the real packet implementation against a
deterministic driver double. It checks CPU progress while the worker is blocked,
owned vertex/pixel lifetimes, immutable state, actual driver offsets, uniform rebinding,
read/state barriers, repeated destruction/reinitialization and driver-error reporting.
The queue tests cover command/byte backpressure, barriers, shutdown and restart.
These are host concurrency tests, not real GPU rendering or performance measurements.

Android ARM64 compilation and the Compose JVM suite are also required. The newly
split backend still needs separate on-device correctness and performance testing
on OpenGL, ANGLE and Vulkan. Earlier device smoke tests of the GL submission worker
do not validate this new architecture. No speed gain is guaranteed; the packet
copies, synchronization and increased memory use can be slower on some devices.

The renderer label and diagnostic journal report `hardware_backend_worker=1` only
when the split backend was actually created. Merely starting a presenter worker
does not count as hardware-backend overlap. Reports include queue high-water marks.

## Bounded whole-frame presentation

The hardware-backend option now also queues complete libretro display/presentation
jobs. Ordinary hardware frames no longer drain at `UpdateDisplay`, graphics-state
restore/reset, the end of `RunFrame`, or the render callback. The CPU can build the
next frame while a previous presentation runs. **Maximum queued frames** limits
accepted complete frames, including the executing one, to 1–3. A further CPU frame
may be in preparation, and command/byte backpressure still bounds its packets.

Each frame reservation is an owned ticket released when the presentation job is
destroyed, including driver-error and dispatch-rejection paths. Display commands,
rendering and presentation stay on one ordered owner, so the core's output texture
cannot be overwritten by the next frame before its presentation consumes it.
Each native dispatch captures its frame's deadlines, cadence and aspect by value;
subsequent CPU frames cannot replace the worker's timing metadata.

CPU geometry queries use the frontend CRTC descriptor rather than concurrently
reading the backend's host-display object. Geometry changes synchronize before
resizing; surface and target replacement run on the graphics owner after earlier
jobs. Screenshots, reads, save/load/reset, settings changes, pause boundaries and
teardown retain completion barriers. Graphics timing accumulates through atomic
counters, independently of the CPU's thread-local timing. In async mode the
completed presentation time in a CPU statistics window may belong to earlier
frames; it is not GPU execution time for that particular CPU frame.

GunCon, PlayStation Mouse and runahead currently retain synchronous frames, as does
any Software fallback. They can still use the earlier command worker where
applicable. The adapter does not pass borrowed software pixel buffers to an
asynchronous frame job. Diagnostics expose `async_frame_queue`, `queued_frames`
and `peak_queued_frames`; the option being selected alone does not prove that the
whole-frame queue is active for a particular session.

Host tests block the first real pipeline presentation, prove CPU progress through
a second frame, and verify backpressure on the third. They also exercise all three
capacities, state save/load, readbacks, outstanding-frame destruction, dispatch
rejection, presentation exceptions and repeated lifecycle. Android GPU/device
testing is still needed; these tests cannot establish smoothness or driver safety
on every phone or renderer.

## Command overhead

Vertex uploads now travel in the first draw packet for their batch. An ordinary
batch submits one job instead of two; a two-pass transparency batch submits two
instead of three. The second pass reuses the driver vertex offset and receives
its own uniform snapshot. Mapping still precedes uniform binding, including a
Vulkan command-buffer rollover. Empty batches submit no upload or draw.

Commands use move-only inline callable storage, with a heap fallback for oversized
or unusually aligned captures. Every current state-carrying hardware command is
compile-checked to fit its inline storage. The host queue preallocates its ring
slots and collects up to 16 ready commands per consumer lock. Commands in that
local batch still count against both limits until execution; prefetch does not
increase the permitted CPU lead. Command order and readback/state barriers remain
unchanged.

The queue signals its consumer only when it is waiting for work and signals
producers only while one is blocked on command/byte capacity. Shutdown still wakes
both sides. Diagnostics include batch counts, largest batch, heap-backed host
commands and notification counts alongside submitted commands and producer waits.
Host regressions verify packet counts, two-pass ownership/uniforms, empty batches,
burst notifications, local-batch capacity accounting, concurrent ring wrap and
shutdown with a blocked producer, as well as the existing state and frame barriers.

These changes reduce adapter operations. They do not establish a game FPS or
power improvement without a corresponding device comparison. In the initial
Thor comparison at 3x Vulkan, both apps delivered about 60 FPS, while ARMSX1 used
more process CPU time. Those measurements used nearby camera positions in the
same Crash Bandicoot 2 level, not identical game states, and do not establish
uncapped engine throughput or equal performance across games.

## CPU diagnostic test build

`-Parmsx.cpuDiagnostics=true` enables additional two-second journal details. The
default is off. This diagnostic flag does not change JIT, fast-memory, graphics,
audio or pacing settings. It can be used in an optimized release update with the
same application ID and signing key; a debug build is not required.

The test records effective core execution/memory settings after compatibility
rules, not just the requested frontend values. Thread CPU clocks separate the VM,
graphics owner, other app threads and process total. Percentages use one core as
100%; a multi-threaded process can exceed 100%. Sleeping and driver waits do not
consume thread CPU time. Missing/restarted/shared graphics owners report `-1`
rather than a misleading zero. Pause/resume resets the window. Queue command and
notification totals provide context, not per-function CPU attribution.

For actual function attribution, leave the game running and use
`tools/Capture-SwanCpuProfile.ps1 -Serial <adb-serial>`. The installed app already
declares Android shell profiling support. The script uses the system profiler's
`--app` path, captures 20 seconds of userspace CPU-clock samples, and saves thread,
library, function and call-graph reports with the original `perf.data`. It does not
launch, restart, move or reconfigure the game. `-Package` can select another
profileable emulator for a comparison; access depends on that APK's profiling
support. No settings or saves are copied between apps.

Sampling has overhead, excludes kernel time and cannot identify all generated or
stripped code. These percentages describe the sampled CPU work, not one-core CPU
utilization. Keep unprofiled frame/CPU measurements separate. A hot function is a
candidate for a controlled experiment, not proof of the cause of an inter-emulator
performance difference.

## Independent ARM64 memory experiment

`Build-SwanCore.ps1 -CheckedLutFastmem`, or Gradle
`-Parmsx.checkedLutFastmem=true`, enables a checked RAM lookup in the pinned
SwanStation ARM64 recompiler. Both build entry points now default to on. The Gradle
staging task forwards the setting explicitly so rebuilding cannot silently
replace the selected core with a different experiment.

To disable it for a comparison, use `Build-SwanCore.ps1 -DisableCheckedLutFastmem`
or Gradle `-Parmsx.checkedLutFastmem=false`. The explicit negative switch also works
through Windows PowerShell's `-File` entry point.

The new path applies only to Recompiler + LUT with memory exceptions and
fault-driven rewrites off. MMap, Disabled and explicit rewrites retain their
existing paths. Dynamic accesses check the fast-memory base, alignment and the
read/write page entry before accessing RAM. A missing base, unaligned access,
unmapped page or protected code page branches to the pinned core's existing slow
handler, including its timing and code-invalidation behavior. It does not install
fault patches for those checked accesses. Diagnostics report
`effective_checked_lut`, in addition to the original effective settings.

The initial private tests cover all four renderers, repeated boot, audio/input,
paused RAM save/load round trips, and Vulkan runahead/rewind. Additional tests
keep the original LUT-rewrite and MMap-rewrite paths covered. These checks do not
establish compatibility for every game or device. This is an independent change
to the pinned GPL compiler, not a port of current DuckStation compiler code.

## Reusable backend packets

The hardware adapter uses a 512-slot core-owned packet pool. Only its CPU owner
acquires packets; the backend returns accepted packets through a single-producer,
single-consumer free ring. Rejected packets return to a separate CPU-only stack,
so a rejection cannot race with the backend's free-ring producer. Exhaustion has
an owning heap fallback. No pool slot returns until its command and captures have
finished, including failed commands. Shutdown still drains the host queue first.

Vertex and VRAM payload buffers travel with their leased packet and reuse their
capacity. Source memory can still be reused as soon as submission returns. A
large payload is freed on retirement; idle retained capacity is bounded to 16 KiB
per slot (8 MiB total), separately from the existing queued-payload bound. The
second transparency pass still reuses the first pass's driver vertex offset and
binds its own uniform snapshot after mapping.

CPU-owner diagnostics expose `gpu_pooled_packets`, `gpu_heap_packets` and
`gpu_payload_growths`. Host regressions cover deliberate pool exhaustion,
rejection, thousands of wraps/reuses, large payload retirement and all previous
frame/readback/state barriers. Device CPU savings must be measured separately.

## Independent ARM64 compiler experiments

`Build-SwanCore.ps1 -CpuOptimizationMask N`, forwarded by Gradle's
`-Parmsx.cpuOptimizationMask=N`, selects three independently implemented changes
in the pinned GPL SwanStation core. Future builds default to seven, enabling all
three changes just like the tested APK. Use `-CpuOptimizationMask 0` or Gradle
`-Parmsx.cpuOptimizationMask=0` to disable them for comparisons. Direct CMake builds
also default to checked LUT on and mask seven; explicit cached overrides remain
respected. These are compiler changes, not user-facing
renderer toggles or a replacement with current DuckStation source.

* Bit 1 combines byte/halfword loads with sign or zero extension. Constant RAM,
  constant ROM and dynamic memory paths preserve final register width; protected,
  unaligned, isolated and unmapped accesses retain the existing bus fallback and
  timing behavior.
* Bit 2 uses conservative instruction lookahead to choose register evictions.
  Identity operations can reuse a cached register when its old guest value is
  provably overwritten before any read, trap, memory access, coprocessor or block
  exit. Pending loads, branch delay slots, unknown instructions and PGXP fence
  destructive reuse. Scratch-register ownership is never copied.
* Bit 4 changes frequently invalidated RAM code pages to manual instruction-word
  validation after four invalidations within 60 emulated frames. Such blocks
  cannot link around their guards. Store-containing manual blocks fetch live
  instructions through the interpreter, including stores changing a later
  instruction in the same block. Rapidly changing blocks can fall back after
  three recompilations in 15 frames. Cache reset clears the page policy.

All changes are generated into build-directory source copies; the pinned core
checkout remains unchanged. The mask and counters appear in CPU diagnostics.

The paused, private-app native probe compares 70 memory-load cases and 96 seeded
register vectors with the interpreter or the unchanged unchecked bus path where
unaligned-access policy differs. It checks architectural registers, instruction
timing, exceptions and load delays, and requires the new paths to execute. Thirty
self-modifying-code checks use the real fast-map dispatcher, including a store
changing a later instruction. RAM, CPU state and settings are restored afterward.
On Thor the probe passed with 12 fused-load emissions, 5,306 guided evictions and
97 register renames. These are probe counters, not game performance measurements.

The separate private functional matrix passed twice for Vulkan, OpenGL, ANGLE
and Software with checked LUT, and for Vulkan with the original LUT-rewrite and
MMap-rewrite paths. It covers boot, audio/input, pause/resume and RAM save-state
round trips; it does not establish compatibility with every game or device.

In the 2026-10-30 Thor ABBA comparison against the previous checked-LUT plus GPU
packet-pool build, process CPU averaged 37.10% of one core before and 37.13% with
mask seven. Each pass loaded the same Turtle Woods checkpoint, warmed for ten
seconds and captured thirty seconds without a profiler at Vulkan 3x. Both builds
ran about 59.8 emulated FPS with zero audio underruns. CPU frequency was automatic
and the battery temperature stayed at 32 C. This is effectively unchanged within
the observed run-to-run variation, not a demonstrated additional CPU saving or
DuckStation parity. Raw captures and analysis are in `tmp/cpu-three-*`.
