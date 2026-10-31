# Frame pacing in the SwanStation adapter

The independently authored pacing code lives in `native/swanstation/frame_pacer.h`.
Video settings and the in-game Video menu expose global/per-game configuration:

| Option | Behaviour |
| --- | --- |
| Optimal frame pacing | Uses planned presentation deadlines and recent Android Choreographer timestamps. Chooses the nearest display slot while retaining the independent emulation clock. Only long discontinuities reset the schedule. |
| Sync to host refresh rate | With VSync at normal speed, matches a display rate or integer multiple only when the resulting speed adjustment is within 1%. Incompatible pairs such as 50/60 or 60/90 remain unadjusted. |
| Reduce input latency | With optimal pacing, delays frame execution using recent useful work duration and the configured headroom. Input is read after that delay. |
| Frame-time buffer | 0–10 ms of scheduling headroom; default 2 ms. Lower values trade latency for greater deadline-miss risk. |
| Maximum queued frames | 1–3 whole frames on the threaded hardware backend and independently fenced Vulkan command/acquire resource slots; default 2. This does not limit the Android compositor's entire scanout queue. |

These choices apply on the next game start. Optimal pacing and the threaded
hardware backend default on, matching the corresponding DuckStation defaults.
Host-refresh sync and input-latency reduction default off; the buffer is 2 ms and
the queue depth is 2. Existing explicit choices remain unchanged. A legacy GL-only
worker selection takes precedence over the new hardware-worker default when no
hardware-worker choice was saved. The existing VSync preference is now consumed: OpenGL/ANGLE
use swap interval 1 at normal speed when enabled. Vulkan uses FIFO for matched
host cadence and black-frame/shader-subframe sequences. At unmatched rates it
prefers tear-free MAILBOX so an independent emulated clock does not accumulate a
blocking FIFO backlog, with FIFO fallback when MAILBOX is unavailable. With Vulkan
VSync disabled, MAILBOX is preferred, then IMMEDIATE, then mandatory FIFO fallback.
The timer still caps emulation speed. Software uses the same GL presenter policy.

Host-rate matching includes streaming stereo resampling in the audio producer.
Fractional phase survives libretro audio batch boundaries, avoiding a slowly
growing/shrinking audio queue from a 59.94/60 mismatch. The realtime audio callback
keeps its existing bounded queue and performs no resampling allocations. Fast
forward, rewind and unrelated custom speeds do not engage host-rate matching.

Android display timestamps guide the schedule when recent; a timer remains the
fallback when the UI thread cannot supply them. VSync and compositor scheduling
still determine physical scanout. There is no promise of exact display deadlines
on every device, and this does not use Android Swappy or copy current DuckStation
implementation code. Background audio keeps its normal timer when the surface is
suspended; pausing, rate changes and long stalls reset timing state.

CPU frame timing alone does not determine which refresh displays each buffer.
The presenter prepares and flushes/submits its GPU work before the presentation
wait. OpenGL/ANGLE use the selected driver's `glFlush`; Vulkan submits its prepared
command buffer and semaphore dependencies. The wait then precedes swap/present,
allowing GPU work to use the remaining budget rather than starting at the deadline.
This also applies to the Software renderer's GL upload/presentation and black
subframes. Swapchain-image semaphore ownership and command-slot fences are unchanged.
With optimal pacing at normal speed and VSync, the adapter supplies separate
compositor deadlines through `EGL_ANDROID_presentation_time` on OpenGL/ANGLE and
`VK_GOOGLE_display_timing` on Vulkan when supported. Headroom changes when a
frame is submitted without changing its display target. Subframes receive their
own successive refresh deadlines. Timestamp requests outside a bounded monotonic
clock window are discarded rather than queuing a distant future presentation.
Unsupported drivers keep the timer/VSync fallback.

