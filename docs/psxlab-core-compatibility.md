# PSX Lab media, audio and presentation fixes

The Android host continues to use the PSX Lab CPU, GPU, GTE, CD-ROM and SPU. The
shader runtime only processes completed images. No SwanStation core is packaged.

## Disc loading

`ArmSxBackend.openMedia` recognizes staged CUE files and mounts the referenced BIN
tracks through `CueDisc`, rather than trying to parse the CUE text as ISO sectors.
Track indexes, stored/generated pregaps, leadout and CDDA reach the CD controller.
Binary AUDIO, MODE1/2048, MODE1/2352, MODE2/2048, MODE2/2336 and MODE2/2352 tracks
are supported. Nonbinary audio files and invalid/missing tracks produce an error.
The ISO boot reader still expects the PlayStation data filesystem in track 1.

`PbpDisc` reads unencrypted PS1 PSISOIMG data, raw or deflated 16-sector blocks,
and embedded multi-disc tables. Launch currently selects the first embedded disc;
the reader's indexed constructor supports other discs, but there is no new menu for
switching embedded PBP discs. PSP applications and encrypted PSN containers are
rejected explicitly. PBP verification uses generated compressed, raw, multi-disc
and corrupt fixtures; no supplied commercial PBP was available for a full-game test.

## Audio and widescreen

Fast-forward respects the existing mute choice and fast-forward volume. Audible
fast-forward uses the existing SoundTouch processor with a tempo multiplier while
Android output remains 44.1 kHz. Normal-speed processing remains the same. Uncapped
fast-forward uses a bounded 16x audio tempo; it cannot follow unlimited CPU speed.

Aspect ratio, stretch and integer scaling configure the native software display,
OpenGL display, Vulkan upload path and HD shared-image path. Changing them invalidates
cached static pictures. The widescreen hack adjusts GTE horizontal perspective
projection around its existing centre in Java, native GTE and native CPU paths;
it is independent of the display aspect ratio. Games can still have their own
visibility culling and 2D elements, which this projection adjustment does not rewrite.

## Shared shader presentation

The PS1 launch gate restores the host's resolved global shader settings on every
launch, including cold starts and title changes. Explicit per-game shader choices
remain overrides; "Use global shader settings" clears just those shader overrides.
Both the settings hub and pause menu edit the scope shown by their header.

Settings rows show a small accent dot and "Changed" when their effective value
differs from the factory default. This includes values inherited from global settings;
the existing per-game badges still identify the storage layer. Resetting a value
removes the changed marker. Comparison snapshots are built only for visible menus.

Software, OpenGL and Vulkan game rendering can feed the same Vulkan postprocessing
stage. Software keeps its CPU rasterizer; OpenGL keeps its integer/HD rasterizer.
OpenGL native frames use a shared hardware buffer where the driver supports it,
with a CPU transfer fallback. Shader processing does not emulate another game frame.
With shaders disabled, software/OpenGL return to their ordinary display path and
the optional runtime is not loaded on a fresh launch. A usable Vulkan driver is
required for shaders even when software or OpenGL game rendering is selected.

The bridge adapts this repository's `frontend/render_shaders.cpp`. It loads the
pinned librashader 0.12.0 C API on demand. Chain compilation uses a worker; the
GPU worker owns rendering, parameters and teardown. Queue access is serialized
with compilation. Shader source/lookup textures must be accessible local files.
Unreadable or invalid presets fall back to a plain image and report their status.

HD inputs are downsampled to native game dimensions for the CRT-oriented shader
path, then processed at the aspect-correct display size. This means a general
sharpening shader is not a replacement for HD game rasterization.

Build the optional Android runtime on Windows with:

```powershell
./tools/Build-PsxShaderRuntime.ps1
```

This extracts the vendored C API crate, uses its Cargo.lock and builds only the
Vulkan runtime with the Android NDK and writes shared libraries under
`psxlab/backend/src/main/jniLibs/arm64-v8a`. The pinned ARM64 shader
and matching Android C++ runtime binaries are tracked there so a fresh checkout can
package shaders without first installing Rust. The script regenerates those binaries.
The APK includes notices;
the corresponding librashader source crates are under `third_party/librashader/source`.

## Black-frame insertion and shader subframes

The saved choice is separate from eligibility. At 2x, a 60 Hz game needs a display
near 120 Hz and a 50 Hz game needs one near 100 Hz. Multipliers 3 and 4 require their
corresponding refresh rates. The engine checks the actual display refresh supplied
by the host, not just the requested mode. Settings and diagnostics explain inactivity.

