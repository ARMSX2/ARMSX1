# Original-engine performance audit — 2026-10-31

These changes retain PSX Lab as the only Android emulation engine. They do not
alter guest clocks, audio processing, or the speed limiter.

## Shared changes

- Reuse unchanged native-resolution GLES scanouts, as the HD path already did.
  Surface identity, size, display state/filter and writes to visible VRAM
  invalidate reuse. This does not skip changed pictures or guest fields.
- For software/GLES scanout, upload selected full-width VRAM row bands rather
  than the entire 2 MiB buffer. Handle wrapped rows, packed 24-bit and interlace.
- Reuse the Java ARGB array for native-resolution Vulkan staging. Captures and
  save-state frames retain their own arrays.
- Use per-swapchain-image presentation semaphores for native Vulkan output,
  retaining submission fences and teardown/recreation waits while removing the
  redundant per-frame queue-idle wait. See the [Vulkan semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
- Cache device deadlines until an MMIO scheduling change or clock dispatch;
  increase bounded CPU/native slices from 20,000 to 100,000 guest clocks while
  retaining native field-boundary exits.
- Remove the frontend Choreographer callback feeding an unconsumed legacy API.
- Read the completed-frame counter once for each FPS sample. Previously the
  GPU worker could advance it between the rate calculation and the next sample
  baseline, causing a completed field to disappear from the FPS calculation.
  No rate is rounded up or forced to a nominal value.

## Measurements

AYN Thor, Adreno 740; separate 30-second Warped cold boots, USA BIOS, native
resolution. Thread CPU is percent of one logical core, sampled over the final
20 seconds. These are short single runs, not controlled sustained GPU benchmarks.

| Renderer | VM CPU before / after | GPU-worker CPU before / after |
| --- | --- | --- |
| Software / GLES display | 30.00% / 30.29% | 7.39% / 2.54% |
| GLES compute | 37.59% / 33.60% | 4.54% / 1.85% |

All three candidate renderers completed their boot test at approximately 59.83
fields/s with zero reported audio underruns, refills or dropped audio. Vulkan
candidate CPU was 23.71% VM and 11.93% GPU worker; no valid Vulkan baseline was
collected, so no Vulkan performance improvement is claimed from these numbers.

A separate two-minute run through the normal Compose host at the existing 3x HD
setting reproduced 57.8 FPS readings while request cadence averaged approximately
16.715 ms. Its first 15-second segment included one request interval over 25 ms;
most later segments had maxima below 17.1 ms. GPU-ready latency and request timing
are diagnostic timestamps, not physical screen presentation timestamps. Correcting
the counter race removed the persistent approximately two-FPS undercount in the
repeat capture, and the user reported a steady counter. This does not prove that
all gameplay scenes or devices are free of presentation stalls.

## Validation and reproduction

- 330 shared-core JVM tests and 170 Compose-host JVM tests passed.
- On-device native CPU/GTE/SPU/GLES/Vulkan verification passed.
- 128 partial-upload cases matched full-VRAM scanout, including 15/24-bit,
  interlace, disabled display, wrapped rows and revision changes.
- Static/changed-picture Surface tests passed PixelCopy checks. For each software
  and hardware case, 120 static requests produced one presentation and 119 reuse
  hits; all 120 subsequent changed requests were presented.
- Main APK package and signing identity were verified before updating the existing
  com.nanodata.armsx installation. User data was preserved.

The debug-only EnginePerformanceAuditTest harness requires explicit instrumentation
arguments: performanceAudit=true for gameScene/presentationWork/normalGameScene,
or nativeCheck=true for nativeCorrectness. normalGameScene enables the existing
Minimal OSD and records the real frontend in bounded 15-second trace chunks.
Game fixtures remain on the device; no BIOS, disc image, user save or capture is
included in Git. Broader level, HD-load, surface recreation and weaker-GPU tests
remain necessary before claiming universal stable performance.

## Second cleanup pass — 0.3.9-work-cleanup, version code 92183365

The CPU VM and PSX-GPU worker already run independently. A GP0 VRAM-read header
now queues preceding drawing without waiting for it; the first actual data read
still fences and publishes rendered VRAM. An asynchronous regression test holds
the GPU worker busy and checks that the header proceeds while the data read waits
and returns the correct pixels. Device checks also cover an intervening upload
across software, GLES, Vulkan and 3x HD configurations.

GLES uniform locations are queried immediately after each program links and reused
for its lifetime, including the HD dirty-row array. Native Vulkan Java uploads now
copy directly into persistent, owned, coherent staging memory after waiting for the
prior submission fence. This removes the intermediate per-frame vector allocation
and copy, without pinning Java memory across a driver wait. Six GPU readback
fixtures cover both upload paths, source resizing and exact channel/alpha bytes.
Nonzero-audio diagnostics accumulate locally per PCM block before publishing their
volatile count; PCM values, DSP processing and AudioTrack routing are unchanged.

331 core JVM tests and 170 host JVM tests passed. On-device native correctness
checks and 30-second boot comparisons passed for all three renderers at native
resolution; GLES at 3x also passed. All candidate runs reported approximately
59.83 fields/s and zero audio underruns/refills/drops.

| Native renderer | VM CPU before / after | GPU-worker CPU before / after |
| --- | --- | --- |
| Software / GLES display | 33.75% / 33.05% | 2.95% / 2.90% |
| GLES compute | 32.51% / 32.90% | 1.85% / 1.85% |
| Vulkan display | 23.66% / 23.76% | 11.93% / 11.53% |

These small differences are within short-run scheduling/clock variation; no large
CPU improvement or universal performance guarantee is inferred. The final HD run
also includes the last cached HD dirty-row lookup. Native-resolution comparison
binaries differ from the final APK only in that HD-only cached lookup.

The final installed APK SHA-256 is
81476477136ebdfd54b8003e904005220b71c7d8676d2fc25b5a704f81739ea5.
The existing package and signing key were retained and no separate emulator app
was installed. Native timing was preserved: 53,693,175 GPU clocks divided by
3,412.5 clocks/line and 263 lines gives approximately 59.8261 NTSC fields/s.
An exact 60.00 wall-clock target would require a distinct host-refresh sync policy
and corresponding audio-rate handling; it is not an optimization to guest clocks.

## Third pass — Vulkan reuse, stable swapchains and BIOS FPS measurement

0.3.10-frame-reuse (92183366) adds a host-only converted-pixel cache for native
Vulkan output. VRAM revisions and pixel-affecting display state invalidate it.
Native presentation also keys reuse on retained window identity, dimensions and
filter, avoiding submissions for an unchanged image. Captures still own separate
pixel arrays, and the cache is excluded from serialized machine state.

The surface test exposed a repeated Android VK_SUBOPTIMAL_KHR result even when
surface properties were stable. The old code recreated its swapchain and waited
for the device on every such successful presentation. The new code queries
surface capabilities and retains a usable swapchain until those capabilities
change. VK_ERROR_OUT_OF_DATE_KHR still forces rebuilding. This follows Vulkan's
[successful-suboptimal result contract](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html),
while preserving the existing upright identity-transform output policy. Both
native and shared-buffer HD presentation use this handling. Source-image resizing
also retains its submission-fence dependency without an additional device-idle
wait: only swapchain images are owned by presentation.

The synthetic surface test submitted 120 unchanged requests with one presentation,
119 reuse hits and one upload. All 120 changing requests were presented, and one
swapchain handled that entire stable-surface sequence despite 121 suboptimal
results. PixelCopy matched expected pixels after filter, size, surface recreation,
display-position, packed-24-bit and display-enable changes.

Native CPU device callbacks now use typed JNI argument arrays, preserving all
argument values and event/interrupt timing. On-device differential verification
passed. A shutdown race found in the baseline's audio diagnostics is fixed by
serializing track queries with release and retaining the final diagnostic values.
String formatting runs outside the PCM queue monitor during playback. PCM samples,
buffers and DSP algorithms are unchanged; eight concurrent query/close cycles pass.

Short native-resolution Vulkan comparisons showed GPU-worker CPU changing from
11.53% to 2.90% of one core, while VM CPU changed from 23.76% to 32.20%. Audio/DSP
threads bring the respective totals to approximately 41.93% and 41.44%. No total
CPU saving is established by those single runs. Request-to-GPU-ready mean fell
from 3.093 ms to 0.657 ms; 95th percentile changed from 3.945 to 4.231 ms and maximum
from 12.122 to 17.908 ms. These timestamps are not physical display timestamps.
The measurements do not establish elimination of occasional stalls.

### HD validation

All six 30-second BIOS/early-intro cases below passed on Thor's 1080p display.
Screenshots were taken seven seconds after launch, before the CPU sampling window;
all were upright. Renderer labels represent the selected mode. The software HD
selection shares the GLES HD drawing/display implementation with OpenGL, and
Vulkan HD uses GLES drawing plus shared hardware-buffer presentation. Internal
multipliers do not imply fixed pixel dimensions for every PS1 video mode.

| Selected renderer | Internal multiplier | Final field FPS | Audio underruns |
| --- | --- | --- | --- |
| software | 3x | 59.826 | 0 |
| hardware | 3x | 59.828 | 0 |
| vulkan | 3x | 59.824 | 0 |
| software | 5x | 59.827 | 0 |
| hardware | 5x | 59.825 | 0 |
| vulkan | 5x | 59.827 | 0 |

These are boot/intro checks, not completed-level performance or tests of every GPU.
HD Vulkan reports one swapchain and asynchronous shared-buffer frames, confirming
it did not silently fall back to a different presentation backend.

### Final FPS readout — 0.3.11-frame-meter (92183367)

The user narrowed the remaining brief 57.x reading to the final BIOS logo/game
handoff, with no visible pause or audio glitch. The previous half-second FPS sample
used CPU-thread time boundaries with asynchronously completed GPU frame counts.
A completion moving across that boundary could change the short sample by one
field (about two FPS). FrameRateMeter now measures intervals between completed
fields on the completion thread, with the same half-second reporting period.
It resets after pause/fast-forward changes, does not clamp to a nominal rate, and
unit checks confirm that actual stalls lower the value and faster rates can
exceed 60.

338 core JVM checks and 170 host JVM checks passed. Native correctness and a
one-minute run through the normal Compose game host passed. That run ended at
59.8234 fields/s with zero audio underruns/refills/drops; transition samples around
26–29 seconds were about 59.83–59.86. The user reported that the readout stayed up
through the handoff. Initial startup samples can still vary; no artificial fixed
FPS value is displayed. All native libraries in this final APK are byte-identical
to the HD-tested 0.3.10 build. The FPS change leaves emulation speed and audio
processing intact.

Final APK SHA-256:
fa70444559d341e9dd24da8e8cfae191dc83fe44287b0a2c6092d9f66a0e0a01.
The existing main package/signing identity was verified and updated in place.
Local Windows builds required nonsynced temporary output directories after the
synced workspace began denying deletion of Gradle intermediates; this changed
build locations only.
