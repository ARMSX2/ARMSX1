# One-frame prediction reuse

`runahead_prediction.cpp` compares the production `RunaheadPrediction` helper
against executing the same frame normally. It uses a caller-supplied BIOS and
optionally a disc image; neither is included in the repository.

Run with the Android native libraries beside the executable:

```
./runahead_prediction BIOS verify [DISC [FRAMES]]
./runahead_prediction BIOS baseline [DISC [FRAMES]]
./runahead_prediction BIOS fast [DISC [FRAMES]]
```

Append `two-cards` after DISC and FRAMES to attach two in-memory test cards.
This also tests that changes to either card reject a cached prediction. These
fixtures have no file paths and cannot overwrite user saves. Run
`state_mcard_restore` and `state_mcard_divergence` to check image restoration,
cached fingerprints, truncated data and existing save-divergence warnings.

`verify` executes the reference frame, restores the starting point, then checks
the cached/uncached result against it. Checks include serialized machine state,
SPU samples and scheduling state, loop-address latches, and CD transport state.
Button changes, analog changes, external RAM writes, renderer/audio options and
explicit invalidation must all reject the previous prediction. `baseline` and
`fast` print final machine-state and mixed PCM hashes; these must match.

Default length is 360 frames. The Android regression used 900 frames with a
local CHD and confirmed identical state/audio hashes, with 845 reused frames
and 55 normal executions for changed inputs/settings/state. This is a bounded
regression test, not a claim of compatibility with every game.

The single-frame optimisation applies to runahead level 1 with software
rasterisation, PGXP off, emulated CPU clock at 100%, and texture replacement
and primitive capture inactive. Levels 2-5 use `RunaheadSequence` with the same
requirements. OpenGL/Vulkan presentation is independent of software
rasterisation. Other configurations retain the existing runahead path.

The sequence retains up to five start/end pairs. An exact starting-state match,
including the fractional audio phase and sample rate, permits reuse of the
remaining consecutive predictions. Changed input/state rebuilds them. Only
real-frame audio reaches the device; intermediate predictions mix privately
and canonicalise their state exactly as a real frame would. Pause, state loads,
rewind, unsupported modes and depth/rate changes invalidate the chain. Memory
is bounded (roughly 80 MiB at depth 5 for current states), and freed when unused.

`runahead_sequence.cpp` checks both real and future frames against independently
executing each frame. With the native libraries beside the test executable:

```
./runahead_sequence BIOS verify DISC 540 two-cards 2
./runahead_sequence BIOS verify DISC 540 two-cards 3
./runahead_sequence BIOS verify DISC 540 two-cards 4
./runahead_sequence BIOS verify DISC 540 two-cards 5
./runahead_sequence BIOS verify DISC 540 two-cards 2 switch
./runahead_sequence BIOS verify DISC 540 two-cards 2 switch irq
```

The `switch` variant changes depth and audio rate while running; `irq` also
exercises the CD-audio capture interrupt divider on every mix. Speculative
mixing must leave the real timeline's process-local divider untouched.
All modes test
a change in fractional audio phase that preserves the next integer budget.
Use `baseline` and `fast` instead of `verify` for independent state/audio hashes
and timings. A cache miss still requires fresh execution of the selected number
of future frames, so these improvements do not guarantee full speed in every
game, at every depth or with constantly changing inputs.

Performance timings no longer automatically disable runahead. The level is
user controlled. Actual snapshot failures still stop runahead because proceeding
without restoring the real timeline would corrupt emulation.

Disk save files are unchanged. The additional state is transient and local to
the prediction cache: ordinary save-state loading intentionally discards queued
audio and normalises some registers, which would be incorrect when substituting
an already computed frame.

Also run `gpu_triangle_regression`, `gpu_bios_font` and `runahead_snapshot` against
the final library. Native PGO profiles must be regenerated after source changes;
the repository's `tools/pgo.sh` workflow documents training and source checks.
The device validation used a matching locally trained profile. Profiles and
user-supplied BIOS/disc images are not included in this change.
