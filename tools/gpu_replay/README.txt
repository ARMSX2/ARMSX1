Graphics report replay
======================

On the reporting device, enable logging, reproduce the visual fault, pause as
soon as it appears, then export the usual diagnostic text report. Stopping also
captures before the app clears its surface. No separate file picker is needed.

Logging retains two bounded graphics windows (about four seconds under ordinary
load). A canonical VRAM readback seeds a new window every two seconds, or earlier
if the command budget fills. This diagnostic work can affect timing and is named
gpu.replay.checkpoint in the journal. Logging off does not capture graphics.
Only pause/stop/export writes the compressed attachment to disk. A new launch
clears the prior session's attachment. Guest save states exclude this host data.

The text report embeds the entire gzip capture as base64 with its SHA-256;
normal 512 KiB log-tail truncation is not applied to this attachment. Its metadata
identifies the game/session, renderer, CPU registers, recent CD/MDEC DMA requests
and completions, accepted CD FIFO headers, display state, and drawing state.
The capture contains VRAM game artwork and resolved GPU command data, not a ROM.

With JDK 17+ and Python installed, from the repository root:
  tools/gpu_replay/replay.ps1 -Capture report.txt -OutputDirectory C:/temp/armsx-replay

Direct .armgfx files work too. The tool writes captured.png, replayed.png, a
visible difference image, whole-VRAM images, metadata, commands and comparison.
It uses the engine's independent Java software rasterizer and needs no game,
BIOS, Android device, or external emulator. All input counts and lengths are
bounded; extraction validates the attachment checksum.

Interpretation matters: a matching replay can still show incorrect commands or
assets supplied by the CPU, including a missing menu. A mismatch can expose a
backend difference, but live texture feedback differs from the existing hardware
snapshot contract. Canonical VRAM does not capture HD detail, texture replacement
images, postprocessing, physical screen tearing, or the complete CPU execution
leading to a bad scene. Read the original logs/video alongside the replay.
