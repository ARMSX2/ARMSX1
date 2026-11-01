# Final Fantasy IX: movie decoder reports ready too early

## Reproduction and cause

On Thor, the USA Rev 1 Disc 1 opening movie showed a single narrow column, then stalled or returned distorted graphics. A scene snapshot reproduced the column in canonical VRAM, independently of the Vulkan window. Fresh-boot tracing reproduced it with the software GPU as well.

The first movie frame decodes 280 macroblocks (1,680 DCT blocks) into 53,760 words of 24-bit output. The player retrieves twenty 16-pixel-wide columns, each using 2,688 DMA words for 224 rows. Our synchronous decoder accepts and transforms the compressed input ahead of output consumption.

`Mdec.status()` previously cleared command-busy when the compressed input count reached zero. It therefore reported ready while nearly the entire decoded frame was still queued. FFIX's DMA completion callback polls that flag to distinguish an intermediate column from a completed frame. Its ready branch switched/reset the current movie rectangle instead of advancing to the next column. The trace consumed output DMA data but only uploaded rectangles at the first column of the two display buffers (x=0 and x=480).

## Engine change

Command-busy now includes decoded work queued beyond the final output macroblock. Its size follows the active output format: 8, 16, 192 or 128 words for 4-bit, 8-bit, 24-bit or 15-bit output. The final macroblock may remain readable after busy clears. Reset still discards pending work.

This is an original change to the shared MDEC model. It applies across CPU execution modes and software/OpenGL/Vulkan presentation. It adds no title checks, game patches, settings, native-library changes or save-state fields. Decode/FIFO cycle timing remains approximate; this fixes the command's observable completion status within the existing synchronous model.

References for the status contract and final output stage:

- [PSX-SPX MDEC status register](https://psx-spx.consoledev.net/macroblockdecodermdec/#1f801824h-mdec1-mdec-status-register-r)
- [DuckStation's MDEC state and output-stage handling](https://github.com/stenzek/duckstation/blob/master/src/core/mdec.cpp) (comparison only; no code copied)

## Regression coverage

Two new tests failed on the old engine and pass with the fix. Across all four output depths, they verify that column callbacks remain busy while later columns need processing, then complete after the last column; and that final-block output stays readable after command-busy clears. The full core suite passes 429 tests.

The same fresh-boot trace after the change uploads all forty column positions across the two 320×224 movie buffers: x=0…456 and x=480…936, stepping by 24 VRAM words (16 RGB pixels). Previously only x=0 and x=480 appeared. A 90-second guest run completed the opening sequence and produced the correct full title screen; 1,241,520 DCT blocks had been decoded. Eight SPU streaming tests and two cross-renderer movie presentation tests also passed on the installed APK.

An already-broken movie snapshot can contain callbacks/buffers corrupted before this fix. Validate from a fresh game boot, rather than expecting such a snapshot to reconstruct missing picture columns.
