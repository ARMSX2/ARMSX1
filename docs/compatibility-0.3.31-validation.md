# 0.3.31 compatibility candidate: changes and validation

This is a built candidate, not confirmation that every reported game is fixed.
No game-specific patches, BIOS dumps, game images or private user reports are
included in these commits. The engine remains PSX Lab.

## Changes

- MDEC output DMA can drain available blocks while the rest of the transfer is
  still being decoded. Completion remains deferred; the final block raises the
  completion interrupt. Partial transfers and older pending-transfer save states
  are covered by regression tests.
- Polygon UV and Gouraud attributes use the same half-unit rounding in the Java
  and native/shared compute rasterizers. Sprite coordinates retain their exact
  values. Analytic fixtures cover transparent texel holes and colour boundaries.
- CD Setloc acknowledges valid BCD coordinates, including the lead-in/pregap.
  A later sector read still determines whether the image contains that sector.
- Executable validation accepts the console's decoded RAM mirrors and physical
  wrapping, retaining the 2 MiB payload limit and rejection of unmapped ranges.
  ISO boot loading can read declared payload padding from the final physical
  sector; it does not manufacture bytes beyond that sector.
- PBP/CHD container identity takes precedence over a misleading PS-X EXE prefix
  in the disc system area, in both adapter and standalone media-opening paths.
- Normal presentation permits one outstanding host presentation. Guest GPU work,
  transfers, audio and clocks continue when an additional host scanout is omitted.
  Active black-frame/shader-subframe modes retain their every-field path.
- Reports retain game identity after stopping, include MDEC progress and boot
  header details, and sample Android thermal status at five-second intervals
  while host diagnostics are enabled. Thermal status alone is not a measurement
  of a particular CPU/GPU clock or proof of throttling.

## Completed checks

- Core JVM suite: **450 tests passed**, no failures, errors or skipped tests.
- Android frontend JVM suite: **193 tests passed**, no failures, errors or skips.
- Compact GitHub APK assembly: successful, including native compilation and R8.
- Controlled Thor presentation test: injecting 32 ms of GPU presentation latency
  reduced the previous Vulkan build to approximately **0.502x** guest speed.
  The candidate measured **0.999x Vulkan**, **1.000x software**, and **0.998x
  OpenGL**, using guest clocks divided by wall time rather than an FPS overlay.
  This is an injected-stall regression test, not a measurement on the reporters'
  devices or a claim that physical scanout remains 60 Hz during a display stall.
- The misleading-PBP-header fixture failed on 0.3.30, reproducing the erroneous
  EXE classification. The updated device fixture still needs its candidate run.
- Dragoon's controlled 180-second intro/title capture completed without engine
  errors or reported Android underruns/refills/dropped frames, first at 2x then
  normal speed. Audible crackles were nevertheless reported on Thor; those
  counters are not proof of clean generated sound. Audio diagnosis remains open.

Updated device tests for native/HD polygon rounding, executable RAM mirrors and
logger fields compile, but their final device runs remain pending. No testing or
installation should interrupt a player's active session.

## Reported-game status

The seven compatibility reports were from 0.3.26 on a Galaxy A55 using Vulkan at
native resolution. Their exact game images were unavailable for local testing.

| Report | Evidence and remaining validation |
| --- | --- |
| London Seirei Tanteidan | Loader rejected an executable RAM range. Container classification and mirrored-range fixes need retesting with the reported file. |
| Popolocrois Monogatari 2 | Loader rejected a truncated/invalid payload in a multidisc PBP. Container identity and sector padding fixes need exact-file retesting. |
| Ogre Battle | Repeated all-zero Setloc commands matched the rejected-pregap case. The command-level regression passes; the game loading screen is unverified. |
| Linda3 | Alternating movie buffers were logged. Shared decoder/raster changes are candidates; doubled playback and in-game flicker are unverified. |
| Poporogue | Movie image reportedly stopped while audio continued. Partial output DMA is covered by a fixture; the reported movie is unverified. |
| SaGa Frontier | GPU display-disabled remained set while audio ran. The renderer must not override the game's display-disable command. Startup still needs retesting. |
| Threads of Fate | No host crash was logged. Shared polygon rounding has fixture coverage; reported tearing needs visual retesting. |

The other reported dot-artifact games have no equivalent local visual test yet.
Do not describe all of these titles as fixed based only on shared-engine tests.

## Separate older slowdown report

The Moto G75 report used 0.3.21, Vulkan and 3x internal resolution. Its short
logged window averaged about 57.35 guest fields/second, with a 62.1 ms average
GPU queue delay and substantial presentation/acquire waiting. Native CPU work
and compilation were not the dominant measured costs in that window. RSS was
about 281 MiB with about 3.5 GiB system memory available; the report does not
show an out-of-memory condition. The exported thermal status was light, with
no temperature/thermal history proving sustained throttling.

Presentation fixes shipped since 0.3.21 and the new bounded submission path are
relevant to that waiting, but improvement on that user's device is unmeasured.
Fresh logs and a sustained test are needed before claiming reduced heat or a
resolved slowdown. The new thermal sampling makes that follow-up more useful.