BFI posts real black buffers after each game image on software, OpenGL or Vulkan
presentation. Shader subframes render the same input again with distinct
`TotalSubFrames`/`CurrentSubFrame` values. A subframe-aware preset is needed for a
visible difference. Both features are off by default and suspend during pause and
fast-forward. Pausing or disabling BFI restores the ordinary picture, including
static HD scenes. Guest FPS counts emulated fields, not black or shader presents.

## Verification and logging

Core JVM tests cover CUE layout/ownership, CDDA controller routing, PBP decompression,
GTE projection and refresh eligibility. Android tests exercise media mounting and
original executable execution, file-descriptor cleanup, real-game fast-forward audio,
native CPU/GTE parity, and aspect changes on all three presenters at 1x/3x.

`OffscreenPresentationTest` reads real posted buffers through ImageReader without
flashing the physical display. It checks BFI restoration, shader parameters, distinct
subframe colours, image orientation, renderer handoff, disable and invalid-preset
fallback. Hardware coverage is Thor/Adreno 740, not a guarantee for every driver.

Each launch resets the last CPU/audio/PC diagnostics before opening media, preventing
failed launches from inheriting a previous session's status. Reports include display
geometry, actual refresh, presentation eligibility, shader status and audio tempo/gain.
Frame records label requested presentation counts separately from API acceptance and
include paths/timings for BFI and shader subframes. API completion is not a physical
scanout timestamp or proof that every submitted buffer reached the panel.

# Performance overlay integration (0.3.16)

The original engine now feeds the frontend's 33-slot statistics contract. The
overlay samples frame completion intervals, CPU-worker emulation wall time,
scanout/presentation API wall time, actual pacing sleep, guest clock ticks,
triangle/rectangle/line submissions, active SPU envelopes, MDEC blocks, CD sectors,
DMA words, native/internal video dimensions and process CPU/RSS plus available RAM.
CPU/GPU workers overlap: phase durations are not additive and presentation API time
is not physical GPU execution time. Native instruction counts, GTE operation counts
and hardware GPU busy percentage remain unavailable and are never invented.

Statistics are opt-in and independent of disk logging. Closing the overlay clears
the window; queued samples cannot leak into the next enabled window or session.
Diagnostic-only fields are excluded from save states.

`video.gpu_backend` selects the presentation API, independently of `video.renderer`
and internal scale. Configs with only the legacy renderer key still work. Vulkan
with hardware rasterization reports both Vulkan display and OpenGL compute drawing
because the current hardware rasterizer uses GLES; a requested setting alone is
never presented as proof of an active API. Changes to the presentation API require
restarting the game.

The compact build labels the OSD `Display: Vulkan`, `Display: OpenGL` or
`Display: Software` from the active presenter objects. It keeps the complete
rasterizer/presentation description in diagnostics, including the separate
`displayRenderer` field. Shader presentation and real API fallbacks update this
label; it never substitutes the requested setting for a running backend.

## GPU linked-list DMA recovery (0.3.18)

A Moto G75 Tekken 3 report stopped in `Dma.transfer` with `Cyclic DMA linked list`
after a knockout. Repeated addresses no longer raise a host exception. A detected
cycle retains the next MADR and busy bit, then resumes through the device
scheduler. Each retry re-reads guest RAM, allowing CPU-side repairs or
cancellation. Only an end marker completes the channel and raises its completion
interrupt; a permanently cyclic guest list remains busy. This prevents the
reported fatal path, but does not establish why that particular list was cyclic
or prove the affected fight will recover. The original device must be retested.

Cycle detection retains the existing per-transfer visited-address set. Valid
lists retain their immediate path, including long BIOS ordering tables. Splitting
valid lists into work-budget batches failed a real BIOS regression check and is
not enabled. GPU DREQ/FIFO timing, CPU bus stalls and DMA arbitration remain
approximate. This is not a cycle-accuracy claim. Pending retries survive save states; pre-patch states load
with no pending slice. Diagnostics include DMA2 MADR/CHCR, last header/address,
yield count and resume delay. Rate-limited `dma.gpu.linked.yield` events identify
`Dma.java#transfer/tick`; a yield alone is not proof of a malformed list.

JVM regressions cover self/two-node cycles, CPU RAM edits, native-backed RAM,
atomic completion of long valid lists, terminal-only IRQs, disable/cancel, batched
clock updates, state restoration and old-state defaults. An Android regression
exercises the DMA-start callback and subsequent guest store on native interpreted
and recompiled CPUs, with event scheduling enabled and disabled.

## CPU cache, GPU batching and memory (0.3.19)

