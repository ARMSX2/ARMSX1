# Avoid blocking the GPU command worker on a busy Vulkan display

The Moto G75 0.3.38 report uses 3x hardware drawing with Vulkan output. It has
193 ms and 107 ms slow display incidents, zero audio underruns, and a native
Vulkan fence wait maximum of 158 ms (only 0.33 ms of thread CPU). The largest
actual `vkQueuePresentKHR` call is 1.54 ms. The timeline's broader "present"
category also includes producer scanout, fences and other host work; it does
not establish that `vkQueuePresentKHR` itself took 171 ms.

Hardware shared output now polls the previous Vulkan submission fence before
flipping the shared picture buffer, producing scanout, or creating a producer
sync fd. If busy, it finishes submitting the guest drawing in order, retains
the two-frame GL queue bound, and declines only that host presentation request.
The following guest frame tries again. The existing blocking fence remains as
the resource safety check on accepted submissions; acquire/submit/present,
surface recovery and software upload paths retain their synchronization.

An unaccepted base picture does not emit black frames or shader subframes of
the old picture. Reports count accepted base requests separately from rejected
requests and expose `Vulkan busySkipped`. Accepted requests can include a
retained unchanged picture; they are not physical display timestamps. Host
presentation counters remain excluded from save states.

No guest clocks, audio buffering/DSP, internal resolution or shader pixels are
changed. A pending fence can reflect GPU work, driver scheduling or another
consumer of the GPU. Avoiding a worker wait does not remove that external
stall or guarantee that every display refresh gets a new picture. Retest the
same Moto scene at 3x and compare accepted frames, rejected requests, audio
underruns and worst presentation times before claiming smoother output.

The host fence test uses the production gate and checks repeated pending
polls, signalled reuse and propagation of device loss, without any linked
wait/reset/acquire/submit implementation. It is not a GPU benchmark.

Local validation: 521 core JVM tests and 201 frontend JVM tests pass. The
ARM64 native library compiles, and the host fence-gate fixture passes. No Moto
replay or device installation has been performed for this patch.
