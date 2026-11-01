# FIFA pre-movie flash and logo transitions

## Separate the intended fade from the flash

A same-device recording of FIFA 2000 (SLUS-00994) in DuckStation confirms that
the loading logo fades before the licensing screen. Measuring the central
image's luminance at 60 samples/second gave the same transition in both apps:
approximately 350 ms from 90% to 10% brightness, followed by 1,150 ms before
the next image brightens. This is not an emulator crossfade to remove.

The separate pre-movie flash was reproducible in 0.3.22: the full EA SPORTS
animation was missing and a small graphic briefly appeared at the upper left.
Disabling the recompiler reproduced the same display-register sequence at
the same guest times, so changing the allocator or presentation pacing was
not justified by this test.

## Cause and correction

The CD controller delivered a queued sector-data interrupt (INT1) while a new
command was still being received. The command response must take priority;
sector delivery remains pending until that response has been acknowledged.
In FIFA this ordering error caused the movie's Setloc command to be replaced
before execution. SeekL then used the old directory-read target.

An identical checkpoint captured before the transition demonstrates the result:

| | 0.3.22 | 0.3.23 |
|---|---|---|
| Movie target Setloc | Lost before execution | Executed before SeekL |
| Seek target in this test | LBA 181171 (previous target) | LBA 957 |
| Visible sequence | Brief small graphic, missing EA animation | Full EA animation, then movie |

`CdRom` now holds INT1 behind a pending command and avoids scheduling a one-clock
wakeup for that held interrupt. Sector reading and XA decoding continue; no
game-specific address check, extra movie delay, command queue or artificial
black frame is introduced. Optional command-write traces now include the
command being replaced, IRQ state and parameter snapshot, so a lost command
can be distinguished from a seek or rendering problem.

This follows the command/async-response distinction documented in
[psx-spx](https://github.com/psx-spx/psx-spx.github.io/blob/master/docs/cdromdrive.md)
and exercised by [DuckStation's CD controller](https://github.com/stenzek/duckstation/blob/master/src/core/cdrom.cpp).
The own-engine implementation remains in use.

## Validation

- The new register-level regression fails with the old ordering: INT1 arrives
  during command reception. It passes with the guard and also verifies that
  the held data interrupt is subsequently delivered.
- All 413 core tests and 191 app tests pass.
- Thor's eight SPU streaming/IRQ integration cases and both movie-presentation
  tests pass, including the 96 software/OpenGL/Vulkan border presentations.
- The captured checkpoint replay and a separate normal-app cold boot both show
  the complete EA animation followed by the movie on Vulkan at 4x. FIFA's Small
  crop selection remains applied; the previously fixed bottom strip is absent.
- A 150-second Harry Potter clean-boot source-audio capture reaches menu music
  without an engine fault. No input route was used to reach Ron's dialogue in
  that capture; it is not a fresh manual NPC-speech confirmation.
- 0.3.23 updates `com.nanodata.armsx` in place with the existing signing key.
  Package/update identity and 16 KiB ZIP alignment checks pass. CPU, GPU, SPU and
  tempo native libraries are unchanged.

Recordings, BIOS/disc data and diagnostic RAM dumps are private test artifacts
and are not part of this commit.
