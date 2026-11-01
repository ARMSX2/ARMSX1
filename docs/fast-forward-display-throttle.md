# Fast-forward display throttling

The 0.3.29 backend removed the normal clock limiter during fast-forward, but
queued a host presentation on every guest vblank. Vulkan's FIFO swapchain still
waited for the screen. Since presentation and guest drawing share the ordered
GPU worker, the display wait backpressured the emulator instead of accelerating
the scene. A static/offscreen fixture and an audio-only test did not expose this.

## Reproduction

AYN Thor, physical display, original animated PS-X EXE changing its full picture
on every guest vblank. Speeds are guest clocks / 33,868,800 / wall seconds, not
the FPS counter. Each session uses a private directory and private memory cards.

0.3.29, requested 4x:

| Renderer | Normal | Fast-forward | Back to normal |
|---|---:|---:|---:|
| Software | 0.994x | 4.004x | 1.003x |
| OpenGL | 0.994x | 4.005x | 0.999x |
| Vulkan | 1.003x | 0.999x | 1.003x |

An initial excessively frequent MMIO-polling fixture was CPU-bound even at
normal speed and was discarded. The checked-in fixture batches its polling
intervals so normal speed is verified before measuring acceleration.

## Change

- Finite fast-forward uses monotonic guest-clock deadlines at the selected
  multiplier, including when fast-forward audio is muted. Zero remains uncapped
  by the clock limiter; actual speed still depends on the device/game and audio
  processing capacity.
- Host presentations during fast-forward are limited to 60 Hz or a lower reported
  screen refresh rate, with at most one host presentation queued or executing.
  Intervening guest fields flush and execute their draws and VRAM
  transfers in order without acquiring/swapping a host display buffer.
- The same policy applies to software, OpenGL, Vulkan and HD presentation.
  Normal-speed audio and pacing retain their existing path.
- Logs label intentional omissions as `fast_forward_skip`, with zero requested
  host presentations, rather than treating them as failed presents. Performance
  sampling records zero presentation time for these fields.
- A guest field completion still contributes to the existing guest FPS counter;
  it does not claim that every accelerated field was physically displayed.

## Regression coverage

`FastForwardPacerTest` covers fractional/finite/custom speeds, unlimited, live
speed changes, clock resets, host stalls, pending-display ownership and inaccurate
60/120 Hz refresh metadata.
`GpuBatchOrderTest` verifies that every intervening draw/upload survives omitted
host presentations. `FastForwardSpeedTest` uses a real visible Android Surface
and can run all menu speeds, muted/unmuted fast-forward, and an explicitly
supplied owned game. The existing fast-forward audio tests check PCM continuity,
time compression and normal tempo/gain restoration.

## Refresh-rate mismatch found during final validation

The first patch reached each requested speed at a reported 60 Hz. Repeating it
after Android switched to reporting 120 Hz exposed the same bottleneck again:
the window still accepted approximately 60 presentations/second. Requested 2x
then measured 1.008x. The final policy also bounds outstanding presentations;
display backpressure therefore cannot create one blocking swap per guest field
even when refresh metadata is stale or differs from the window's granted rate.
The device test accepts `reportedHz` to exercise this regression deliberately.