Vulkan timing is enabled only if the selected driver advertises the extension.
The adapter queries its refresh-cycle duration and past actual presentation times,
using recent display feedback as the phase reference when available. Diagnostic
reports include timestamped-frame count, actual presentation intervals and extra
unrequested refresh delays. Counters restart after pause/resume or swapchain
replacement, excluding stale feedback and menu time. These metrics are distinct
from the CPU FPS counter and are needed to validate visible smoothness.
Timing feedback is collected every present because Android retains a short
history and withholds recent incomplete records. Consecutive present IDs alone
contribute to interval statistics; missing IDs and coalesced timestamps have
separate counters so missing history cannot masquerade as low display FPS.
When recent Vulkan scanout feedback anchors the planned slot, its not-before
timestamp falls halfway between that slot and the preceding refresh. This avoids
an exact-boundary request slipping to the next refresh because of small phase
estimation errors. CPU submission headroom and the emulated clock are unchanged.
Drivers without recent actual presentation feedback retain the existing timestamp
policy. Simulated phase-error tests cover the boundary case; device verification
is still required before attributing any observed stutter to it.
See [Android's frame-pacing explanation](https://developer.android.com/games/sdk/frame-pacing/).

## Vulkan ownership

Each frame slot owns a command pool, command buffer, submission fence and acquire
semaphore. A slot's fence completes before its command pool or acquire semaphore
is reused. The present-completion semaphores instead belong to **swapchain image
indices**. Reacquiring an image and waiting on the acquire semaphore orders reuse
of that image's presentation semaphore; a submission fence alone does not do this.
See the [Khronos semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

Normal frame presentation has no `vkQueueWaitIdle`. Device-wide waits remain for
swapchain/driver/shader lifecycle changes and exceptional readbacks. The core's
single shared output image is protected before replacement/destruction by checked
build-copy hooks that wait for frontend submission fences. Ordered image barriers
on the same graphics queue cover normal source-image reuse. Screenshots retain
their readback completion barrier. Shader subframes retire pending work before
reusing the same librashader frame index; shader output-size changes also retire it.

The initial test implementation rounded display slots upwards and rebased the
clock for small overruns. That compounded phase differences: a deterministic
trace reproduced 59.817 Hz content running at about 58.8 FPS and PAL 50 Hz at
30 FPS on a 60 Hz display. Nearest-slot selection and retaining the emulation
clock fix that regression. Full-minute trace tests cover both rates with input
latency reduction on and off; real display cadence still needs device verification.

Host tests cover schedule recovery, incompatible refresh rates, fast-forward,
subframes, audio batch independence and long-term audio count, plus frame-slot
ownership. JVM tests cover persistence, invalid values, per-game overrides and
Video reset ownership. Android compilation checks the real Vulkan/GL/JNI paths.
On-device cadence, latency and Vulkan validation-layer checks remain separate
requirements before treating this test build as a validated release.

## Comparison used for the October 29 test build

The reference is DuckStation commit
[`ca184adb3b4d86316e24f28ab663819ed2faacae`](https://github.com/stenzek/duckstation/tree/ca184adb3b4d86316e24f28ab663819ed2faacae),
inspected on 2026-10-29. This is a comparison of the published source, not proof
that any particular installed Android build uses identical timing behaviour.
The SwanStation adapter implements its own code; no DuckStation source is included.

| Area | Reference behaviour | Adapter status |
| --- | --- | --- |
| GPU preparation before waiting | The video presenter renders/submits before waiting to present. | Corrected for Vulkan, OpenGL, ANGLE and Software presentation. |
| Vulkan presentation mode | FIFO for host-matched pacing; MAILBOX when clocks do not match. | Implemented with capability fallback and the existing VSync control. BFI/subframes keep FIFO. |
| Host cadence matching | Optional small speed adjustment, accounting for integer refresh multiples. | Implemented, retaining this adapter's narrower 1% limit (reference accepts 5%). Existing user preference is preserved. |
| Deadline precision and phase recovery | Dedicated presentation timing, phase tracking, adaptive margin and late-frame recovery. | Independently implemented with Android presentation feedback, bounded adaptive headroom and cooldown-limited presentation recovery. Driver fallback behaviour differs; see below. |
| CPU/video frame overlap | Asynchronous frame packets with a bounded count of queued frames. | Implemented for eligible threaded hardware sessions. Whole-frame reservations are separate from Vulkan command slots; readbacks and lifecycle changes synchronize. Pointer controllers, runahead and Software retain synchronous frames. See THREADED-HARDWARE-BACKEND.md. |

Relevant reference entry points are
[`VideoPresenter::PresentFrame`](https://github.com/stenzek/duckstation/blob/ca184adb3b4d86316e24f28ab663819ed2faacae/src/core/video_presenter.cpp),
[`System::GetEffectiveVSyncMode` / phase locking](https://github.com/stenzek/duckstation/blob/ca184adb3b4d86316e24f28ab663819ed2faacae/src/core/system.cpp),
and [`GPUBackend` queued-frame accounting](https://github.com/stenzek/duckstation/blob/ca184adb3b4d86316e24f28ab663819ed2faacae/src/core/gpu_backend.cpp).
This closes the synchronous end-of-frame queue difference for eligible hardware
sessions and adds independent margin/recovery policies. Other backend differences
and driver behaviour still prevent claiming identical performance or complete
architectural parity.

## Automatic margin and presentation recovery

Both are built into Optimal frame pacing, with VSync and host-refresh matching at
normal speed. They do not have separate UI toggles. Fast-forward, unmatched PAL
cadences, black-frame insertion and shader-subframe sequences do not engage them.

The graphics owner records each presentation's intended display slot, CPU handoff
deadline and actual handoff time. Vulkan collects GOOGLE display-timing feedback;
OpenGL/ANGLE collect EGL_ANDROID_get_frame_timestamps scanout times when the selected
driver supports them. Pending/invalid/expired EGL timestamps are not treated as
misses; collection and request history are bounded. Without scanout timestamps the
adapter keeps timer/VSync pacing and backlog recovery, but cannot adapt the margin.

If an on-time handoff is displayed over half a refresh after its intended slot,
extra headroom increases by 1/32 refresh, capped so total automatic headroom cannot
exceed a quarter refresh. A user-selected buffer already larger than that stays
unchanged. Late CPU/shader submissions do not grow the margin. The extra headroom
changes CPU submission/optional pre-frame sleep, never the guest clock or display
slot. CPU and graphics communicate the adjustment only through atomics.

For a one-refresh-per-frame matched cadence, a crossed presentation deadline with
evidence of backlog, or repeated late scanout feedback, can skip one presentation.
The following frame must present, with at least eight refreshes between recovery
drops. Feedback from before a recovery cannot re-arm it. Multi-refresh cadences
retain their natural recovery slack without these drops. Emulation, audio, VRAM
operations and guest frames still execute. In Vulkan even a dropped presentation
submits the core waits/commands/signals and advances fenced resources without
acquiring a window image. OpenGL flushes already-issued rendering work.

Pause/resume, output-surface replacement and session restart reset the controller
and discard stale feedback. Diagnostics include adaptive margin, adjustment count
and recovery drops. Deterministic tests exercise a missed FIFO refresh, recovery
without a lost emulation step, cooldown bounds, late-CPU exclusion, large user
buffers, policy disabling and stale-epoch feedback. Real-device validation remains
necessary for each driver; this is not a copy of DuckStation implementation code.
