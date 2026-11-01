# ArmSX1 0.3.30 fast-forward validation

AYN Thor, 2026-11-01. Actual guest clocks divided by 33,868,800 and elapsed
monotonic wall time; these results do not come from the FPS overlay.

## Final visible-surface results

The original animated test program changes its full picture every guest vblank.
All three outputs deliberately receive 120 Hz metadata, including when Android
grants the window a lower presentation rate.

| Renderer | 1.5x | 2x | 3x | 4x | Unlimited, audio active |
|---|---:|---:|---:|---:|---:|
| Software | 1.500x | 2.000x | 2.998x | 4.000x | 7.071x |
| OpenGL | 1.500x | 2.000x | 3.000x | 4.000x | 7.123x |
| Vulkan | 1.499x | 2.000x | 3.000x | 4.000x | 7.270x |

The optional fast-forward-mute path also passed every speed on all three outputs:
30 renderer/speed/mute combinations in total. Finite multipliers changed live
while fast-forward stayed enabled. All sessions returned to approximately 1x;
the final matrix reports zero underruns, refills and dropped audio frames.
The matrix's master volume is zero to keep the repeating test animation quiet;
its unmuted fast-forward path still runs the complete audio producer/DSP/writer.
Separate audio tests check positive gain and continuing PCM output.

Owned Final Fantasy IX Disc 1, Vulkan: normal 0.997x, requested 2x measured
1.999x; a separate unlimited run reached 6.241x. Both returned to approximately
1x with zero audio underruns/refills/drops. These were fresh private sessions,
including BIOS-to-game progression, rather than the user's save slots.

## Checks and scope

- 441 core and 193 frontend unit tests passed: 634 total.
- Seventeen device checks passed: two full renderer/speed matrices, FFIX 2x and
  unlimited runs, three audio checks, eight SPU streaming regressions and two
  movie-presentation regressions.
- Audio checks cover default audible gain at 1.5x/2x/3x/4x/unlimited, continuous
  unlimited PCM, fixed Android sample rate and restoration of normal tempo.
- GPU ordering regression verifies that omitted host presentations retain every
  guest draw and transfer. Pending-present tests cover stalls and stale refresh
  metadata. Source-clock tests cover fractional speeds, unlimited, live changes,
  host stalls and long sessions.
- All nine packaged native libraries match 0.3.29 byte-for-byte. The prior FFIX
  decoder, renderer, audio, disc and compatibility fixes remain in place.

The presentation policy is shared by software/OpenGL/Vulkan, including their HD
paths. Physical testing here used Thor; maximum acceleration remains dependent
on game and hardware. Unlimited audio leaves processing headroom to remain
supplied; it does not promise a particular multiplier. Normal playback retains
its existing clock pacer and audio DSP path.

## Artifact

- Package `com.nanodata.armsx`, version `0.3.30` / `93486626`, non-debuggable.
- `ArmSX1-0.3.30-fast-forward.apk`, 63,287,831 bytes; Desktop outside OneDrive.
- SHA-256 `b817e61d77de7ae04c25119a3070de58cbf1655e76e19fa6f3deca715851cf2a`.
- Release signing identity and 16 KiB ZIP alignment verified; updated Thor's
  existing app without uninstalling it. Tests used isolated cards/state roots.
- Both user memory-card hashes match the pre-test values. The launcher-less test
  helper was removed and the normal release app reopened after validation.

See [the presentation cause/fix](fast-forward-display-throttle.md) and
[unlimited audio adaptation](unlimited-fast-forward-audio.md).