Vulkan CPU staging is allocated on the first CPU-pixel upload and grows to fit
the uploaded frame, with geometric reuse bounded by the existing frame capacity.
Shared AHardwareBuffer presentation needs no staging allocation. The previous
eager buffer capacity was 18 MiB at 3x and 50 MiB at 5x; actual process RSS savings
depend on the driver. Software upload, fallback, resizing and native readback keep
their submission-fence ownership. Logs include staging capacity, allocated bytes
and allocation count.

JNI command batches no longer force a compute dispatch at every Java batch end.
The existing 32-primitive limit, texture dependencies, transfers, readback and
scanout remain flush boundaries. Texture/CLUT region scratch storage is a fixed
eight-rectangle array; each wrapped region needs at most four entries. Each
primitive computes its ranges once for feedback and batch dependency checks.

A CPU page generation change now checks the cached block's own instruction words
before recompiling it. An unchanged block retains its decoded/native code and
updates its generation. Changed instructions, included delay slots and DMA writes
still invalidate it. Native diagnostics retain their first eight counters and add
unchanged-block reuse and process-wide mapped JIT bytes. Guest clock budgets,
RAM waits and audio processing are unchanged.

Android regressions check shared staging at 1x/3x/5x, CPU-upload growth/reuse,
native transfer readback, cross-JNI batch ordering and transfer boundaries,
independent raster/HD pixel parity, sibling code writes, actual code changes,
RAM aliases, delay-slot replacement and DMA writes. CPU interpreter/recompiler,
event scheduling and state/PCM continuation are checked separately. The Samsung
A52s report identifies CPU pressure; these checks do not establish a universal
performance gain on that phone or on a Snapdragon 865 without device retesting.


## Texture dumping and replacements (own engine)

Both texture switches now connect to the PSX Lab GPU worker. Disc boot identifiers
provide the automatic `textures/SERIAL/` folder; custom folders remain supported.
Each complete GP0 upload uses XXH3-128 over little-endian words, high half followed
by low half in `vram-write-<32 hex digits>.png`. PNG alpha 0/255/128 preserves
transparency and STP. This is the raw VRAM-write format, not a conversion of
arbitrary decoded-texture or older psxe/FNV packs.

Dumping works with software/hardware drawing. Replacements require hardware
drawing and use a separate enhanced image, including hardware 1x. OpenGL and
Vulkan presentation both see it; CPU/DMA readback and save-state VRAM retain the
original pixels. Protected-mask uploads and self-feedback retain native rendering
rather than applying a conflicting replacement. Indexed uploads retain their
packed-index/CLUT interpretation; RGB24 scanout stays native. Replacement detail
is limited by internal scale.

Limits: 16 MiB decoded cache, 4 megapixels/image, uniform 1x–8x dimensions, 8192
scanned entries, 4096 dumped images/session and four queued PNG writes. Dumping
has an asynchronous worker. First-use decoding runs on the GPU worker and its
cost is logged. Both switches off bypass hashing/PNG/cache work. Empty replacement
folders avoid hashing and activating the enhanced replacement path. Disabling
packs clears caches and frees the extra hardware-1x resources; teardown joins
the writer. Toggle replacements off/on to rescan files added during a session.

Settings status and diagnostic reports expose index size, lookup/hit/write counts,
cache bytes, errors and skips. Generated-image Android tests verify all 65536
VRAM values round-trip through PNG, real OpenGL/Vulkan output changes at 1x/3x,
guest VRAM remains unchanged, repeated toggles restore original output, invalid
dimensions are reported and the writer leaves no session thread behind.


## GameShark cheats

NativeApp's catalogue load/list, selection and armed-count calls now delegate to
a per-backend own-engine cheat service. Startup reads the selected game's
`[cheats]` settings; edits during play publish an immutable selection. Applied
once per emulated frame: 30/80 writes, 50 slides, D0-D3 16-bit and E0-E3 8-bit
conditions. Group boundaries reset condition/slide state. Unsupported code types
remain visible in the list and do not execute. Only physical 2 MiB RAM is patched;
invalid addresses cannot alias another area or touch device registers. Unaligned
halfword writes are handled as two bytes. Unchanged values bypass stores and
cache invalidations; changed code uses the same native RAM and instruction-cache
invalidation as normal Bus writes. The disabled per-frame path returns immediately.
Catalogue memory is bounded by 4 MiB text, 4096 groups and 65536 lines. Names with
commas/escaped quotes survive settings parsing. Each game's boot loads a fresh
catalogue; master-off disarms it. Diagnostics expose armed groups and missing
selection names. This does not implement encrypted codes or arbitrary GameShark
extensions beyond the listed types.
