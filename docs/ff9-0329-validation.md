# 0.3.29 — Final Fantasy IX movie validation

AYN Thor, 2026-11-01; USA Rev 1 Disc 1, selected SCPH1001 BIOS dump.

## Evidence

- User's original scene: almost black, narrow image strip at the left; canonical VRAM and the save preview contained the same strip.
- Fresh-boot software trace before the fix: decoded the full frame, but only uploaded the first column at x=0 and x=480.
- Identical trace after the fix: uploaded all twenty columns in both buffers (forty x positions, step 24 VRAM words). A 90-second guest run completed the opening and displayed the correct full title screen. No guest instructions or game files were patched.
- Normal ArmSX1 frontend: fresh-launched the user's M3U on the existing Vulkan/HD settings. A device screenshot at 20:18:38 shows the complete moving opening picture, rather than the single strip. This also verifies the actual window path, independently of the software trace.

## Checks

- 429 core + 193 frontend unit tests passed (622 total).
- Both new MDEC tests failed before the fix; they pass across all four output depths afterwards.
- Eleven device checks passed: the fresh-boot FFIX movie trace, eight SPU streaming regressions, and two movie presentation/filter/crop/mode-change regressions across software/OpenGL/Vulkan and HD.
- All nine packaged native libraries match 0.3.28 byte-for-byte. The prior CPU, GPU and presentation improvements are retained.
- Release package/signing identity matched the installed APK; non-debuggable and 16 KiB ZIP-alignment checks passed.

## Artifact and preservation

- Package `com.nanodata.armsx`; version `0.3.29` / `93486625`.
- `ArmSX1-0.3.29-ff9-movie.apk`, 63,284,811 bytes.
- SHA-256 `0931fd479c893d030740a7c183d43c7a4ecd02dba814215bafc6906ecdf9ebd9`.
- Updated Thor's existing release app in place. Existing states and memory cards retained.
- The temporary diagnostic capture used an initially empty FFIX Slot 8. After copying and verifying both files, it was moved outside active slots into `performance/ff9-0329-validation`; the user's pre-existing FIFA state was retained. The broken capture was not auto-loaded for validation.
- Private tests used isolated cards. The launcher-less helper was uninstalled. No other device was used.

This fixes the shared decoder's completion status; it does not make its existing synchronous decode/FIFO timing cycle-exact. See [the cause and model change](ff9-movie-decoder-busy.md).
