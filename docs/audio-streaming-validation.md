# Audio streaming validation — 2026-11-01

Harry Potter and the Chamber of Secrets (USA) failed to start its stereo title
music and lost the dialogue following Ron's first line. The initial recording
was described as a mid-sentence cutout. Comparing the complete sequence with
DuckStation established that the following lines were absent from our output.

## SPU request DMA

Channel 4 previously copied every request-mode block and raised its completion
interrupt inside the CHCR write. A game publishing its in-flight request after
starting DMA could receive the callback too early. The regression fixture runs
that ordering as guest MIPS code, including the interrupt handler: it observes
the unpublished value before the fix and the published value after it.

Request-mode SPU DMA now transfers the first block, yields while that block
drains, and schedules the remaining blocks. The nominal drain budget is 16 CPU
clocks per halfword. MADR and BCR advance per block, completion follows the final
block, disabling DPCR pauses progress, and clearing CHCR cancels it. Pending
blocks and their remaining delay survive save states. Older states default to
no pending SPU request.

This remains a functional timing model: CPU bus stalls, unusual block sizes,
and a complete SPU FIFO/DREQ implementation remain approximate. Single-block
requests retain their immediate completion behaviour.

## Other independently tested corrections

SPU IRQs now latch until acknowledgement, check transfers at the address
boundary, and check already-loaded blocks when the IRQ address or enable is
changed. CD controller interrupts now generate an edge when their line rises;
clearing I_STAT alone does not generate another edge. Legacy CD states derive
their existing line state from the saved flag and enable registers.

Those interrupt corrections alone did not restore this game's missing audio.
The request-DMA change is the candidate which restored the missing streams in
the controlled replay.

## Validation

- 405 core JVM tests and 191 Android host unit tests passed.
- Eight SPU integration tests passed on Thor, including the callback fixture
  with Java, native interpreter, and ARM64 recompiler execution.
- Java and JNI SPU tests cover interrupt boundaries, live streaming refills,
  fractional/high pitches, and save-state continuation. Six frozen PCM checksums
  remain unchanged for static streams.
- A clean BIOS boot selected English and let the title's attract sequence run.
  Before request-DMA scheduling, the stereo music voices never received key-on
  commands. Afterwards voices 20 and 21 started together, with the expected
  separate left/right routing, and the subsequent dialogue was present.
- Paired recordings from the same disc in DuckStation and the candidate contain
  the following Ron dialogue in the same order. Earlier candidate recordings
  contained only the first line. A two-second title-music segment also aligned
  with the reference at a normalized correlation of about 0.96.

These observations concern the tested Harry Potter opening on Thor. Listening
confirmation on the player's normal game screen is a separate final check.
Temporary waveform capture and live logging were restored to their original
settings after the test. The APK updates `com.nanodata.armsx` with the existing
release signing identity and uses the PSX Lab engine.
