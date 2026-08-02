# ARMSX — GPU Hardware Rasterizer Design

**Status:** Stage 0 and Stage 1 have LANDED — see §0.5 for exactly what is in the build,
what it does not do yet, and where the next stage starts. The rest of this document is
still the specification the implementation is following.
**Scope:** replace the per-pixel software rasterizer in `psx/dev/gpu.c` with a GPU-side
rasterizer that renders into an upscaled VRAM render target, enabling internal-resolution
upscaling (the DuckStation-class capability).
**Target:** Android arm64 (Adreno / Mali / Xclipse / PowerVR), GLES 3.0+ primary,
Vulkan 1.1 secondary. Desktop keeps the software path.

All line citations are against the tree as read on 2026-07-31:
`psx/dev/gpu.c` (2208 lines), `psx/dev/gpu.h` (203 lines).

---

## 0. Executive summary — read this first

Five things about **this** core make the port materially different from a textbook
`GPU_HW` bring-up. Three of them make it *easier* than usual, two make it harder.

**Easier than expected:**

1. **The live rasterizer surface is tiny.** Despite ~2200 lines, only **three** drawing
   functions are reachable: `gpu_render_triangle` (`gpu.c:251`), `gpu_render_rect`
   (`gpu.c:453`), and `gpu_render_flat_line` (`gpu.c:669`). Roughly 500 lines of
   triangle/rect renderers are **dead code** — see §1.1. A backend only has to
   reproduce three entry points plus four VRAM transfer commands.
2. **A backend seam already exists.** `psx_gpu_renderer_t` (`gpu.h:93-97`) with a
   `render_triangle` function pointer, installed at `gpu.c:72` and dispatched at
   `gpu.c:1155-1164`, under `USE_HARDWARE` (which is already defined globally —
   `Makefile:168-169`). It is currently a no-op indirection: `frontend/gpu_hw.c:64-66`
   just calls straight back into `gpu_render_triangle`. The seam is in the right place
   but is **too narrow** — it has no hooks for rects, lines, fills, copies, or transfers.
3. **A pixel-parity test harness already exists**, `tests/gpu_renderer_parity.c`, which
   instantiates two `psx_gpu_t` and diffs VRAM. Stage 1 acceptance can reuse it verbatim.

**Harder than expected:**

4. **There is no command FIFO and no frame boundary inside the GPU.** `psx_gpu_write32`
   (`gpu.c:1978`) executes each command synchronously and immediately, and VRAM is
   mutated in place at `gpu.c:446`, `gpu.c:586`, `gpu.c:1289`, `gpu.c:1849`,
   `gpu.c:1883`. Nothing batches. A hardware backend *must* batch to be fast, so the
   port has to introduce a flush/coherency layer that does not exist today. The only
   natural frame boundary is the `GPU_EVENT_VBLANK` callback at `gpu.c:2152-2155`.
5. **The software rasterizer is not hardware-accurate**, so "match the software output
   pixel-for-pixel" is *not* the same goal as "be accurate". Notably it always bilinear-
   filters polygon textures (`gpu.c:359`), never implements the mask bit at all
   (`gpu.c:1966` is a literal `/* To-do: Implement mask bit thing */`), dithers only
   Gouraud primitives and with a bounding-box-relative kernel index (`gpu.c:330-331`),
   and ignores the GPUSTAT dither-enable bit entirely. See §1.9 and §7.4. **A decision is
   required up front:** parity-with-software, or correctness. This document recommends
   *parity first, correctness second*, and enumerates every divergence so the second step
   is mechanical.

A note on naming: `[video] gpu_backend` in `frontend/config.c:79,618-639` currently
selects the **present** backend (`software | sdl-accelerated | opengl | vulkan`), not a
rasterizer. It must not be overloaded. See §5.4.

---

## 0.5 Implementation status — what is actually in the build

**Landed (Stage 0 + Stage 1, plus Stage 4's plumbing):**

| Piece | Where |
|---|---|
| Backend ABI, pure C99, no GL types | `psx/dev/gpu_backend.h` |
| Hooks in the core, all NULL-guarded | `psx/dev/gpu.c` (poly / rect / line dispatch, GP0 02/80/A0/C0, vblank, load-state re-seed) |
| Opaque backend pointer on the GPU | `psx/dev/gpu.h` — `struct psx_gpu_backend* backend`, replacing the one-entry `psx_gpu_renderer_t` |
| Internal-resolution rasterizer | `frontend/gpu_hw_rt.c` / `.h` |
| Settings | `[video] renderer` and `[video] internal_scale` in `frontend/config.c` |
| Scanout at scale | `psx_gpu_get_display_surface()` + `frontend/main.cpp` upload path |
| Parity + scale gates | `tests/gpu_renderer_parity.c` (`make test-gpu`) |

**The one deliberate architectural deviation from §2–§4.** The landed backend rasterizes
on the CPU into a `1024S x 512S` BGR555 render target rather than on the GPU, and it sets
`PSX_GPU_BACKEND_SOFTWARE_SHADOW`, which makes the core run the software rasterizer as
well so `gpu->vram` stays authoritative. Consequences, all intentional:

* **§4 (VRAM coherency) is not needed and is not implemented.** No dirty tracking, no
  `vram_read` texture, no readback stall — GP0(C0) costs nothing because host VRAM is
  already correct. This is §4.6 option 3, chosen so a *correct* upscaler could ship
  without first landing the highest-variance part of the port.
* **It costs roughly 2x rasterization CPU at 1x, and more as S grows** (the render-target
  pass is S^2 fill). This is the §7.3 "Stage 1 will be slower" tradeoff, made explicit.
* **It is graphics-API-independent**, so it works with whichever present backend is
  selected — including Vulkan, which is the one that currently works on device.

What this buys the GPU backend that comes next: the ABI, every hook site, the config
plumbing, the scanout-at-scale path, and — most importantly — a *verified* coordinate and
scaling model. `make test-gpu` proves at 1x that the render target is byte-identical to
the software rasterizer, and at 2x/3x/4x that the scaled target is coherent with it. A
GLES/Vulkan backend implements the same vtable and inherits those gates.

**Not implemented yet:** batching (every primitive is drawn immediately), the mask bit
(GP0(E6) is still the stub at `gpu.c:1965`), GPUSTAT bit 9 dither gating, and true-colour
/ texture-filtering enhancements. Semi-transparency, textures (4/8/15bpp + CLUT), the
texture window, sprites and lines are all implemented and gated.

### 0.5.7 CHECKPOINT — seventh session, 2026-07-31 from ~16:40 — **§4.3 ROW 1 BUILT, MEASURED, AND NOT YET CORRECT**

Seventh engineer. Task: re-take §0.5.6's whole-VRAM diff at a busy frame, land §4.3 row 1,
then drop the shadow and prove save states. Nothing in §0.5.4/§0.5.5/§0.5.6 is superseded.

**Status: the shadow is STILL ON, deliberately.** §4.3 row 1 is implemented and fires on
hardware, but arming it moves the 1x parity gate from **0.4708 % to 0.9986 %**, so it is
demonstrably not correct yet and dropping the shadow on top of it would ship a known
regression. Read "state right now" at the end.

#### The headline correction: §0.5.6's 0/524288 is a BOOT number, not a gameplay number

§0.5.6 quoted `0/524288 texels differ … softnonzero=313810` as "the strongest evidence so
far that dropping the shadow is safe at 1x". That measurement is real and it reproduces —
but it was taken at frame 256, during the Naughty Dog logo, at **9.4 primitives per frame**.
It does not survive contact with gameplay.

`hwgl_vram_diff` repeats the whole-VRAM readback every 600 frames, and each line now carries
its own frame number, its own `softnonzero`, its own `gpunonzero` and the same near/far/
blank/extra buckets `gl_parity_check` uses — so a busy sample can be told from a black one
*in the log* instead of by assertion. **27 samples out to frame 16 200** on the final binary,
driven into N. Sanity Beach with the pad (975 tri / 836 K px per frame,
`softnonzero ≈ 425 000`); an earlier 26-sample run out to frame 15 600 on an earlier build
of this session reproduces the same bands, so the numbers below are two independent runs,
not one:

| frames | differ | % | near | far | extra | blank | scene |
|---|---|---|---|---|---|---|---|
| 600–3000 | 0–5 | 0.0000–0.0010 % | 0–5 | 0 | 0 | 0 | BIOS, logos, CD load |
| 2400 (busy) | 1388 | **0.2647 %** | 664 | 628 | 4 | 92 | title, 422 K non-zero |
| 3600–16200 (gameplay) | 965–1390 | **0.1841–0.2651 %** | 929–1186 | 32–196 | 4–6 | 0–2 | level, 425 K non-zero |

So the honest statement is: **at boot the two copies of VRAM are identical; during gameplay
they differ on about 0.19–0.27 % of all 524 288 texels, of which 87–96 % are within one
5-bit step.** That is the same divergence `gl_parity_check` already reports for the display
window (0.4708 % at window 3) — it is the rasterizer's own §2.5 blend-precision and edge
residue showing up in VRAM, not a fault in the readback. The `far` samples repeat exactly
across ten minutes and come in pairs 512 apart — `(386,16)` and `(898,16)`, `(263,19)` and
`(775,19)` — i.e. the *same pixel in the two double-buffered framebuffers*, which is what a
rasterization difference looks like and not what a transfer bug looks like.

This does not block dropping the shadow — the user already sees the GPU's answer on screen —
but it does mean **a state saved under the GL rasterizer and loaded under the software one
will differ on ~0.2 % of VRAM**, and that should be written down rather than discovered.

Run 2's `softnonzero=0` line from §0.5.6 remains vacuous and is still not quotable.

#### What landed — `frontend/gpu_hw_gl.c` only, again

| piece | what it is |
|---|---|
| `gpu_dirty` tilemap + `any_gpu_dirty` | §4.2's second map: regions that exist ONLY in the render target. Maintained unconditionally (a bitmap costs nothing), acted on only under `gpu_own`. |
| `prog_gres` / `kGResFS` | the resolve shader with a **uint** output. `vram_tex` is R16UI, which is colour-renderable in GLES 3.0, so §4.3 row 1 is an FBO and a draw — no `glReadPixels`, no CPU round trip, no stall. Same arithmetic as `kResolveFS`. |
| `vram_fbo` + `gl_ensure_vram_fbo()` | `vram_tex` as a render target, created lazily and refused gracefully: if R16UI is not framebuffer-complete the backend logs it, and with the shadow off walks §4.6's ladder rather than rendering something wrong. |
| `gl_resolve_gpu_tiles()` | the mirror of `gl_sync_vram()`: same tile walk, source is `rt_tex`, destination is `vram_tex`. |
| `gl_mark_drawn()` | one place where a rasterizer write is recorded, routing to `dirty` (shadow) or `gpu_dirty` (`gpu_own`), **clipped to the drawing area**. |
| `gl_upload_rect()` + a 4-rectangle GP0(A0) wrap split | see below; this one is a latent bug fix that matters only after the flip. |
| exact-run tile walks in BOTH `gl_sync_vram()` and `gl_resolve_gpu_tiles()` | see below. |
| `hwgl_gpu_resolve`, `hwgl_vram_diff`, `hwgl_geom`, `hwgl_no_shadow` markers | four things that were untestable are now testable on one binary. |
| `gres/f=`, `grespx/f=` in the stats line; a `coherency:` line at create | so a marker-driven claim has a log line proving it took. |

#### Two latent bugs found on the way, both invisible while the shadow is on

**1. GP0(A0)'s wrap case would have erased the frame.** A transfer that runs off 1024/512
wraps per halfword and cannot be expressed as one rectangle, so the old code re-seeded the
*whole surface* from host VRAM. That is correct only while host VRAM is authoritative;
without the shadow a wrapping 32×32 texture upload would have uploaded 1 MB of stale host
VRAM over the render target and blanked everything the rasterizer had drawn. It now
enumerates the up-to-four rectangles the wrap actually lands in, which is correct in both
worlds and cheaper in each. Not observed to fire in Crash — the 1x parity numbers are
unchanged to the digit — so it is a reasoned fix, not a measured one.

**2. Both tile walks resolved SPANS, not runs.** `gl_sync_vram()` uploaded from the first
set tile in a row to the last, dragging every clean tile in between along. That is free when
the payload is host VRAM (identical data, wasted bandwidth). It is not free when the payload
is a render-target round trip: RGBA8 has no bit 15, so a laundered 16-bit texel loses its
STP flag and a laundered **4bpp texture word loses the high bit of one palette index in
four**. Both walks now emit one operation per contiguous run. Verified inert with the shadow
on: the control run reproduces `0.0049 % / 0.0003 % / 0.4708 %` with every bucket identical
(`blank=84650 extra=3748 near=181608 far=771226 clustered=260258`).

#### §4.3 row 1 — it fires, it is inert when off, and it is WRONG when on

All three measured on device, Retroid Pocket 6 / Adreno 740, Crash Bandicoot (USA), 1x,
`renderer = "hardware-gl"`, no pad input (window 3 is scene-dependent — see the traps).

| run | `gres/f` @3600 | `vramsync/f` @3600 | window 1 | window 2 | **window 3** |
|---|---|---|---|---|---|
| control, `gpu_own=off` | 0.00 | 12.99 | 0.0049 % | 0.0003 % | **0.4708 %** |
| `hwgl_gpu_resolve`, first cut (spans, unclipped) | 12.99 | 0.00 | 0.0049 % | 0.0003 % | **4.9181 %** |
| + exact runs | 11.36 | 0.00 | 0.0049 % | 0.0003 % | **4.9180 %** |
| + drawing-area clip | 0.00 | 0.00 | 0.0049 % | 0.0003 % | **0.9986 %** |
| + GP0(80) `src`-conditional mark | 0.00 | 0.00 | 0.0049 % | 0.0003 % | **0.9986 %** |

Windows 1 and 2 never move. Window 3 does, and the value is bit-stable across three
separate builds and runs (`differing=2208670`, `blank=268640 extra=9552 near=123404
far=1807074 clustered=1047236` every time), so this is a deterministic disagreement and not
noise.

The drawing-area clip is worth 4 percentage points on its own and is independently correct:
a primitive's bounding box is not what it writes, because `gpu.c:322-325` rejects every
pixel outside `[draw_x1,draw_x2] × [draw_y1,draw_y2]` and `gl_range_for()` scissors to
exactly the same rectangle. Over-marking is free when a mark means "re-upload this from host
VRAM" and destructive when it means "launder this through the render target".

#### Where the remaining 0.53 points comes from — narrowed, not solved

`hwgl_geom` dumps the first eight textured samples and the first twelve draws that reach
below row 256, once, at frame 3500. Crash's geometry at that moment:

```
geom: page=(512,256 128x256) depth=1 clut=(512,304) draw=(0,12)-(511,227) disp=(512,0)
      cpudirty=0 gpudirty=0
geom-draw: bbox=(379,252 6x8)  clipped=(379,252 6x-24) drawarea=(0,12)-(511,227)
geom-draw: bbox=(289,320 11x19) clipped=(289,320 11x-92) drawarea=(0,12)-(511,227)
```

* the two framebuffers are at `(0,0)` and `(512,0)`, 512×240; the drawing area is
  `(0,12)-(511,227)`; the 8bpp texture pages are at `(512,256)` and `(640,256)` and the
  CLUT at `(512,304)`. **Textures and framebuffers do not overlap.**
* the draws whose bounding boxes reach rows 246–339 are entirely outside the drawing area —
  the scissor discards them and so does `gpu.c`'s `bc` test. **They write nothing.**
* therefore the control's 12.99 syncs per frame into the texture rows were pure accident of
  over-marking, and by the geometry they should be no-ops.

**They are not no-ops.** Removing them (which is exactly what the correct clip does) costs
0.53 points of parity. So something writes host VRAM in rows 256+ that the backend never
learns about, and those accidental uploads were repairing it. Candidates checked and
eliminated: the software rasterizer does clip (`gpu_render_triangle`, `gpu.c:269`, is the
live path — `gpu_render_flat/shaded/textured_triangle` at `gpu.c:809/865/958` have no clip
but are reached only from `gpu_cmd_28/2c/...`, which `psx_gpu_update_cmd` routes past);
GP0(A0) syncs its own rectangle immediately; GP0(80)'s destination is now marked `gpu_dirty`
only when its *source* was (§4.3 row 4's actual wording — marking unconditionally was worth
none of the 0.53 but is wrong for the bit-15 reason above). **The one candidate not yet
eliminated is `gpu.c:1814`**, a single-texel host VRAM write with no backend hook, no
drawing-area clip and no bounds check. Whoever picks this up: instrument that line first.

#### `emu` before and after — measured, and the "after" is a TIMING-ONLY probe

The economic case for all of §4 is that most of the ~9 ms `emu` phase is the shadow. That
cannot be tested without turning the shadow off, so `hwgl_no_shadow` does exactly that —
`g->base.flags = 0` — behind a marker. It is **not the flip**: §4.3 row 1 is not correct, so
a session run this way may render wrongly and MUST NOT be used to save a state. The timing
is valid regardless, because the same primitives are still submitted to the same backend and
the only thing that stops running is the software rasterizer.

Both arms carry `perf_log`, so they are comparable with each other and with §0.5.5's table
and with nothing else.

Matched on **emulated frame number** through the `[hwgl] frame=N` stats lines, and the
workload measure has to be `prims/f` from the BACKEND — because `[perf]`'s `tri`/`rect`/
`line`/`px` counters are incremented by `PSX_PERF_ADD` inside the **software** rasterizer,
which is precisely what the shadow-off arm stops running. They read **zero** with the shadow
off. §0.5.5's px-band method therefore cannot be used across this particular A/B, and a
first attempt at it produced "0.07 ms" from 264 samples that were all in the px=0 bucket.
Crash Bandicoot (USA), 1x, capped, `perf_log` on both arms, both driven into N. Sanity Beach.

| emulated frame | `prims/f` on | `prims/f` off | `emu` on | `emu` off | delta |
|---|---|---|---|---|---|
| 1200 | 191.0 | 191.0 | 9.22 | 8.75 | −0.47 |
| 1800 | 328.8 | 328.8 | 9.17 | 8.59 | −0.58 |
| 4200 | 961.1 | 945.0 | 11.08 | 7.99 | −3.09 |
| 4800 | 944.8 | 945.0 | 10.89 | 8.02 | −2.87 |
| 5400 | 944.9 | 944.4 | 10.87 | 7.93 | −2.94 |
| 6000 | 943.9 | 944.7 | 10.91 | 7.96 | −2.95 |
| 6600 | 944.5 | 944.4 | 10.89 | 7.97 | −2.92 |
| 7200 | 944.5 | 944.9 | 10.94 | 7.98 | −2.96 |
| 7800 | 944.6 | 944.1 | 10.91 | 8.00 | −2.90 |

Windows 4200–7800 are steady-state gameplay and the two arms agree on `prims/f` to within
0.1 %, so they are rendering the same work. **`emu` goes 10.93 ms → 7.98 ms, −2.95 ms, −27 %.**

**Say it plainly: the expectation that "~8 of ~9 ms is the shadow" does not hold.** At 1x on
an Adreno 740 the software shadow is worth about 2.9 ms of a 10.9 ms `emu` phase — a real
and worthwhile win, and about a third of the size the premise assumed. The other ~8 ms is
CPU work the shadow was never responsible for: the R3000A itself, the DMA/GPU command
FIFO, SPU and CD emulation all live in the same `emu` phase (`frontend/main.cpp`
`publishFrameStats()`), and dropping the shadow does not touch any of them. §7.3's
arithmetic should be revised against this number rather than against the assumption.

Two corroborating details that make the arm believable rather than merely different:
`present` also falls (3.76 → 2.43 ms), consistent with less CPU contention on the same
core; and the 1x parity gate in the shadow-off arm reports `14.1951 % … extra=20931580,
blank=0, near=0, far=0` — every mismatch is "GPU has content, host VRAM has zero", which is
exactly what a gate comparing against a VRAM copy nobody writes any more must say. It is
not a rendering result and must not be read as one; it is proof the shadow really was off.

#### Verified

* `make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` — **13/13 green**, and the gate was
  re-proven able to FAIL on this tree: `+ 1` on `gpu_hw_rt.c:397`'s `xc` makes it **exit 2**;
  restoring from the byte-checked baseline (`md5 8ea4459ff92b241056d5d87b35d335fc`, identical
  to §0.5.6's) returns **exit 0** and "all cases passed".
* Android clang `-Wall -Wextra` on `frontend/gpu_hw_gl.c` — clean, and the command was
  control-tested by appending `#error` and confirming it is actually compiled.
* 1x parity with everything at its shipping default reproduces §0.5.4/§0.5.5/§0.5.6 to the
  digit, twice, on two different builds of this session.
* Every device run verifies the app has focus before and after, that the log contains
  exactly ONE `GL rasterizer up`, that it says `scale=1x`, and that the `.text` extracted
  from the installed APK matches this session's pinned build.

#### Four operational traps this session hit, all of which faked a result first

1. **The device is SHARED.** Another session's `adb install` force-stopped a run mid-flight
   (`ActivityManager: … due to installPackageLI`) and another changed `settings.toml` to
   `renderer = "software"` — which produced a run with no rasterizer at all and no error.
   Every run now pushes its own settings twice (the Compose UI rewrites the file at startup,
   §0.5.5), refuses to proceed unless `GL rasterizer up … scale=1x` is in the log, and
   re-checks the device `.text` at the END. `scratchpad/verify_device.sh`.
2. **`adb install -r` used an INCREMENTAL install**, which keeps streaming after the command
   returns and re-fires `installPackageLI` minutes later, killing the app mid-run. Two runs
   were lost before the ActivityManager log named it. Use `--no-incremental`.
3. **`$SECONDS` is measured from shell start**, so a runner that computes its deadline from
   it after a long setup phase silently produces a short run — a "180 s" run was 50 s.
   Count iterations.
4. **Parity window 3 is SCENE-dependent.** Windows 1 and 2 (frames 600 and 1800) are BIOS
   and logos and reproduce exactly no matter what; window 3 covers 1800–3600 and only
   reproduces 0.4708 % on a run that sends **no pad input**, like §0.5.6's. A run driven
   into gameplay gives 0.2868 % for the same binary — not a regression, a different scene.
   Drive for the VRAM diff; do not drive for the parity gate.

#### Device state left behind

`files/settings.toml` is §0.5.6's exactly — `renderer = "hardware-gl"`, `internal_scale = 1`,
`gpu_backend = "opengl"`, `frame_limit`/`vsync` true, `logging_enabled = true`,
`quiet = false`. **Every marker file removed**, including this session's four new ones
(`hwgl_gpu_resolve`, `hwgl_vram_diff`, `hwgl_geom`, `hwgl_no_shadow`), and the removal was
control-tested: `ls files/logs/` shows `armsx.log hwgl_geom` with one touched and
`armsx.log` alone after it is deleted, so "only the log is there" is real absence and not a
broken command. The installed APK is this session's build, verified by comparing the `.text`
pulled back off the device.

#### Not done, and not attempted

* **The shadow is not dropped.** §4.3 row 1 is measurably wrong; flipping on top of it would
  be shipping a regression the gate already sees.
* **Save states and thumbnails are still not re-proven**, and are still structurally
  unchanged, because `gpu->vram` is still authoritative. They remain a gate on the flip.
  When it happens they need `psx_gpu_save_state` (`gpu.c:2413`) and `thumbnail.c:527` to
  force a full `download_vram` first; no such hook was added this session, deliberately —
  it would have been unexercised code in the core.
* **The remaining `gpu->vram` readers** (`gpu.c:117/126`, `gpu.c:2322`) are unchanged.
* **The `[perf]` workload counters do not survive the flip.** `tri`/`rect`/`line`/`px`/
  `vramwords` are all `PSX_PERF_ADD` calls inside `psx/dev/gpu.c`'s software rasterizer, so
  once the shadow is off the OSD and the perf log will report a workload of zero. That is a
  user-visible regression in the diagnostics and needs the counters moved to the dispatch
  sites (or mirrored from the backend) as part of the flip, not after it.

#### State right now

Tree builds, `make test-gpu` 13/13 with a demonstrated failure mode, 1x parity unchanged at
its shipping defaults, §4.6's ladder untouched and still armed.

Next, in order:

1. **Instrument `gpu.c:1814`** and any other host-only VRAM write, then re-run
   `hwgl_gpu_resolve` and see whether window 3 returns to 0.4708 %. That is the whole of
   what stands between here and the flip.
2. **Then flip** `g->shadow = 0` — the marker already exists, and both sides of every path
   are written.
3. **Then the save-state / thumbnail flush**, and prove it the way the task asked: save a
   state, kill the app, load it, confirm the picture *and* the thumbnail.

### 0.5.6 CHECKPOINT — sixth session, 2026-07-31 from ~16:00 — **§4.6 SAFETY NET FIRST**

Sixth engineer. Task: **§4, drop the software shadow.** Non-negotiable order of work: land
§4.6's automatic downgrade *first*, and prove it fires. This section tracks that; nothing in
§0.5.4 or §0.5.5 is superseded.

**Status: §4.6 LANDED AND PROVEN ON HARDWARE. The shadow is still on** — dropping it is the
next step and §4.3 row 1 is the one piece still missing before it can go. Read "state right
now" at the end of this section.

#### Baseline re-established before touching anything

* `make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` — 13/13 green on the tree as
  inherited.
* Device (`390011f0`, Retroid Pocket 6) online; `files/settings.toml` is exactly §0.5.5's
  "device state left behind" (`renderer = "hardware-gl"`, `internal_scale = 1`,
  `gpu_backend = "opengl"`, `logging_enabled = true`) and `files/logs/` holds only
  `armsx.log`, i.e. **no marker files** — the shipping path is what runs.
* Every file this session may touch was copied to a scratch baseline with `md5` recorded
  first. There is no git here; that copy is the undo.

#### What landed in this step — `frontend/gpu_hw_gl.c` only

| piece | what it is |
|---|---|
| `u_step` on the resolve shader | one shader now serves two jobs: `u_step = 1` is the existing scanout (one output texel per RT texel, **byte-identical to before**), `u_step = S` is a NATIVE-resolution readback that downsamples on the GPU so the transfer is 2 bytes per native texel instead of 2·S². Origin gains `+S/2` in the readback case, which is `+0` at S=1 — so the 1x parity gate keeps measuring the same pixels. |
| `xfer_tex` / `xfer_fbo` | an RG8 target at native VRAM size for those readbacks. Separate from `resolve_tex` on purpose: that one is re-sized to the display region every frame and sharing it would make a GP0(C0) of a texture page thrash the scanout's allocation. |
| `gl_readback_rect()` | **§4.3's stall, implemented.** flush → resolve at `u_step = S` → `glReadPixels` → scatter into native VRAM layout. This is the primitive GP0(C0), the save-state flush and §4.6's seed all need, and it is the piece §4 could not proceed without. |
| `gl_seed_host_vram()` | §4.6's "download `vram_rt` once to seed `gpu->vram`". See the twist below. |
| `gl_downgrade()` | the ladder. Logs loudly, seeds, sets `failed` — which hands over to `main.cpp`'s **existing** `checkRasterizerHealth()`, so no frontend change was needed. |
| 60-frame ring + trigger in `gl_end_frame()` | §4.6 option 1 verbatim: running average of GP0(C0) bytes over the last 60 frames, threshold 256 KB/frame. A ring, not a session total — a game that reads VRAM hard for two seconds during a transition must not strand the session on the CPU, and a running total cannot tell those two apart. |
| `hwgl_c0_trip`, `hwgl_force_downgrade` markers | how it gets tested. See below. |
| `c0avg=`, `reads/f=`, `readpx/f=` in the stats line; `shadow=`, `c0limit=` in the create line | so a marker-driven claim has a log line proving it took (§0.5.5's trap #1). |

`gl_download_vram()` now counts into the per-frame window and, **when the shadow is off**,
performs the real readback. While the shadow is on it stays a no-op beyond the counting,
because `gpu->vram` is already correct. The switch is one field (`g->shadow`, read from
`base.flags` at create) and both sides are already written.

#### The seed does something more useful than seeding

With the shadow still installed, `gpu->vram` is authoritative **and is the more accurate of
the two copies** — it has not been through the render target's 8-bit blend precision (§2.5).
Overwriting it from the GPU would be a downgrade in the literal sense. So `gl_seed_host_vram()`
reads the whole 1024×512 render target into a scratch buffer instead and **diffs it against
the software shadow**, logging the mismatch count.

That turns an otherwise-untestable code path into a **whole-VRAM parity measurement** —
strictly stronger than `gl_parity_check`, which only ever sees the display window — and it is
precisely the number that says whether dropping the shadow is safe. When the shadow goes, the
same read lands in `gpu->vram` and the diff branch is skipped.

#### Why the trigger needs markers at all, and what each one proves

§0.5.3 recorded `c0bytes=96` for a *whole session*, so the real threshold can never fire on
this game: the fallback would ship having never once executed. Two markers, because they prove
different halves:

* **`hwgl_c0_trip`** sets the limit to 0, so **any** GP0(C0) traffic inside the 60-frame
  window trips it. This fires from **real game traffic**, which is what proves the counter and
  the window are wired to the emulated GPU and not merely to each other.
* **`hwgl_force_downgrade`** trips unconditionally at frame 900 regardless of GP0(C0). This is
  deterministic, so it proves the **ladder** — seed, teardown, replacement rasterizer, picture
  keeps moving — in a game that reads no VRAM at all.

The fallback destination is `main.cpp:2653`'s existing choice, the **CPU internal-resolution
rasterizer**, not the plain software one §4.6's text names. That is deliberate and better: the
CPU RT rasterizer keeps `PSX_GPU_BACKEND_SOFTWARE_SHADOW`, so it is immune to this failure mode
*by construction* (it never reads VRAM back at all) while preserving the user's `internal_scale`.
Plain software remains the second rung if it declines.

#### PROVEN ON HARDWARE — it fires both ways, and it does not fire when it must not

Retroid Pocket 6 / Adreno 740, Crash Bandicoot (USA), `renderer = "hardware-gl"`,
`internal_scale = 1`, `gpu_backend = "opengl"`. The installed APK was repacked per §0.5.3 and
its `.text` **pulled back off the device** matches `bin/libarmsx.so` (sha256
`0b03d318c3b4522f…`), so every line below came from this source.

**Run 1 — `hwgl_c0_trip`, i.e. fired by REAL game traffic:**

```
[hwgl] GL rasterizer up: ... scale=1x, zerocopy=armed, shadow=on, c0limit=0/frame (hwgl_c0_trip)
[hwgl] AUTOMATIC DOWNGRADE at frame 256: GP0(C0) readback traffic over the 60-frame limit.
       GP0(C0) total=96 bytes, window=96 bytes over 60 frames (limit 0 bytes/frame).
[hwgl] whole-VRAM readback vs software shadow: 0/524288 texels differ (0.0000%),
       softnonzero=313810.
[psx] error main.cpp:2646 GLES rasterizer disabled itself: automatic downgrade (...)
[psx] info  main.cpp:2664 Fell back to the software rasterizer.
```

Those 96 bytes are the *same* 96 bytes §0.5.3 recorded for a whole session, so the trigger is
wired to the emulated GPU and not merely to itself.

**Run 2 — `hwgl_force_downgrade`, deterministic, no GP0(C0) involvement:** fires at frame 900
with `window=0 bytes over 60 frames (limit 262144 bytes/frame)` and walks the same ladder.

**Run 3 — no markers, the control:** `c0limit=262144/frame`, `c0avg=0` in **every** stats
window out to frame 5400, and **no `AUTOMATIC DOWNGRADE` and no `Fell back to` line at all**.
The real rule does not fire on a normal session. Both directions control-tested — which is the
only reason run 1 means anything.

**The picture survives it.** Screenshotted immediately after run 1's downgrade: the Naughty Dog
logo rendering normally at 59.3 fps / 100 % speed. The adopted scanout texture is released, the
backend destroyed, and the software rasterizer takes over with no visible seam.

Note the rung actually taken: `main.cpp:2653` skips the CPU internal-resolution rasterizer when
`rasterizer_mode_ == 3` (an *explicit* `hardware-gl` request, which is what this device is set
to), so it lands on the plain **software** rasterizer — exactly §4.6's own wording. The CPU
internal-res rung is only reached from `renderer = "hardware"` / `"hardware-cpu"`. Both are
immune to the failure mode; only the first preserves upscaling.

#### The bonus result, and it is the big one for §4

```
whole-VRAM readback vs software shadow: 0/524288 texels differ (0.0000%), softnonzero=313810
```

**Every one of 524,288 VRAM texels read back off the GPU matched the software shadow exactly,
with 313,810 of them non-zero.** Not the display window — *all of VRAM*, including texture
pages and off-screen buffers `gl_parity_check` has never been able to see. That is the
strongest evidence so far that dropping the shadow is safe at 1x, and it says the new
`gl_readback_rect()` is correct end to end (resolve → downsample → `glReadPixels` → scatter).

Run 2's copy of the same line is **vacuous** and must not be quoted: it landed during a black
CD-load with `softnonzero=0`, so it compared zeros to zeros. Re-take this at a busy frame.

#### 1x parity re-measured after the change — unchanged to the digit

The resolve shader was modified (`u_step`), which is exactly the kind of change that could move
the scanout silently. It did not. All three windows reproduce §0.5.4/§0.5.5 **exactly**:

| window | differing | blank | extra | near | far | clustered |
|---|---|---|---|---|---|---|
| 600 | 6110 (**0.0049 %**) | 0 | 0 | 6110 | 0 | 255 |
| 1800 | 496 (**0.0003 %**) | 0 | 0 | 70 | 426 | 8 |
| 3600 | 1041232 (**0.4708 %**) | 84650 | 3748 | 181608 | 771226 | 260258 |

Same `blank=0/0/84650`, same `extra=0/0/3748`, same `far=0/426/771226`, same `clustered=255` in
window 1, same `far` sample coordinates. Batching unchanged too: frame 3000 reads
`draws/f=3.7 ranges/f=2.8 prims/f=542.9` against §0.5.5's `3.7 / 2.8 / 542.9`, and
`reads/f=0.00 readpx/f=0` throughout — the new readback path costs nothing when unused.

#### Verified

* `make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` — **13/13 green**.
* Android clang `-Wall -Wextra` on `frontend/gpu_hw_gl.c` — clean.
* New strings PRESENT in `bin/libarmsx.so` (`AUTOMATIC DOWNGRADE`, `whole-VRAM readback vs
  software shadow`, `hwgl_c0_trip`, `hwgl_force_downgrade`, `c0limit=`, `reads/f=`) with a
  negative control (`THIS_STRING_NEVER_EXISTED_CONTROL`) reading ABSENT — §0.5.3's lesson.
* `libvulkan` absent from `NEEDED` (`llvm-readelf -d`).
* Every run checks app focus before *and* after and aborts rather than reporting numbers if it
  lost it — §0.5.5's lesson. Runner: `scratchpad/run.sh <label> <secs> [markers…]`.
* **The injected-bug negative re-confirmed on this tree, both directions.** `+ 1` on
  `gpu_hw_rt.c:397`'s texel counter (`xc`) makes `make test-gpu` **exit 2**, stopping at
  `hw-2x-blocks-uniform` after 10 passes; restoring from a byte-checked copy (`md5`
  `8ea4459ff92b241056d5d87b35d335fc`, identical to the pre-session baseline) returns it to
  13/13 "all cases passed". The suite can still fail.
* The final binary was re-run on device after the last edit and reproduces the downgrade
  verbatim (`AUTOMATIC DOWNGRADE at frame 256`, `0/524288 texels differ`), and its `.text`
  pulled off the device matches `bin/libarmsx.so` (`708dc6c21a55327f…`).

#### Device state left behind

`files/settings.toml` unchanged from §0.5.5 (`renderer = "hardware-gl"`, `internal_scale = 1`,
`gpu_backend = "opengl"`, `frame_limit`/`vsync` true, `logging_enabled = true`,
`quiet = false`). **Every marker file removed** — `hwgl_c0_trip`, `hwgl_force_downgrade`,
`hwgl_no_adopt`, `hwgl_no_bbox`, `hwgl_no_defer`, `hwgl_dump`, `perf_log` — so shipping
behaviour is what runs. The installed APK is this build.

#### Not re-proven this step, deliberately

**Save states and thumbnails.** Nothing here touches `psx_gpu_save_state`, `psx/state.c` or
`psx/thumbnail.c`, and with the shadow on `gpu->vram` is authoritative, so they are
structurally unchanged. They become load-bearing the moment the shadow drops —
`psx_gpu_save_state` (`gpu.c:2413`) serialises `gpu->vram` directly and `thumbnail.c:527` reads
it for the whole-VRAM and 24bpp cases — so proving them is a **gate on that step**, not this one.

#### State right now

**§4.6 is landed and proven.** Tree builds, tests 13/13, 1x parity unchanged, downgrade fires
from real traffic and from a forced trigger, and does not fire when it should not.

Next, in order:

1. **Re-take the whole-VRAM diff at a busy frame** (run 2's was vacuous). A
   diff-without-downgrade marker is a few lines on top of `gl_seed_host_vram()` and it is the
   go/no-go number for the shadow.
2. **§4.3 row 1 — the one genuinely missing piece before the shadow can go.** A textured draw
   that samples a region the GPU has drawn into: today `gl_sync_vram()` uploads from host VRAM,
   which is correct *only because* the shadow keeps it current. Without the shadow this needs a
   `gpu_dirty` tilemap and a GPU→GPU resolve of those tiles into `vram_tex` — which is
   renderable as R16UI, so it is an FBO plus a uint-output variant of the resolve shader, with
   no CPU round trip. `tiles_mark`/`tiles_intersects` already exist and are reusable verbatim.
3. **Then the shadow itself**: `g->shadow = 0` (and drop the flag in `base.flags`), which also
   switches `gl_download_vram()` onto the real readback already written and
   `gl_seed_host_vram()` onto writing `gpu->vram`.
4. **Then every remaining `gpu->vram` reader**: `gpu.c:117,126` (the GPUREAD drain — already
   covered by the GP0(C0) hook), `gpu.c:2322` (display buffer / debug view), `gpu.c:2413` (save
   state), `thumbnail.c:527`. Note `gpu_fetch_texel` (`gpu.c:202-220`) and every software
   rasterizer write do **not** need handling: with the shadow off, `GPU_BACKEND_SHADOWS()` is
   false and none of them run.

### 0.5.5 CHECKPOINT — fifth session, 2026-07-31 from ~14:30 — **THE SEAM IS WIRED**

Fifth engineer. §0.5.3's brokered seam now has both halves. **Nothing in §0.5.4 is
superseded** — 1x parity, the marker files and the diagnostic apparatus are all unchanged and
still work; this section only adds the scanout path and what it measured.

#### What landed, file by file

| file | change |
|---|---|
| `frontend/gpu_hw_gl.c` | `gl_display_buffer()` split into `gl_scanout_resolve()` (resolve, leaves the FBO bound) + `gl_scanout_read()` (the `glReadPixels`). New `armsx_hw_gl_present_texture()` does resolve → adopt and never reads back. New `hwgl_no_adopt` marker. `zerocopy=` in the create line and in the 600-frame stats line. |
| `frontend/gpu_hw_gl.h` | declares `armsx_hw_gl_present_texture(psx_gpu_backend_t*, struct armsx_renderer*)` — spelled as the struct so the header still does not pull in SDL — plus the no-op stub for non-Android builds. |
| `frontend/main.cpp` | `updateTexture()` tries the seam first and returns early when it takes; `releaseAdoptedTexture()` drops the adoption before the backend is destroyed, from both `destroy()` and `checkRasterizerHealth()`. |

`frontend/render*.cpp/.h` were **not touched** — the renderer half was already applied exactly
as §0.5.3 specified it, and it works as specified.

#### The refusals are the interesting part

`armsx_hw_gl_present_texture()` returns 0 — meaning "keep using `upload_frame()`" — on:

* `owns_context`. When the present backend is **not** OpenGL this rasterizer makes its own
  EGL context, so `resolve_tex` is a name in *our* namespace. Handing it to the presenter
  would name a different object or nothing at all. **The present layer cannot detect this;
  only the rasterizer knows.** This is the single most important guard in the change.
* `gpustat` bit 23 (display disabled) — `gpu.c:2351` hands back native VRAM in that case, so
  adopting would leave a stale frame on screen.
* a sticky `adopt_disabled`, set the first time the present layer refuses, so a Vulkan or SDL
  present path costs **one** wasted resolve for the whole run rather than one per frame.

`main.cpp` adds `!debug_view_` and the existing `want_native_scanout` (24bpp playback and the
whole-VRAM source, §4.4), and requires `armsx_renderer_backend(render_) ==
ARMSX_RENDER_BACKEND_OPENGL` before it even asks.

#### The parity gate survives the seam, deliberately

§0.5.4 warned that `gl_parity_check` reads `g->readback`, which only exists because of the
readback the seam deletes. Resolution: **at `scale == 1`, while the gate is still running, the
readback is kept** and parity is checked exactly as before. The gate stops itself at frame
3600, and from that frame on 1x is zero-copy too. So:

* 1x parity is measured on the *new* path, not assumed — the thing the non-negotiable asks for;
* the cost is only paid where it was already being paid, and it self-terminates;
* 2x and above never ran the gate anyway (`gl_parity_check` is `scale == 1` only), so the
  upscaled path — the one this whole change exists for — is zero-copy from frame 1.

A perf run at 1x must therefore be sampled **after frame 3600**, or it measures the parity
gate rather than the seam. Watch for `1x parity frame=3600` in the log.

#### A measurement tool, because the numbers were only ever on screen

`touch files/logs/perf_log` (same marker mechanism, read once on first use) makes the frontend
arm the performance overlay **from native** and write its own snapshot to the diag log every
half-second window:

```
[perf] fps=59.3 frame=16.87 emu=8.72 present=3.21 idle=4.94 worst=18.39
       tri=307 rect=264 line=0 px=147210 vramwords=55296 disp=512x240 frames=30
```

Those are the same numbers the OSD shows — `frontend/main.cpp` `publishFrameStats()` — but as
text, in order, with the primitive counters beside them. §0.5.3's rule that two runs may only
be compared when their `tri`/`px` counters match stops being an eyeball judgement about a
screenshot and becomes an arithmetic one. Two things to know:

* arming the stats from here also makes the Compose OSD appear, since it renders whatever the
  native side publishes. That is a constant across an A/B — every run in the table below had
  it — but it is not free, so these numbers are not comparable with a run that had the overlay off.
* the marker is read once, lazily, so it must exist before the app starts, like the others.

#### Measured: A/B on one binary, matched on emulated FRAME NUMBER

Both arms are the same `.so`; `hwgl_no_adopt` is the only difference. Retroid Pocket 6 /
Adreno 740, Crash Bandicoot (USA), capped (`frame_limit = true`, `vsync = true`), 105 s per
run, `[perf]` samples aligned to emulated frame number through the `[hwgl] frame=N` stats
lines rather than to wall clock. **`emu` is the number that matters** — `updateTexture()`, and
therefore the resolve, the readback and the upload, all run inside that phase (§0.5.3).

The scene match is not a judgement call: over each window the two arms agree on `tri` exactly
and on `px` to within 0.1%, and their three parity windows are identical to the digit, so the
two runs rendered *the same pixels*.

**1x**, where the parity gate keeps the readback until frame 3600 — which makes the first
window a **null control**:

| emulated frames | seam | `emu` | `present` | `idle` | scene |
|---|---|---|---|---|---|
| 600–3600 (gate still reading back in BOTH) | on | 8.75 | 2.66 | 5.63 | 229 tri / 150.1K px |
| 600–3600 | off | 8.78 | 2.84 | 5.41 | 229 tri / 150.1K px |
| **3600–6000 (seam actually free)** | **on** | **9.67** | 3.27 | 3.93 | 547 tri / 344.3K px |
| 3600–6000 | off | 10.29 | 3.37 | 3.21 | 547 tri / 343.8K px |

**−0.62 ms of `emu` per frame at 1x (−6.0%), and −0.03 ms where the mechanism says there
should be none.** The difference appears exactly where the readback is removed and vanishes
where it is not — which is worth more than the size of either number.

A second, stronger reading of the same logs, because a frame number interpolated between
600-frame marks can be ±60 frames out and that is a whole scene transition: bucket every
sample by its own `px` counter and compare arms **band by band**, so a sample is only ever
compared against samples doing the same amount of work (`scratchpad/zcan3.py`). Mean `emu`
per band, on − off:

| px band | 1x | 2x |
|---|---|---|
| 0–50K | −0.12 | −0.67 |
| 200–250K | −0.32 | −0.86 |
| 250–300K | −0.59 | −0.98 |
| 300–350K | −0.64 | −1.46 |
| 700–750K | −0.07 | −0.95 |
| **weighted** | **−0.24** | **−0.76** |

1x is diluted by construction — two thirds of that run is the parity gate, where both arms
read back — and its per-band figures rise to −0.6 in the late bands, matching the windowed
table. 2x is negative in **every** band, from −0.67 to −1.46 ms.

#### 3x, capped and uncapped — the case the seam exists for

3x, capped, workload-matched (`emu`, on − off):

| px band | `emu` on | `emu` off | delta |
|---|---|---|---|
| 0–50K | 6.54 | 7.02 | −0.48 |
| 50–100K | 8.29 | 8.99 | −0.70 |
| 100–150K | 8.58 | 9.55 | −0.96 |
| 200–250K | 9.11 | 10.57 | −1.46 |
| **700–750K (heavy fill)** | **9.87** | **12.42** | **−2.56** |
| **weighted** | **8.20** | **9.23** | **−1.03** |

3x, **uncapped** (`frame_limit = false`, `vsync = false`), same bucketing — here `fps` is the
number that means something, and it is measured per band so the scene drift §0.5.3 warns about
cannot flatter either arm:

| px band | fps on | fps off | gain | `emu` delta |
|---|---|---|---|---|
| 0–50K | 139.5 | 125.9 | +10.8% | −0.63 |
| 100–150K | 113.3 | 100.5 | +12.7% | −1.01 |
| 150–200K | 106.8 | 94.7 | +12.8% | −1.11 |
| **200–250K** | **104.9** | **79.2** | **+32.4%** | −1.35 |
| 300–350K | 96.9 | 81.5 | +18.9% | −1.79 |
| 400–450K | 94.0 | 80.7 | +16.5% | −1.37 |
| weighted `emu` | 9.02 | 10.09 | | **−1.07** |

**The saving grows with the scale exactly as the S² argument says it should** — weighted
`emu`: −0.24 (1x, diluted) / −0.76 (2x) / −1.03 (3x capped) / −1.07 (3x uncapped) — and it
grows *within* a scale with the size of the frame, peaking at −2.56 ms in the heaviest band
sampled. Uncapped 3x throughput is **7–32% higher at matched workload**, and every band sits
above 60 fps in both arms (79–140 fps), so on these scenes 3x is not the 51 fps §0.5.3
recorded. That figure came from a 1.36M-px fill scene which these runs did not reach; the
honest claim is the one the data supports — the seam takes 1–2.6 ms off every 3x frame, and
the more the frame costs the more it takes off.

#### The refusal path is verified on hardware, not just reasoned about

`renderer = "hardware-gl"` with `gpu_backend = "vulkan"` — the case where the rasterizer must
build its own EGL context and the resolve texture means nothing to the presenter:

```
[hwgl] GL rasterizer up: system GLES, own EGL context, scale=2x, target=2048x1024,
       max_texture=16384, zerocopy=unavailable (own context)
[hwgl] frame=600 ... zerocopy=0/600
[renderer] Selected presentation backend=Vulkan (experimental) driver=Adreno (TM) 740
```

Zero `zero-copy scanout` lines, the session runs normally at 59.3 fps through the readback,
and the picture is unchanged. **The guard fires, and it falls back to something that works.**

#### Verified

* **1x parity re-measured after the seam, and it is unchanged to the digit.** All three
  windows reproduce §0.5.4's post-fix numbers *exactly* — `0.0049% / 0.0003% / 0.4708%`,
  `blank=0/0/84650`, `extra=0/0/3748`, `far=0/426/771226`, even `clustered=255` in window 1
  and the same `far` sample coordinates. A texture that bypasses the readback did not move a
  single pixel. (The gate still runs on the readback at 1x by design — see above — so what
  this proves is that the resolve→adopt path presents exactly what the resolve→read path did.)
* Batching is unchanged: frame 3000 reads `draws/f=3.7 ranges/f=2.8 prims/f=542.9` against
  §0.5.3's `3.6 / 2.7 / 543.2`.
* `zerocopy=600/600` from frame 600 onward — every scanout is adopted, none falls back.
* `make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` — **13/13 green**.
* **Injected-bug negative re-confirmed on this tree**, not assumed: `+ 1` on `gpu_hw_rt.c`'s
  texel counter (`xc`) made `hw-2x/3x/4x-blocks-uniform` fail with
  `block-not-uniform at native (120,160) sub (0,0) got=2bef want=74cf`; the file was restored
  from a byte-checked copy (`md5` identical) and the suite went green again.
* Android build green; `zero-copy scanout`, `present layer will not adopt` and `zerocopy=%u/%d`
  all PRESENT in `bin/libarmsx.so`, with **both controls** (§0.5.3's lesson): a string that
  never existed reads ABSENT, a string that always existed reads PRESENT.
* The APK **pulled back off the device** has a `.text` section with the same sha256 as the
  local `bin/libarmsx.so` (`09cd640c…`), so every number below was produced by this source.
* The A/B switch is control-tested in both directions, not assumed: the `off` runs log
  `zerocopy=off (hwgl_no_adopt)`, `zerocopy=0/600` in every stats window and **zero**
  `zero-copy scanout` lines; the `on` runs log `zerocopy=armed` and `600/600`.
* Launch verified from logcat (`ARMSX_LAUNCH_PS1`) on every run, and a run that could not
  launch **aborted instead of reporting numbers** — which happened once, because the device
  was on its lock screen and the taps were going to the keyguard. The runner now dismisses it
  and refuses to proceed unless the app actually has focus.

#### Two operational things this session added or hit

* **`log_level` in `settings.toml` is written but never read.** `frontend/config.c` has the key
  in its template (`:66`) and a default (`:436`), and `frontend/main.cpp:4611` applies
  `settings_.log_level` — but nothing ever parses it out of the `[runtime]` table, so only the
  `-L` CLI flag can change it. With `logging_enabled = true` the app therefore always logs at
  LOG_INFO: **89,000 lines in a 105 s run (~850/s)**, mostly `cpu.c:1054` and `dma.c:62/128`
  register traces, each an `fprintf` into the app-private diag file **on the emulation
  thread**. It is identical in both arms of every A/B here so it cannot bias a comparison, but
  it inflates every absolute `emu` number in this document and in §0.5.3/§0.5.4.
* **The keyguard eats taps and the run then measures nothing.** `mCurrentFocus` reading
  `NotificationShade` is what a locked screen looks like from `dumpsys window`;
  `input keyevent KEYCODE_BUTTON_A` dismisses it on this device.
* **A run can be interrupted by an unrelated app and it looks exactly like a short run.** One
  3x run came back with 9 `[perf]` samples and no `[hwgl]` lines at all because another app
  (`com.roblox.client`) took the foreground mid-run, which suspends presentation and stops the
  session stepping. Averaging that against a full run would have invented a number. Every log
  now goes through `scratchpad/valid.sh`, which requires the `GL rasterizer up` line with the
  expected `zerocopy=` state, ≥120 perf samples, and an `[hwgl] frame=N` line proving the run
  reached the frames it claims. **A run that fails the gate is re-run, never averaged.**
* **`settings.toml` pushed before launch does not always survive to the core.** Three runs came
  back with no `[hwgl]` lines at all and no diag file, and the file on device then read
  `renderer = "software"`, `internal_scale = 2` — values nobody pushed. The Compose UI writes
  `settings.toml` from its own prefs during startup and can land after the push. The runner
  (`scratchpad/zc2.sh`) now pushes **twice** — once before `monkey`, once ~5 s after, when the
  UI has already had its turn and before the core reads the file at game launch — and then
  *verifies from the log* that the rasterizer that actually came up has the requested `scale=`
  and `zerocopy=` state before it is willing to wait out the run. It retries up to three times;
  in practice one retry was sometimes needed and every reported run passed the check.
  ⚠ **This means a settings-driven claim about this app is only as good as a log line proving
  the setting took.** Do not trust the pushed file.

#### Device state left behind

`files/settings.toml`: `renderer = "hardware-gl"`, `internal_scale = 1`,
`gpu_backend = "opengl"` (the seam needs the GL present backend), `frame_limit = true`,
`vsync = true`, `logging_enabled = true`, `quiet = false`. **Every marker file removed** —
`hwgl_no_adopt`, `hwgl_no_bbox`, `hwgl_no_defer`, `hwgl_dump`, `perf_log` — so the shipping
behaviour is what runs. The installed APK is this build (repacked per §0.5.3, `.text` sha256
verified against `bin/libarmsx.so` from the copy pulled *off the device*).

#### Priority 2 (batching): measured, and there is nothing to do

The brief said verify before optimising. Verified, from the stats lines of the runs above:

| frame | prims/f | draws/f | prims per draw |
|---|---|---|---|
| 3000 | 542.9 | 3.7 | 147 |
| 3600 | 885.1 | 5.2 | 170 |

Identical to §0.5.3's `3.6 / 543.2`, i.e. **the seam changed nothing about batching, and
batching is not the bottleneck.** `vramsync/f` peaks at 11.4 uploads of 9989 px/frame (≈20 KB)
at the busiest sampled frame, which is also not a bottleneck. Optimising either would be work
against the wrong term.

**What the remaining `emu` actually is**: the software shadow. `PSX_GPU_BACKEND_SOFTWARE_SHADOW`
makes the core run its *whole* software rasterizer alongside the GPU one, so every frame is
rasterized twice — once on the CPU for `gpu->vram`, once on the GPU for the picture. That is a
constant ~8 ms of the ~9 ms `emu` here and it does not shrink with anything done on the GPU
side. **Dropping the shadow is the next order-of-magnitude item, and it is exactly §4.**

#### Next steps, in the order the evidence supports

1. **§4 for real: drop the software shadow.** It is now the dominant cost, and everything the
   seam bought (both S² scanout terms) is already banked. The prerequisites §4.6 lists — the
   automatic downgrade — must land first, and note again that with the shadow in place the
   trigger *cannot* fire (`c0bytes=96` for a whole session, still, in every run above), so it
   has to be tested deliberately: force a game that issues GP0(C0), or synthesise the traffic.
2. **The S > 1 sample-point mismatch §0.5.4 left open** (shader samples `X/S − 0.5/S`,
   `gpu_hw_rt.c` samples `X/S`). Still open, still a one-line change, still unverifiable by
   `make test-gpu` because that harness cannot reach a GL context. Worth doing next to a
   screenshot diff at 2x.
3. **Programmable blending** for window 3's last 0.47% (§2.5, driver-gated). A feature, not a
   bug fix, and unchanged by this session.

### 0.5.4 CHECKPOINT — fourth session, 2026-07-31 from ~12:20 — **1x PARITY FIXED**

Fourth engineer. **This section supersedes §0.5.3's "still open" list.** §0.5.3 remains
accurate about the device, the build loop and the performance numbers; only its parity
diagnosis is now closed.

⚠ One correction to §0.5.3 worth making before anything else: its `(447,240)`
`gpu=(22,22,22)` vs `soft=(22,0,0)` "channel replication" lead, called there "the single
most specific clue available", was a **red herring**. That pixel is not a modulation bug; it
is a pixel where the GPU never generated a fragment at all and an older primitive's colour
survived, and the red channel matching was coincidence. Chasing a single dramatic sample
cost time; what actually solved it was the *bucket structure* (`blank` + `far` = 96%, both
explained by one coverage defect) and, later, a rendered difference image.

#### The result, first

Crash Bandicoot (USA), 1x, three windows, **each measured before and after on the same
binary** (the pre-fix behaviour is reproducible at runtime — see the marker files below):

| window | frames | prims/f | BEFORE | AFTER | blank | extra | far |
|---|---|---|---|---|---|---|---|
| 1 boot/BIOS | 1-600 | 9.4 | 0.2129% | **0.0049%** | 169380 → **0** | 758 → **0** | 86096 → **0** |
| 2 title | 601-1800 | ~146 | 0.2981% | **0.0003%** | 537566 → **0** | 21471 → **0** | 23366 → **426** |
| 3 attract/gameplay | 1801-3600 | 486-885 | 4.4936% | **0.4708%** | 727224 → 84650 | 797795 → **3748** | 8214042 → 771226 |

The window-1 "before" row reproduces §0.5.3 **byte for byte** — same
`differing=265522 blank=169380 extra=758 near=9288 far=86096`, same first blank pixel
`(637,2)=0421`, same first far pixel `(447,240) 5ad6/0016`. That is the control: the runs
are deterministic, the scenes are matched, and the comparison is valid.

Windows 1 and 2 are **clean** — `blank`, `extra` and `far` are zero or negligible, and the
only bucket left is `near` (≤ 1 step in 5-bit space). Window 3 is 9.5x better but **not
zero**; what remains is characterised below and is sub-step, not structural: on the dumped
frame 3000, `extra` is **0**, and **92% of all remaining mismatches are within 2 five-bit
steps** (2/32 of a channel), against 48% before.

Three fixes landed, and their contributions were measured separately, not assumed.

#### Fix 1 — the GPU and the software rasterizer were sampling different points

`gpu_render_triangle` decides coverage at the **integer corner** `(x, y)`
(`gpu.c:331-332`). A GPU decides it at the **fragment centre** `(x+0.5, y+0.5)`. Those two
tests do not select the same pixels, and `gpu_hw_gl.c` submitted the triangle itself and
relied on the hardware to generate a fragment for every pixel the shader would then judge
for itself.

That is a one-way trap. **The shader can discard a fragment the hardware produced; it can
never invent one the hardware declined to produce.** Every pixel whose corner is inside the
triangle while its centre is outside was a pixel the software wrote and the GPU left alone
— `blank` where nothing had ever drawn there, `far` where an older primitive's colour
survived. Two buckets, 96% of the mismatches, one defect.

This is why the file header's claim that "coverage matches by construction instead of by
trusting the GPU's fill rule to agree" was not true of the code: the shader did implement
the whole test, but only over the fragments the fill rule had already chosen.

**Fixed in `gl_emit_tri()` by submitting the bounding box instead of the triangle** — six
vertices spanning native `[xmin, xmax) x [ymin, ymax)`, which at scale S generates exactly
the render-target pixels `gpu.c`'s (and `gpu_hw_rt.c`'s) loop visits. The real triangle
still travels in `tri0`/`tri1` as flat attributes, so **the fragment shader is unchanged**;
only the geometry that generates fragments differs. Coverage is now 100% shader-decided.

**Why not just shift the triangle by half a pixel.** It lines the sample grids up, but it
makes correctness depend on GL's fill rule agreeing with `gpu.c`'s `TL()` at samples
landing exactly on an edge — and with integer vertices shifted to `n+0.5`, samples land
exactly on edges constantly. They do not agree: the draw VS maps native `y=0` to the
**bottom** of the framebuffer, so GL's "top" edge is `gpu.c`'s bottom one and the two
top-left rules are mirrored on every horizontal edge. Expanding the triangle outward from
its centroid instead is not a superset either — it does not widen a sliver across its thin
axis, and slivers are exactly where PS1 geometry lives.

**The cost is fragments discarded outside the triangle, ~2x the triangle's own area**, and
§0.5.3's measurements say that is free: 1x and 2x both ran at ~153% uncapped with `emu`
~10.8 ms, i.e. *quadrupling* the fragment count cost nothing measurable, so doubling it at
1x costs nothing. Narrowing it back down means offsetting the three edge lines outward and
intersecting them — exact for fat triangles, numerically nasty for slivers. That is an
optimisation, not a fix, and it is not needed to ship.

This fix is responsible for **100% of the window-1 and window-2 improvement** and most of
window 3's.

#### Fix 2 — a dirty tile could be cleared before it was written

`gl_write_hazard()` marked the destination dirty **before** `gl_note_sample()` ran. A
primitive sampling a texture page that overlaps its own destination therefore hit
`dirty ∩ page`, synced that region out of host VRAM — which does **not** yet contain this
primitive, the software shadow runs *after* the backend hook returns (`gpu.c:1213-1225`) —
and cleared the tile as a side effect. Nothing re-marked it, so every later sample of that
page read stale texels until something else happened to write there.

Split into `gl_write_flush()` (before `note_sample`, so the flush still clears `sampled`
before the note is taken — §0.5.3's ordering comment still holds) and a `gl_mark_dirty()`
moved to **after** the emit, in `gl_triangle()` and `gl_draw_rect()`. The transfer paths
keep the combined `gl_write_hazard()`; there nothing can sample and re-clear in between.
**Measured contribution on this title: exactly zero.** A run with `hwgl_no_defer` set
(fix 1 on, fix 2 off) produced numbers *byte-identical* to the run with both on, in all
three windows. It needs a primitive whose destination overlaps its own texture page, which
Crash does not do. It is kept because it is correct, and recorded as unmeasurable here so
nobody re-derives it or mistakes it for a contributor.

#### Fix 3 — GLSL ES 3.0 division is allowed 2.5 ULP, and a texel boundary is an integer

This is the one that took the diagnostic apparatus to find, and it is worth the retelling.

The bucket totals had stopped being informative, so `gl_parity_check` was made to dump both
surfaces — the GPU scanout and the software shadow — for one frame, and the difference was
rendered as an image. It was immediately obvious: the residue was **almost entirely the
pulsing menu text** ("START / LOAD GAME / PASSWORD / OPTIONS"), with the rest of the frame
showing only isolated sub-step dots. 36% of the mismatches were **exactly one pixel tall**,
i.e. single scanlines through a textured primitive.

Reading the raw values across one such scanline settled it. At row y=147 the software wrote
**pure black** across runs of x where the GPU drew the glow, while the rows above and below
matched exactly. `gpu_fetch_texel_bilinear` early-outs to 0 on a zero top-left tap
(`gpu.c:233`), so one scanline where the interpolated `ty` lands on a texel boundary makes
the software select the transparent texel row and skip the pixel, while the GPU selects the
opaque one and draws it.

`gpu.c` divides in C, where IEEE-754 division is **correctly rounded to 0.5 ULP**. **GLSL
ES 3.0 only requires highp division to be accurate to 2.5 ULP** (spec 4.5.1). Everywhere
else that is irrelevant; at a texel boundary the exact quotient is an *integer*, so the two
implementations land on opposite sides of `floor()` — and adjacent texels in a font atlas
are "opaque" and "transparent", so the disagreement is not a rounding step, it is a whole
pixel appearing or vanishing.

Fixed with a four-line `bdiv()` in the fragment shader, used for `tx`, `ty` and the Gouraud
colour: take the quotient, round it to the nearest integer `r`, and return `r` when
`r*den == num` exactly. When the true quotient is `r` that identity holds exactly (|z| and
|area| stay far inside 2^24 for real PS1 triangles), and a correctly-rounded division of an
exactly-representable value returns it unchanged — which is what `gpu.c` gets. When the test
fails nothing is claimed and the ordinary quotient stands, **so this can only ever agree
more**. Costs two multiplies and a compare per interpolated component.

Measured effect, window 3, with fixes 1 and 2 already in:

```
                differing            blank   extra    near     far    clustered
before bdiv  2515026 (1.1371%)      165582  387416  147286  1814742    1292248
after  bdiv  1041232 (0.4708%)       84650    3748  181608   771226     260258
```

`extra` fell by **99.0%** and clustering by 80%. Windows 1 and 2 were already clean and did
not move.

#### What is left, and why it is sub-step

On the dumped frame the residue is 1028 pixels of 122880 (0.84%), `extra` **0**, and:

```
  |d| <= 1 step   17.5%
  |d| <= 2 steps  92.0%      <- 74.5% of everything is exactly 2 steps
  |d| >= 3 steps   7.3%
```

Three sources, in order of size:

1. **The multi-blend precision divergence the file header already documents.** A pixel
   blended more than once keeps 8-bit precision in the RGBA8 target where the software
   re-truncates to 5 bits after every blend. Modes 0 and 3 leave the render target holding
   a multiple of 4 rather than 8, so the *second* layer onward reads a different
   background. Crash's title screen is exactly this case — pulsing, layered, semi-
   transparent text. Fixing it needs framebuffer fetch, which is driver-gated here.
2. **`-ffast-math` is in the Android build flags**, so the software reference's own
   `z0*c0 + z1*c1 + z2*c2` may be contracted to FMA and reassociated; the GLSL compiler may
   do the same, differently. Where the modulation product then saturates at 255 on one side
   and not the other, the two answers separate by a step or two.
3. Both sides truncate (`(int)`, `floor(c/8)`), so any sub-step difference at an integer
   boundary becomes a whole 5-bit step.

**Window 3 is not at zero and this checkpoint does not claim it is.** It is 9.5x better,
`extra` is gone, and the shape of what remains is sub-step rather than structural. The next
lever, if it is judged worth it, is item 1 — and that is a real feature (programmable
blending), not a bug fix.

#### The diagnostic apparatus that found all of this — it is in the tree, use it

* `gl_parity_check` now runs **three windows** (600 / 1800 / 3600) and **resets between
  them**. §0.5.3's numbers were the quiet window only; see the operational note below.
* It reports `clustered` (mismatches with a mismatching neighbour — regions vs speckle),
  `worst_scanout`, `clean` scanout count, and **four** sample coordinates per bucket
  instead of one.
* **Marker files** next to the diag log, read once at `create()`, let one binary reproduce
  every configuration — no rebuild, no settings-schema change:

  ```
  adb shell run-as com.nanodata.armsx touch files/logs/hwgl_no_bbox   # fix 1 off
  adb shell run-as com.nanodata.armsx touch files/logs/hwgl_no_defer  # fix 2 off
  adb shell run-as com.nanodata.armsx touch files/logs/hwgl_dump      # dump frame 3000
  ```

  Absent = the shipping behaviour. **They are read at create(), so they must exist before
  the game launches**, and a stale `hwgl_no_bbox` silently reinstates the bug — the device
  was left with all of them removed.
* `hwgl_dump` writes `files/logs/parity_gpu.bin` and `parity_soft.bin`, `w*h` raw BGR555
  each. Pull them with `run-as ... cat` and render a difference map. **Doing this was the
  step that turned "1.14% of pixels differ" into a named mechanism**; the bucket counters
  alone had stopped narrowing it down two iterations earlier.

In window 1 the 6110 remaining `near` pixels have `clustered=255`, i.e. **96% are isolated
single pixels** — the signature of rounding, not of a wrong primitive. Single-layer
transparency, dithering, the texture window, CLUT indexing, texel selection and the fill
rule are all exact: those are integer, and they are clean at zero.

#### Verified, and how

* `make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"` green, 13/13, at every build.
* Android build green each time, `bin/libarmsx.so` timestamp advanced each time.
* **The on-device `.so` was control-tested both ways** (§0.5.3's lesson): a string only the
  new build has → PRESENT; the old build's log format string → ABSENT; a string that never
  existed → ABSENT; a string that always existed → PRESENT. Later builds were verified
  harder: `llvm-objcopy -O binary --only-section=.text` of the APK pulled **off the device**
  vs the local `.so` — **identical sha256**.
* Launch verified from logcat (`ARMSX-JNI: core: run() enter` with the ROM path), not from
  a screenshot.
* The pre-fix numbers were **re-measured on the current binary**, not quoted from §0.5.3,
  and they matched it byte for byte.

#### Two operational facts worth keeping

* **`am start -n com.nanodata.armsx/.EmulatorActivity --esa ...EXTRA_NATIVE_ARGS <rom>`
  CRASHES** — SIGABRT in Android's `RenderThread` ~6 ms after `core: run() enter`, before
  the GL backend can exist, followed by `FORTIFY: pthread_mutex_lock called on a destroyed
  mutex`. It is the launch method, not the build: the same `.so` runs fine through the
  library. Launch with `monkey -p com.nanodata.armsx -c android.intent.category.LAUNCHER 1`
  and tap the cover (`input tap 128 1010` on the focused Recently Played tile at
  1080x1920). `input keyevent 23` on the focused tile does **not** activate it.
* **The first 600 frames of a disc are worthless as a parity sample** — `prims/f` averages
  **9.4** there (BIOS + boot logos), against 543 at frame 3000 and 885 at frame 3600. A
  clean number over frames 1-600 says almost nothing. `gl_parity_check` now runs three
  windows (600 / 1800 / 3600) and **resets between them**, so the busy windows are
  attributable on their own. §0.5.3's numbers were measured over the quiet window only.

#### Performance: not re-benchmarked, and here is exactly what is and is not known

The brief for this session said performance was already answered, so it was not
re-measured. The bounding-box geometry roughly doubles fragment count per triangle and the
argument that this is free is §0.5.3's own data: 1x and 2x both ran at ~153% uncapped with
`emu` ~10.8 ms, i.e. *quadrupling* fragments cost nothing measurable, so doubling cannot.

The one incidental observation, recorded as an observation and **not** as a measurement:
a capped 1x GLES frame sampled at 601 tri read `FRAME 18.2 emu 13.1 present 3.5 worst 23.8`
against §0.5.3's `FRAME 17.0 emu 12.4 present 3.5 worst 20.6` at the same triangle count.
**Those two are not comparable** — the `px` counter was 400.2K against 174.7K, so it is a
much heavier scene at the same triangle count, which is exactly the §0.5.3 trap about
matching scenes on the OSD's own counters. It is not evidence of a regression and it is not
evidence of its absence. If anyone wants the real number, match on `px`, not `tri`.

#### Device state left behind

`files/settings.toml` is set to `renderer = "hardware-gl"`, `internal_scale = 1`,
`logging_enabled = true`, `quiet = false` so the log is readable. All marker files removed.
The installed APK is the current build (repacked, not gradle — §0.5.3's loop, which works
exactly as documented; `zip -0` is required or install fails `res=-2`).

#### Exact next steps

1. **Land the caller side of the brokered seam** — §0.5.3's spec is unchanged and the
   renderer side is already applied and exported. `gl_display_buffer()` keeps the resolve
   and drops the `glReadPixels`; a new `armsx_hw_gl_present_texture(backend, renderer)` does
   resolve + adopt and returns true, in which case `main.cpp` skips
   `armsx_renderer_upload_frame()` for that frame. **Note it interacts with the parity
   check**, which reads `g->readback` — that buffer only exists because of the readback the
   seam removes, so `gl_parity_check` has to keep its own `glReadPixels` (gated on
   `scale == 1 && !parity_done`) or be moved behind the `hwgl_dump` marker.
2. §4 coherency, §4.6 downgrade first.
3. Optional, only if window 3's last 0.47% is judged worth it: programmable blending
   (framebuffer fetch) for the multi-blend precision term. That is a feature with a driver
   gate, not a bug fix — see §2.5.

**Known, not blocking, recorded so it is not rediscovered:** at S > 1 the shader samples at
`pf = gl_FragCoord.xy/S - 0.5`, i.e. native `X/S - 0.5/S`, while `gpu_hw_rt.c` — the
reference `make test-gpu` proves scale-coherent — samples at `X/S`. A quarter-native-pixel
offset at 2x. Identical at S=1 (both reduce to the integer corner), so it is not a parity
issue, but §3.3 warns specifically against a half-pixel term and the bounding-box geometry
above now makes aligning the two a safe one-line change. Not done here because 1x was the
gate and S>1 could not be verified in the same run.

### 0.5.1 CHECKPOINT — session state as of 2026-07-31 03:25

Written mid-task in case of interruption. A fresh engineer should be able to resume from
this section alone.

#### Tree state RIGHT NOW: builds green, nothing half-edited

`android/app/src/main/jniLibs/arm64-v8a/libarmsx.so` rebuilt 03:22:26 (7,275,416 bytes),
`build.sh android` clean, staged copy identical to `bin/libarmsx.so`. `make test-gpu`
green — 13 cases. **No file is in a known-broken state.** The GPU backend has NOT been
started; there is no partial GLES code anywhere in the tree.

Changed since the previous report (the Stage 3 remainder — mask bit + dither gate):

| File | Change |
|---|---|
| `psx/dev/gpu.h` | `accuracy_flags` field on `psx_gpu_t`; `PSX_GPU_ACCURACY_MASK_BIT` / `_DITHER_GATE` enum; `psx_gpu_set_accuracy_flags()` / `psx_gpu_accuracy_flags()`; three `static inline` resolvers — `psx_gpu_mask_check()`, `psx_gpu_mask_set()`, `psx_gpu_dither_enabled()` |
| `psx/dev/gpu.c` | GP0(E6) implemented (was the empty stub) — latches GPUSTAT bits 11/12 unconditionally; mask check + `\| mask_set` on the write in `gpu_render_triangle` and `gpu_render_rect`; dither now gated by `dither_on` |
| `frontend/gpu_hw_rt.c` | identical mask/dither logic, reading the SAME inline resolvers so the two rasterizers cannot drift |
| `frontend/config.h` / `config.c` | `accurate_mask_bit`, `accurate_dither` — both default **false** |
| `frontend/main.cpp` | settings fields, `psx_gpu_set_accuracy_flags()` call right after `psx_get_gpu()`, settings.toml writeback |
| `tests/gpu_renderer_parity.c` | mask/dither corpus; `default-path-unchanged` guard; accuracy-on variants |
| `Makefile` | `test-gpu` target gained `psx/perf.c` (another agent's `g_psx_perf` broke the link) |

**Both fixes default OFF**, so the software path is byte-for-byte what it always was. That
is enforced, not assumed — see the new `default-path-unchanged` case below. *(Historical: both
were later turned on by default — see §0.5.11. The rest of this checkpoint stands as written.)*

#### The API decision: GLES 3.0, not Vulkan. Reasoning, not just the conclusion.

Vulkan is the present backend that currently works on device, so it looks like the obvious
target. It is the wrong one, for a reason that is structural rather than aesthetic:

* `frontend/render_vk.cpp` leaks **nothing**. `struct VkRenderer` (`render_vk.cpp:206-262`)
  holds instance/device/queue/queue-family/command-pool inside an anonymous namespace
  spanning `:70-1355`; the only exported symbol is `armsx_render_create_vk` (`:1359`).
  `OpSdlHandle` (`:963`) returns `nullptr`. There is no accessor, no external-memory
  extension enabled at device creation, no exported semaphore, and the queue is owned
  exclusively behind a single `frame_fence`. A Vulkan rasterizer therefore needs either a
  large brokered accessor (device + queue + family + the `VkApi` table) or its own device
  plus `VK_KHR_external_memory` — which nothing enables today.
* `frontend/render_gl.cpp` needs far less. The EGL context is **already current on the
  emulation thread** (`EnsureContextCurrent`, `:603`; the header comment at `:591-602`
  says so explicitly and calls it "an accident of the call graph rather than a contract").
  ANGLE is wired as a first-class provider (`LoadEglApi`, `:706`, providers at `:714-717`,
  selectable via the public `armsx_render_gl_driver()`), giving one consistent GLES 3.1
  implementation across vendors. The present path has just been hardened through a
  black-screen debugging campaign and reads as healthy — one `TODO`-class comment in 1524
  lines, and it is describing a fixed bug (`:1194-1204`).

So: **GLES 3.0 core, ANGLE-compatible (nothing above ES 3.0 without an explicit gate).**

#### Constraints the GPU backend must respect — measured from the source, not assumed

* **GLES 3.0 hard floor.** Context requested as ES3 (`render_gl.cpp:833`, `:936`); shaders
  are `#version 300 es` (`ShaderHeader`, `:447-452`). ANGLE advertises 3.1. `render.h:148`
  states the rule outright: anything above ES 3.0 needs an ES-3.1 fallback or a gate.
* **The default framebuffer has no depth and no stencil** — `EGL_DEPTH_SIZE 0`,
  `EGL_STENCIL_SIZE 0` (`:837-839`, mirrored for SDL at `:1425-1426`). Mask-bit-via-stencil
  (§2.6) therefore requires our own FBO attachments; it cannot borrow the default FB.
* **`OpPresent` (`:1247-1276`) clobbers global GL state and never restores it:** disables
  `GL_BLEND`, `GL_SCISSOR_TEST`, `GL_DEPTH_TEST`, `GL_CULL_FACE`; leaves the viewport at
  the letterbox rect; leaves unit 0 bound to its own texture; sets `GL_UNPACK_ALIGNMENT 1`.
  It restores VAO and program to **0**, not to the previous value.
* **`OpPresent` never calls `glBindFramebuffer` and the loader table (`:184-226`) contains
  no framebuffer entry points at all** — it structurally assumes FBO 0 is bound. **If the
  backend leaves its own FBO bound, present silently renders into it and the screen goes
  black with no GL error.** Rebinding FBO 0 before returning is entirely on us.
* The proc loaders (`EglProcLoader` `:428`, `SdlProcLoader` `:423`) are anonymous-namespace
  and not exported. We must `dlopen` our own — and critically, we must open the **same**
  provider the present path bound, or we get system-GLES function pointers operating on an
  ANGLE context. `armsx_render_gl_driver()` (`render.h:158`) and
  `armsx_render_active_name()` (`:170`) are public and are enough to determine which.

#### The one thing that must be brokered

`render.h` has **no** way to hand the present layer a GPU texture. The only pixel ingress
is `armsx_renderer_upload_frame()` (`render.h:103`), which takes `const void* pixels` — a
CPU pointer. The internal vtable (`render_internal.h:16-34`) has ten slots and none of them
is an external-texture bind.

Consequence: a GLES backend can be built and made *correct* without any brokered change, by
doing `glReadPixels` of the display region each frame into the existing upload path (this
is exactly what §6 Stage 1 prescribes: "deliberately slow, but it isolates rasterization
bugs from coherency bugs"). But it cannot be made *fast* that way — a per-frame readback is
a full pipeline sync on a tiler. The brokered entry point is the difference between
"correct" and "worth shipping", and its shape is already drafted at
`frontend/gpu_hw_gl/gpu_hw.h:185-186`.

#### Dead ends and traps — do not retry these

1. **`psx_gpu_renderer_t`'s `display_texture` hook does not exist in the shipped ABI.** The
   drafted `frontend/gpu_hw_gl/gpu_hw.h` has it; `psx/dev/gpu_backend.h` deliberately does
   not. Do not go looking for it.
2. **A scale-coherence test that samples only `(x*N, y*N)` is not sufficient.** It checks
   1 render-target pixel in S² and **passes a deliberately injected half-pixel bug**. This
   was verified, not theorised. The `hw-*-blocks-uniform` cases exist specifically to close
   that hole, and they only close it because the axis-aligned corpus contains **textured**
   sprites — with untextured sprites alone, the UV-scaling control also passes.
3. **The mask-bit corpus was initially inert.** `draw_corpus()` narrows the drawing area to
   `(64,48)-(320,240)` partway through and never restores it, so mask primitives originally
   placed at x≥360 were clipped away entirely and the "backend forgets the mask bit"
   control passed. They now live at x 230..312, y 198..238. **Any primitive added to that
   corpus after the clipping section must be inside that box or it tests nothing.**
4. **`run-as` does not work** — `com.armsx2` is not debuggable, so app-private files cannot
   be read and the `.so` inside the APK cannot be swapped.
5. `zsh` eats unquoted `grep --include=*.c`; quote it or get a false negative.

#### Device reality — why there are no performance numbers here

Retroid Pocket 6 (`kalama`, Adreno 740) is on adb as `390011f0`. **But the installed
`com.armsx2` is versionName 2.6.6.2, versionCode 1469, lastUpdateTime 2026-07-30 18:09 —
which predates every change in §0.5.** It contains neither the CPU backend nor these
accuracy fixes. The app is not debuggable, so the `.so` cannot be hot-swapped, and building
an APK needs gradle, which this agent must not run.

**Therefore no 1x/2x/3x measurements exist yet, and none can be taken until the coordinator
builds and installs an APK from the current tree.** That is a hard blocker on the
performance half of the task, and it is a blocker on validating any GLES backend at all.
Nothing here should be read as a performance claim.

#### Exact next step, and the one after

1. **Next:** implement `frontend/gpu_hw_gl.c` — a GLES 3.0 backend behind the existing
   `psx/dev/gpu_backend.h` vtable, so it inherits the whole gate suite unchanged. Order:
   own `dlopen` of the provider matching `armsx_render_gl_driver()`; an FBO with an
   `RGBA8` `1024S x 512S` colour target plus `STENCIL_INDEX8`; an `R16UI` `1024x512`
   `vram_read` texture refreshed wholesale at first (correctness before speed); one
   program covering flat/Gouraud/textured; scanout via `glReadPixels` of the display
   region into the existing upload path. Save/restore every piece of GL state listed above
   and **always rebind FBO 0 on the way out**. Select it with a third `renderer` value
   (`renderer = "opengl"`), leaving `"hardware"` meaning the CPU backend, so both can ship
   side by side and be A/B'd on the same device.
2. **After that:** Stage 5 coherency (§4) — but land §4.6's automatic downgrade
   (count GP0(C0) readback bytes/frame, log once, fall back to software) *before* any of
   the optimisations, per the coordinator's instruction that the fallback precedes the
   speedups.

**§5.3's threading question is RESOLVED, favourably.** `runVMThread` calls
`external_main_ex()`, and `frontend/main.cpp` creates the renderer, runs `psx_update()`
and presents all on that one thread. There is no separate render thread, so a GL context
made current by the present backend is *already* current on the thread that drives
`psx_gpu_write32()`. A GLES backend needs no thread hand-off — but note it only has a
context when `gpu_backend = "opengl"`, so a Vulkan rasterizer backend is what pairs with
`gpu_backend = "vulkan"`.

### 0.5.2 CHECKPOINT — GPU backend session, 2026-07-31 ~10:30

Second engineer, resuming after the first was killed by a spend limit. This subsection
supersedes §0.5.1's "exact next step" and is the live status from here on.

#### First: a build-recipe correction that costs 10 minutes to rediscover

`make test-gpu` does **not** build as written on this Mac. `sdl2-config` is not installed,
so `SDL_CFLAGS` is empty and `frontend/gpu_hw.h`'s `#include <SDL.h>` fails. §0.5.1 says
the gate was green; it was, but only with an SDL include path in the environment. The
invocation that works against the vendored copy is:

```
make test-gpu SDL_CFLAGS="-Ithird_party/SDL/include"
```

Verified green, 13 cases, on the tree as inherited. That is the baseline every later run
is compared against.

#### The API decision, re-derived against current facts: GLES 3.0, same conclusion, partly different reasoning

The brief said both of §0.5.1's premises had changed (GL present is fixed; ANGLE is
bundled; Vulkan is the present path that works on device). Re-derived:

* **Vulkan is still structurally closed.** `render_vk.cpp` exports exactly one symbol
  (`armsx_render_create_vk`, `:1359`); instance/device/queue/family/command-pool live in an
  anonymous namespace, `OpSdlHandle` returns `nullptr`, no external-memory extension is
  enabled at device creation. A Vulkan rasterizer needs a *large* brokered accessor — the
  whole `VkApi` table plus device, queue, family and the frame fence — or its own second
  device plus `VK_KHR_external_memory`, which nothing enables. That has not changed and is
  not a matter of opinion about GL's health.
* **GL needs one small brokered addition, not a large one.** The context is already current
  on the emulation thread, and the only missing piece is a way to hand the present layer a
  GPU texture (§0.5.1 established this; it is still true — `render_internal.h`'s ten-slot
  vtable has no external-texture bind). Everything else — entry points, FBO, programs — the
  backend can do for itself with its own `dlopen`.
* **ANGLE makes "GL" and "Vulkan" much less of a fork than they look.** ANGLE is GLES-on-
  Vulkan. Selecting `gpu_backend = "opengl"` with the ANGLE provider means the rasterizer's
  GL calls are executing on the device's Vulkan driver anyway, with one consistent shader
  compiler across vendors. So the practical gap between "write a GL rasterizer" and "write a
  Vulkan rasterizer" is a large brokered accessor and a second device, in exchange for very
  little.

**Decision: GLES 3.0 core (ANGLE-compatible), pairing with `gpu_backend = "opengl"`.**
Cost of being wrong: the ABI is unchanged, so a Vulkan backend later reuses every hook, the
config plumbing, the parity gates and the vertex/coordinate model.

#### The architectural decision that actually removes the user's problem

The brief frames the CPU backend's cost as "2x rasterization at 1x, growing as S²". The
second half is the part that makes upscaling unusable, and it is removable **without**
landing §4's coherency layer, by keeping `PSX_GPU_BACKEND_SOFTWARE_SHADOW` set on the GPU
backend too:

| | CPU rasterization | GPU rasterization |
|---|---|---|
| software only (today's default) | 1x | — |
| CPU backend (`renderer = "hardware"`) | 1x + S² | — |
| **GL backend with the software shadow** | **1x, constant in S** | S² (free on a GPU) |
| GL backend owning VRAM (the §4 endgame) | 0 | S² |

Keeping the shadow costs one constant unit of CPU — exactly what the software renderer
costs today, which the user already runs — and makes the *upscale* free. That is the whole
ask. It also makes the highest-variance part of the port unnecessary for this landing:

* `gpu->vram` stays authoritative, so **GP0(C0) readback costs nothing and never stalls**.
  The §4.6 fallback ladder's main trigger cannot fire.
* Textures are sampled from a native-resolution `vram_read` texture whose source
  (`gpu->vram`) is *always correct*. §4 collapses from a two-way ownership problem to a
  one-way upload problem: keep `vram_read` in sync with host VRAM. Dirty tracking becomes an
  optimisation rather than a correctness requirement.
* Save states, the whole-VRAM debug view and 24bpp FMV scanout all keep working untouched.

The one visual consequence, stated so nobody debugs it later: **render-to-texture content is
sampled at native resolution.** A game that draws into VRAM and then uses that region as a
texture gets the software rasterizer's native-res copy, not the upscaled one. Correct, just
not upscaled. Dropping the shadow (the real §4) is what fixes that, and it is the *next*
optimisation, not this one.

#### How 1x stays pixel-identical on a GPU — the mechanism, because it is not obvious

A GPU interpolates attributes at the fragment centre `(x+0.5, y+0.5)`; `gpu_render_triangle`
evaluates them at the integer coordinate `(x, y)` (`gpu.c:331-332`). That half-pixel is a
guaranteed parity failure for every Gouraud and every textured polygon. The fix is to not
use hardware interpolation at all for attributes:

* Each vertex carries **all three** triangle vertices, colours and UVs as `flat` attributes.
* The fragment shader recomputes the edge functions itself at
  `pf = gl_FragCoord.xy / S - 0.5`, which at `S == 1` is **exactly** `(x, y)` — the
  software's sample point — and at `S > 1` is the natural sub-pixel position, so the same
  shader upscales correctly with no second code path.
* The `TL` fill rule (`gpu.c:266-267`) is evaluated in the shader too, so coverage matches
  by construction rather than by trusting the GPU's fill rule to agree.
* `gpu_fetch_texel_bilinear` (`gpu.c:225`, used unconditionally by polygons) is ported
  verbatim as four `texelFetch`es, so §7.4 deviation #1 is *reproduced*, not "fixed".

This is deliberately "the software rasterizer, in a fragment shader". It costs ~10 extra ALU
ops per fragment, which a GPU does not notice, and it buys the parity gate.

#### Semi-transparency: three of the four modes need no batch break at all

§2.5 proposes segmenting batches by blend mode. Working it through, the segmentation is much
coarser than that. With blending permanently enabled as `(ONE, SRC_ALPHA)` + `FUNC_ADD`:

| Fragment | shader emits | result |
|---|---|---|
| opaque (incl. every non-`PA_TRANSP` primitive) | `rgb = F`, `a = 0` | `F*1 + B*0 = F` — a plain write |
| mode 0 | `rgb = F*0.5`, `a = 0.5` | `0.5F + 0.5B` |
| mode 1 | `rgb = F`, `a = 1` | `F + B` |
| mode 3 | `rgb = F*0.25`, `a = 1` | `0.25F + B` |

So opaque primitives, modes 0, 1 and 3, and the per-texel STP mix *inside* one primitive all
coexist in a **single draw call with no state change**. Only mode 2 (`B - F`,
`FUNC_REVERSE_SUBTRACT`) needs its own range, and only textured mode-2 primitives need the
two-pass split. Batches therefore break on: mode-2 boundaries, drawing-area (scissor)
changes, `vram_read` refreshes, VRAM transfers, and buffer-full. The texture window is
per-vertex, so GP0(E2) does **not** break a batch.

The 8-bit arithmetic works out exactly, which is why the render target is `RGBA8` and not
`RGB5_A1`: the shader emits 5-bit values pre-multiplied to 8-bit (`c5 * 8`), every blend
result stays a multiple of 4, and truncating `>> 3` at readback recovers the software's
integer answer bit for bit.

#### Status: nothing of this is in the tree yet as of this line

Written before the first line of `frontend/gpu_hw_gl.c` exists, deliberately, so the design
survives an interruption. Tree is exactly as inherited; `make test-gpu` green (13 cases).

### 0.5.3 CHECKPOINT — third session, 2026-07-31 from ~11:25

Third engineer. **Read this before believing §0.5.2's closing line or §0.5.1's "Device
reality" paragraph — both are stale in ways that cost real time.**

#### Two inherited facts that were wrong, and how they were caught

1. **§0.5.2 ends "nothing of this is in the tree yet". It is not true.**
   `frontend/gpu_hw_gl.c` is **complete** — 2520 lines — and was written between that line
   being typed (10:16) and the session ending (10:40). It is listed in the `Makefile`
   (`C_SOURCES += frontend/gpu_hw_gl.c`, :318), wired into `frontend/main.cpp`
   (`armsx_hw_gl_create()` at :1870, failure poll at :2372), and **present in the built
   `.so`**. `ARMSX1_STATUS.md` §2.5's "`gpu_hw_gl.h` exists (the new backend's header)" is
   the same understatement. Check timestamps against the doc before trusting a status line.

2. **§0.5.1's "the installed build predates every change" blocker was checking the wrong
   package.** ARMSX1 is **`com.nanodata.armsx`** (`android/app/build.gradle:24`);
   `com.armsx2` is the *PS2* app, which is also installed on the same device. Consequences,
   all of them good:
   * `com.nanodata.armsx` is **DEBUGGABLE**, so **`run-as` works**. App-private
     `files/settings.toml`, `files/logs/`, save states and prefs are all readable *and
     writable* from adb. Every setting in this document can be A/B'd without touching the UI.
   * §0.5.1 dead end #4 ("`run-as` does not work") is about the *PS2* app and does not apply.

#### Verifying a `.so` against a stripped APK — and a false negative worth remembering

Gradle strips on packaging (1.44 MB in the APK vs 7.40 MB unstripped), so `sha256` of the
two files never matches and proves nothing. Compare the **`.text` section**:

```
llvm-objcopy -O binary --only-section=.text <so> out.text     # NDK's copy; not on PATH
```

That showed `android/app/build/outputs/apk/debug/app-debug.apk` byte-identical to
`bin/libarmsx.so`, i.e. the APK the previous session left on disk **is** the current native
code. It only needed installing.

While probing this, a `strings` check for four log messages returned three PRESENT and one
ABSENT, which looked like "the `.so` is stale". It was not: **that fourth string is inside a
`/* comment */`**, so it never reaches `.rodata`. A control string that genuinely does not
exist is what disambiguated it. Never read a single absent string as evidence.

#### The build/measure loop, which does NOT need gradle

The coordinator builds and installs, but the whole loop is available locally, because the
only gradle-produced artifact that changes is one `.so` inside an already-signed APK:

```
./build.sh android                                    # produces bin/libarmsx.so
cd <scratch> && cp app-debug.apk repack.apk
zip repack.apk lib/arm64-v8a/libarmsx.so              # replace in place
$BT/zipalign -p -f 4 repack.apk aligned.apk
$BT/apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android \
    --key-pass pass:android --ks-key-alias androiddebugkey --out signed.apk aligned.apk
adb install -r signed.apk
```

Verified working end to end (`build-tools/36.0.0`, JBR 21 from Android Studio). The debug
keystore is the same one gradle signs with, so `install -r` upgrades in place and keeps app
data.

#### Where the logs actually are — the reason "no rasterizer log" meant nothing

`log_info()` is `psx/log.h`'s macro onto `log_log()`, which `fprintf(stderr, ...)`s, and
`frontend/diagnostics.h` redirects `fprintf` into the **diag file**. `psxe_diag_logf("hwgl",
…)` goes to the same place. **None of it reaches logcat**, and all of it is suppressed while
`[runtime] logging_enabled = false` / `quiet = true`, which is the shipped default.

The first on-device run therefore showed *no* `Rasterizer:` line and looked exactly like
"the backend never initialised". It had initialised. Set `logging_enabled = true` and
`quiet = false`, then read `files/logs/armsx.log` through `run-as`.

#### The GL rasterizer works on hardware

Retroid Pocket 6, Adreno 740, `OpenGL ES 3.2 V@0676.53`, present backend
`OpenGL ES (system)`, Crash Bandicoot (USA):

```
[hwgl] GL rasterizer up: system GLES, shared with present context, scale=1x,
       target=1024x512, max_texture=16384
[psx]  Rasterizer: GLES (GPU) at 1x internal resolution
```

"shared with present context" is the good path — the backend found the present backend's GL
context already current on the emulation thread and did **not** have to create its own.

**Batching is working and is not marginal.** At the busiest point sampled:

```
frame=3000 scale=2 draws/f=3.6 ranges/f=2.7 prims/f=543.2 vramsync/f=6.63 syncpx/f=5636
```

543 primitives in 3.6 draw calls — ~150 primitives per call, and at quieter moments 488
prims in 1.3 calls. The blend-state analysis in §0.5.2 (modes 0/1/3 and opaque all sharing
one state) is what buys that; it is doing exactly what it was designed to do.
`c0bytes=96` for a whole session confirms §0.5.2's other claim: with the software shadow,
GP0(C0) readback traffic is effectively zero and §4.6's fallback ladder cannot fire.

#### The SOFTWARE_SHADOW claim: validated for rasterization, but it is NOT the whole S²

This was the first thing the brief asked to check, so, precisely:

| cost term | scales with S? | where |
|---|---|---|
| PS1 rasterization semantics per pixel | **no** — moved to the GPU | fragment shader |
| software shadow (`gpu->vram` stays authoritative) | **no** — constant, = today's cost | `gpu.c` |
| vertex emission | no | `gl_emit_*` |
| **scanout `glReadPixels` of `w·S × h·S`** | **YES, S²** | `gl_display_buffer`, :1910 |
| **re-upload of those bytes to the present texture** | **YES, S²** | `main.cpp` upload path |

So the claim holds for the expensive half — the per-pixel PS1 semantics — and that is the
part that made the CPU backend unusable. But scanout still moves S² bytes off the GPU and
back, **and on a tiler `glReadPixels` is a full pipeline sync**, which costs more than the
bytes. At 512×240: 240 KB/frame at 1x, 960 KB at 2x, 2.16 MB at 3x, each way, every frame.

**This is exactly what the brokered seam below removes, and it is why that seam is not
optional.** §0.5.2 was right that the coherency layer (§4) is not needed to ship; it
understated that scanout is a second, independent S² term.

#### 1x is NOT yet pixel-identical — 0.21% of pixels differ

The backend measures this itself at runtime (`gl_parity_check`), because
`tests/gpu_renderer_parity.c` runs on the host where there is no GL context and so can
never reach this backend. Measured over the first 600 frames of a 1x session:

```
1x parity frame=600 pixels=124723200 differing=265522 (0.2129%)
          first=(637,2) gpu=0x0000 soft=0x0421
```

`gpu=0x0000` against a non-zero software pixel means **the backend did not write there at
all**, which is a different class of bug from a rounding disagreement — so the raw
percentage is not actionable on its own. `gl_parity_check` was therefore extended to bucket
every mismatch into `blank` (GPU 0, software non-zero), `extra` (the reverse), `near` (every
channel within one 5-bit step — rounding or dither) and `far` (genuinely different colours),
and to record the first `far` pixel separately. **Built, installed and measured:**

```
1x parity frame=600 pixels=124723200 differing=265522 (0.2129%)
          blank=169380  extra=758  near=9288  far=86096
          first=(637,2)   gpu=0x0000 soft=0x0421
          firstfar=(447,240) gpu=0x5ad6 soft=0x0016
```

**This rules out the benign explanation.** `near` — the bucket that the deliberate
multi-blend precision divergence documented in `gpu_hw_gl.c`'s header would land in — is only
**3.5%** of the mismatches. The defect is `blank` (**64%**, the backend not writing at all)
and `far` (**32%**, genuinely wrong colours). Those are real bugs, not tolerances.

**It is visible, and it is worth looking at before theorising.** Compare a 1x GLES title
screen against a 1x software one: the GLES image has scattered wrong-colour speckle along
polygon edges and across textured surfaces — the "BANDICOOT" banner and Crash's eyes are
obviously peppered where software renders them cleanly. It reads as an edge/texel defect,
not as a whole-primitive one.

Ruled out by direct comparison against `psx/dev/gpu.c`, so do not spend time re-checking:

* the texture-window mask and the `& 0xff` wrap — `gpu_fetch_texel` (`gpu.c:192-196`) and the
  shader's `fetch_texel` apply them in the same order with the same truncation, including for
  negative `tx`/`ty` (two's complement makes `-1 & 0xff == 255` in both);
* linear VRAM addressing past `x = 1023` — the shader's `vram_at()` decomposes
  `lin & 1023` / `lin >> 10`, which is exactly the software's flat `x + y*1024`, so the
  §7.4 deviation #8 overrun is reproduced rather than diverging;
* the bilinear early-out on a zero top-left tap (`gpu.c:232-233`) — present in both;
* the drawing-area clip — `gl_range_for()` uses `draw_x2 - draw_x1 + 1`, inclusive, matching
  the software's `x <= draw_x2`;
* vertical orientation — the render target, the resolve pass and the present shader all agree
  that native row 0 is texel row 0, which is why the on-screen image is upright.

Still open, in rough order of likelihood: the `discard` on `texel == 0u` interacting with
fragments the software would have written; sub-pixel disagreement in the shader's
recomputed edge functions at exactly-integer coordinates; and the `far` signature at
`(447,240)` where the GPU produced `(22,22,22)` against the software's `(22,0,0)` — the red
channel matches **exactly** while green and blue take red's value, which looks like a
channel-replication or modulation bug rather than a coverage one and is the single most
specific clue available.

#### Measured performance, Retroid Pocket 6 / Adreno 740, Crash Bandicoot

**Read `emu`, not `present`.** `session_.updateTexture()` — which calls
`psx_gpu_get_display_surface()` and therefore the resolve, the `glReadPixels` and the upload —
runs *inside* the emu phase (`main.cpp:4836`, between `phase_frame_begin` and
`phase_emu_end`). `present` is only the letterbox draw and the swap. So `emu` is the number
that contains the whole rasterizer plus scanout cost.

**Capped** (`frame_limit = true`, `vsync = true`) — all configs traverse identical emulated
time, so a fixed wall-clock offset lands on the same scene. Matched on the OSD's own counters:

| config | scene (tri / px) | FPS | FRAME | emu | present | worst |
|---|---|---|---|---|---|---|
| software 1x | 683 / 178.9K | 58.6 | 16.8 | 11.7 | 4.9 | 18.5 |
| **GLES 1x** | 601 / 174.7K | 59.9 | 17.0 | **12.4** | 3.5 | 20.6 |
| GLES 2x | 766 / 350.2K | 59.1 | 16.7 | 12.4 | 3.9 | 20.6 |
| GLES 3x | 844 / 263.6K | 58.7 | 16.9 | 11.7 | 0.3 | 22.1 |

**Uncapped** (`frame_limit = false`, `vsync = false`) — this measures throughput, but the
configs then advance through the demo at *different* rates, so scenes drift apart and only
like-scene rows may be compared:

| config | scene | FPS | speed | emu |
|---|---|---|---|---|
| software 1x | 546 tri / 607.4K px | 89.6 | 151% | 10.9 |
| GLES 1x | 486 tri / 175.6K px | 91.4 | 154% | 10.7 |
| **GLES 2x** | title screen, 616 tri / 212.3K px | **90.9** | **153%** | 10.8 |
| GLES 3x | 700 tri / **1.36M px** (heavy fill) | 51.3 | 87% | 19.2 |
| **CPU 2x** | 722 tri / 416.1K px | **53.4** | **90%** | **17.1** |

Two conclusions the data actually supports:

1. **Upscaling on the GPU is close to free up to 2x.** GLES 1x and GLES 2x both sit at
   ~153% uncapped with `emu` ~10.8 ms, i.e. **2x costs the CPU nothing measurable over 1x**,
   which is exactly what moving the S² term to the GPU was supposed to buy.
2. **It is the CPU backend that upscaling was unusable on.** CPU 2x manages 90% / `emu`
   17.1 ms where GLES at the same scale is at ~153%. That gap is the whole point of the port.

3x is the honest caveat: on ordinary scenes it holds ~59 fps capped, but the one heavy
fill scene sampled (1.36M pixels — roughly 11x overdraw on a 512x240 framebuffer) fell to
87%. That is a *fill-rate* limit, and it is the case the brokered seam helps least; what
helps there is not readback but the scale itself.

⚠ Every row above is a single sampled frame from a moving scene, not an average over a fixed
workload. Treat differences under ~1 ms as noise.

⚠ **A methodology trap that nearly produced fabricated data.** The first sweep tapped the
cover on a fixed 14 s delay. On one config the boot splash was still up, the tap did
nothing, and the run produced six screenshots **of the library** — which, had they only been
skimmed for an FPS number, would have been reported as a measurement. Every run must verify
it actually launched (`bench.sh` polls logcat for `ARMSX_LAUNCH_PS1` and re-taps), and
scenes must be matched by the OSD's own `tri`/`rect`/`px` counters before two numbers are
compared.

#### THE BROKERED SEAM — exact specification

This is the one edit the rasterizer is not allowed to make itself, and the table above is
why it matters: it removes both S² scanout terms *and* the per-frame pipeline sync.

**It needs no new shader and no new program in the present path.** `ResolveFormat()`
(`render_gl.cpp`) already maps `SDL_PIXELFORMAT_BGR555` to `GL_RG8` with `packed_555 = true`,
selecting `kProgram555Nearest` / `kProgram555Linear`. The rasterizer's scanout target
(`hw_gl_t::resolve_tex`) is **already `GL_RG8` holding packed BGR555** — it is byte-for-byte
what the CPU path uploads today, just never leaving the GPU. The two sides already agree.

**Orientation is already correct, so nothing must be flipped.** The present vertex shader
uses `v_uv = vec2(a_pos.x*0.5+0.5, 0.5 - a_pos.y*0.5)` (`render_gl.cpp:246`), i.e. screen top
↔ texel row 0. The rasterizer's draw VS maps native `y = 0` to NDC `y = -1`, which is texel
row 0 of the attached texture. Both conventions already match, which is also why the current
`glReadPixels` path shows an upright image.

**1. `frontend/render.h`** — public entry point, in the `frame path` section directly after
`armsx_renderer_upload_frame`:

```c
/* Adopt a texture the caller already has resident on the GPU as the source for the next
   armsx_renderer_present(), instead of the pixels last handed to upload_frame(). This
   exists so the internal-resolution rasterizer (frontend/gpu_hw_gl.c) can present the
   target it just rasterized into without a glReadPixels round trip — on a tiler that
   readback is a full pipeline sync, and it is the largest remaining cost in the upscaled
   path.

   `texture` is a GLuint naming a GL_TEXTURE_2D and is only meaningful when the active
   backend is ARMSX_RENDER_BACKEND_OPENGL: it lives in the presentation context's object
   namespace, so the caller MUST have created it on that same context. `sdl_format`
   describes the texture's contents and takes the same values upload_frame() accepts
   (SDL_PIXELFORMAT_BGR555 in GL_RG8, or SDL_PIXELFORMAT_RGB24 in GL_RGB8). Texel row 0 is
   the TOP row, matching upload_frame().

   `width`/`height` are the texture's real pixel dimensions; they feed
   armsx_render_compute_dst() exactly as the uploaded texture's do, so an upscaled texture
   letterboxes identically to its 1x equivalent (the ratio is preserved by construction).

   The adoption lasts until the next upload_frame() or until texture 0 is passed, either of
   which returns the backend to the CPU path. Returns false and changes NOTHING when the
   active backend is not OPENGL or has no context — that is the caller's cue to keep using
   upload_frame(), never a fatal error, because the present backend is user-selectable at
   runtime. */
bool armsx_renderer_adopt_gl_texture(armsx_renderer_t* renderer,
                                     unsigned int texture,
                                     int width,
                                     int height,
                                     Uint32 sdl_format);
```

**2. `frontend/render_internal.h`** — the new vtable slot. It **must be APPENDED after
`shutdown`**, not inserted: all three ops tables (`kGlOps` `render_gl.cpp:1349`, `kVkOps`
`render_vk.cpp:1342`, `kSdlOps` `render_sdl.cpp:221`) use **positional aggregate
initialisation**, so inserting anywhere else silently reassigns every function pointer after
it. Appending means **`kVkOps` and `kSdlOps` need no edit at all** — C++ value-initialises
the omitted member to `nullptr`, which is exactly the "cannot do this" answer.

```c
    /* NULL on backends that cannot take an external texture; the dispatcher returns false. */
    bool (*adopt_gl_texture)(armsx_renderer_t* self, unsigned int texture,
                             int width, int height, Uint32 sdl_format);
```

**3. `frontend/render.cpp`** — dispatch, matching the file's existing null-guard style:

```c
bool armsx_renderer_adopt_gl_texture(armsx_renderer_t* renderer, unsigned int texture,
                                     int width, int height, Uint32 sdl_format) {
    if (!renderer || !renderer->ops || !renderer->ops->adopt_gl_texture) {
        return false;
    }
    return renderer->ops->adopt_gl_texture(renderer, texture, width, height, sdl_format);
}
```

**4. `frontend/render_gl.cpp`** — three small changes:

* three fields on `GlRenderer`: `GLuint external_texture = 0; int external_width = 0,
  external_height = 0; Uint32 external_format = SDL_PIXELFORMAT_UNKNOWN;`
* `OpAdoptGlTexture()`: reject unless `ResolveFormat(sdl_format, …)` succeeds (or
  `texture == 0`, which clears), store the four values, set `has_frame = true`, return true.
* `OpUploadFrame()`: `self->external_texture = 0;` at the top, so a CPU upload always wins
  back the source and the two paths can never both be live.
* `OpPresent()`: where it currently computes
  `drawable = self->texture != 0 && self->has_frame && ResolveFormat(self->texture_format, …)`,
  prefer the external texture when set — use `external_texture` / `external_width` /
  `external_height` / `external_format` for the bind, for `compute_dst` and for `u_texsize`.
  Everything else (program selection, filter, viewport, swap) is unchanged.
* add `OpAdoptGlTexture` as the eleventh entry of `kGlOps`.

**5. Caller side (mine, no help needed).** `gl_display_buffer()` keeps the resolve pass and
drops the `glReadPixels`; `main.cpp` calls a new
`armsx_hw_gl_present_texture(backend, renderer)` which performs the resolve and the adopt and
returns true, in which case `main.cpp` skips `armsx_renderer_upload_frame()` for that frame.
The `display_buffer` vtable entry stays as the fallback for every non-GL present backend, so
returning NULL keeps its existing "fall back to `gpu->vram`" meaning and the ABI is unchanged.

#### Exact next steps, in priority order

1. **Fix 1x parity.** It is the stated non-negotiable and it is the only thing genuinely
   blocking this from being shippable — the performance question is already answered. The
   buckets and the ruled-out list above are the starting point; the `(22,22,22)` vs
   `(22,0,0)` `far` signature is the sharpest lead. A useful next diagnostic is to bucket by
   *primitive kind* (flat / Gouraud / textured-sprite / textured-poly), which localises it to
   one emission path in a single run.
2. **Land the brokered seam** (spec above) and the caller side. Removes both S² scanout terms
   and the per-frame pipeline sync.
3. **Only then** §4's coherency layer, with §4.6's automatic downgrade landed *before* any of
   its optimisations. Note that with the software shadow in place the §4.6 trigger cannot
   currently fire (`c0bytes=96` for a whole session), so it must be tested deliberately rather
   than waited for.

Not blocking, but worth knowing: the mask bit is still not reproduced on the GPU (the render
target is RGBA8 with no stencil attachment), so `accurate_mask_bit = true` diverges by design
on the GLES path. `gl_parity_check` masks bit 15 on both sides so it does not pollute the
numbers above.

---

## 1. Semantics inventory extracted from `psx/dev/gpu.c`

### 1.1 Command dispatch, and what is actually reachable

`psx_gpu_update_cmd` (`gpu.c:1893`) dispatches in two tiers:

```c
int type = (gpu->buf[0] >> 29) & 7;          // gpu.c:1894
switch (type) {
    case 1: gpu_poly(gpu); return;           // gpu.c:1897  — GP0 0x20..0x3F
    case 2: gpu_line(gpu); return;           // gpu.c:1898  — GP0 0x40..0x5F
    case 3: gpu_rect(gpu); return;           // gpu.c:1899  — GP0 0x60..0x7F
}
switch (gpu->buf[0] >> 24) { ... }           // gpu.c:1902
```

The `return`s in the first tier mean **every `case 0x24 … case 0x7f` in the second
switch (`gpu.c:1906-1936`) is unreachable.** Consequently the following are dead:

| Dead function | Lines | Dead command stub | Lines |
|---|---|---|---|
| `gpu_render_flat_rectangle` | 678-713 | `gpu_cmd_24` | 1517-1557 |
| `gpu_render_textured_rectangle` | 715-764 | `gpu_cmd_28` | 1328-1356 |
| `gpu_render_flat_triangle` | 766-818 | `gpu_cmd_2c` | 1470-1514 |
| `gpu_render_shaded_triangle` | 820-909 | `gpu_cmd_2d` | 1560-1604 |
| `gpu_render_textured_triangle` | 911-997 | `gpu_cmd_30` | 1359-1386 |
| | | `gpu_cmd_38` | 1389-1420 |
| | | `gpu_cmd_3c` | 1423-1467 |
| | | `gpu_cmd_40` | 1753-1774 |
| | | `gpu_cmd_60` / `_64` / `_68` / `_74` / `_7c` | 1699 / 1606 / 1727 / 1668 / 1637 |

**Live drawing paths — the complete list a backend must implement:**

| What | Parser | Rasterizer | Lines |
|---|---|---|---|
| Polygons (tri/quad × flat/gouraud × untex/tex × opaque/semi) | `gpu_poly` `gpu.c:1067` | `gpu_render_triangle` | 251-449 |
| Rectangles / sprites (variable, 1×1, 8×8, 16×16) | `gpu_rect` `gpu.c:1001` | `gpu_render_rect` | 453-597 |
| Lines (flat, 2-point only) | `gpu_line` `gpu.c:1180` | `gpu_render_flat_line` → `plotLine` | 669-676, 653-667, 599-651 |
| Fill rectangle GP0(02) | — | inline | 1804-1857 |
| VRAM→VRAM copy GP0(80) | — | inline | 1859-1891 |
| CPU→VRAM GP0(A0) | — | inline | 1260-1325 |
| VRAM→CPU GP0(C0) + GPUREAD drain | — | inline | 1776-1802, 93-140 |
| Env registers GP0(E1..E6) | — | inline | 1940-1967 |

**Design consequence:** a `psx_gpu_backend_t` with ~12 entry points covers 100% of the
live surface. The dead code should be deleted in a separate cleanup commit *before* the
port lands, so nobody ports a rasterizer that can never run. (Deliberately not done here
— this task is design-only.)

### 1.2 Primitive command encodings

**Polygons** (`gpu_poly`, `gpu.c:1067-1178`). Attribute byte is `buf[0] >> 24`
(`gpu.c:1094`), decoded with the `PA_*` flags from `gpu.h:62-68`:

| Flag | Value | Meaning |
|---|---|---|
| `PA_RAW` | 0x01 | raw texture (no vertex-colour modulation) |
| `PA_TRANSP` | 0x02 | semi-transparent |
| `PA_TEXTURED` | 0x04 | textured |
| `PA_QUAD` | 0x08 | 4 vertices instead of 3 |
| `PA_SHADED` | 0x10 | Gouraud |

Word-count computation (`gpu.c:1084-1087`):
`fields_per_vertex = 1 + shaded + textured`, `vertices = 3 + quad`,
`args = fields*verts - shaded`.

Field strides (`gpu.c:1099-1103`) — these are the exact layout rules a backend's parser
must reproduce if it ever intercepts earlier than `gpu_render_triangle`:

```c
color_offset = shaded * (2 + textured);          // 0 / 2 / 3
vert_offset  = 1 + (textured|shaded) + (textured&shaded);  // 1 / 2 / 2 / 3
texc_offset  = textured * (2 + shaded);          // 0 / 2 / 3
texp_offset  = textured * (4 + shaded);          // 0 / 4 / 5
```

`poly.clut = buf[2] >> 16` (`gpu.c:1105`) — always word 2.
`poly.texp = buf[texp_offset] >> 16` (`gpu.c:1106`).

Vertex coordinates are sign-extended 11-bit via `SE10` (`gpu.c:9`), applied at
`gpu.c:1138-1145`. UVs are 8-bit, `gpu.c:1146-1153`. Colours are 24-bit `0x00BBGGRR`,
`gpu.c:1134-1137`.

**Latching side effect (important):** a textured polygon writes the texpage into the
persistent GPU state and into GPUSTAT bits 0-8 (`gpu.c:1111-1117`):

```c
gpu->texp_x = (poly.texp & 0xf) << 6;
gpu->texp_y = (poly.texp & 0x10) << 4;
gpu->texp_d = (poly.texp >> 7) & 0x3;
gpu->gpustat &= 0xfffffe00;
gpu->gpustat |= poly.texp & 0x1ff;
```

Rectangles read `gpu->texp_*` (`gpu.c:502-503`) and their transparency mode from GPUSTAT
(`gpu.c:465`), so **a sprite inherits the texpage of the last textured polygon**. This is
correct hardware behaviour, and it means the backend cannot treat polygons and sprites as
independent — texpage state is a shared, order-dependent register. Batching must not
reorder a sprite past a polygon that changes the texpage.

Quads are split into `(v0,v1,v2)` and `(v1,v2,v3)` (`gpu.c:1157-1162`, `1166-1171`).

**Rectangles** (`gpu_rect`, `gpu.c:1001-1065`). Size class is `(attrib >> 3) & 3`
(`gpu.c:456`) mapping to `RS_VARIABLE / RS_1X1 / RS_8X8 / RS_16X16` (`gpu.h:49-54`);
attribute flags are `RA_RAW / RA_TRANSP / RA_TEXTURED` (`gpu.h:56-60`).
Word count `1 + (size==variable) + textured` (`gpu.c:1017`).
Raw textured rects force the modulation colour to `0x808080` (`gpu.c:1041-1042`), which
makes the `(t*m)/128` modulation an identity — a neat trick the backend should keep.

**Lines** (`gpu_line`, `gpu.c:1180-1258`). Only the 2-point form is implemented.
Gouraud lines read two colours (`gpu.c:1236-1242`) but then discard `v1.c` and rasterize
flat with `BGR555(buf[0] & 0xffffff)` (`gpu.c:1252`). **Polylines are parsed but never
drawn** — `gpu.c:1193-1232` only scans for the `0x50005000` terminator and the entire
draw body is commented out. Lines are drawn with an integer Bresenham
(`plotLineLow`/`plotLineHigh`, `gpu.c:599-651`) that writes a raw `uint16_t` colour with
no dithering, no semi-transparency, and no modulation.

### 1.3 Texture handling

`gpu_fetch_texel` (`gpu.c:175-205`) is the single texel path.

*Texture window* (`gpu.c:176-179`):
```c
tx = (tx & ~gpu->texw_mx) | (gpu->texw_ox & gpu->texw_mx);
ty = (ty & ~gpu->texw_my) | (gpu->texw_oy & gpu->texw_my);
tx &= 0xff;  ty &= 0xff;
```
The window registers are stored pre-shifted by 3 at GP0(E2) (`gpu.c:1947-1952`):
`texw_mx = ((v>>0)&0x1f)<<3`, `texw_my = ((v>>5)&0x1f)<<3`,
`texw_ox = ((v>>10)&0x1f)<<3`, `texw_oy = ((v>>15)&0x1f)<<3`.
This is algebraically equivalent to the documented `(u AND NOT(mask*8)) OR ((off AND mask)*8)`
because `(a<<3)&(b<<3) == (a&b)<<3`. The final `& 0xff` is the 256×256 page wrap.

*Depths* (`gpu.c:181-204`):

| `depth` | Path | Lines |
|---|---|---|
| 0 — 4bpp CLUT | fetch `vram[(tpx + (tx>>2)) + (tpy+ty)*1024]`, nibble `(texel >> ((tx&3)<<2)) & 0xf`, then `vram[(clutx+index) + cluty*1024]` | 183-189 |
| 1 — 8bpp CLUT | fetch `vram[(tpx + (tx>>1)) + (tpy+ty)*1024]`, byte `(texel >> ((tx&1)<<3)) & 0xff`, then CLUT | 192-198 |
| 2/3 — 15bpp direct | `vram[(tpx + tx) + (tpy+ty)*1024]` | 201-203 |

Texpage/CLUT decode (`gpu.c:254-258` for polys, `gpu.c:467-468` for rects):
```c
tpx   = (texp & 0xf) << 6;      // 0..960, 64-px granularity
tpy   = (texp & 0x10) << 4;     // 0 or 256
clutx = (clut & 0x3f) << 4;     // 0..1008, 16-px granularity
cluty = (clut >> 6) & 0x1ff;    // 0..511
depth = (texp >> 7) & 3;
```

**Known deviation:** there is no horizontal wrap on `tpx + tx`. For a 15bpp page at
`tpx = 960` with `tx = 255`, the index is 1215, which silently reads into the *next VRAM
row* rather than wrapping to `x = 191` as hardware does. Same class of issue for 8bpp at
`tpx=960, tx=127`. Low-impact but the backend gets this right for free if it addresses
VRAM as a 2D texture with `x & 1023`.

**Texel 0x0000 is fully transparent** and is discarded before any blending —
`gpu.c:361-362` (polys), `gpu.c:506-507` (rects). This is the single most important
fragment-shader behaviour: `if (texel == 0) discard;`.

*Bilinear filtering* — `gpu_fetch_texel_bilinear` (`gpu.c:207-246`) is used
**unconditionally** by polygons at `gpu.c:359`, while rectangles point-sample at
`gpu.c:498`. So today polygons and sprites disagree about filtering, and neither matches
hardware (which always point-samples). The bilinear helper early-outs on the top-left tap
(`gpu.c:215-216`), so transparency is decided by one tap only, and it ORs the mask bits of
all four taps into the result (`gpu.c:245`).

### 1.4 Colour, modulation, and the 5-bit pipeline

`BGR555` (`gpu.c:30-33`) packs `0x00BBGGRR` → 16-bit `[14:10]=B [9:5]=G [4:0]=R`.
The rasterizer expands 5-bit back to 8-bit with `<<3` (`gpu.c:398-400`, `539-541`), i.e.
`31 → 248`, not 255. Round-tripping is exact; additive saturation clamps at 255
(`gpu.c:433-435`) which maps back to 31, so no visible error.

Texture modulation (`gpu.c:370-392`, `512-534`):
```c
c = (texel_component_8bit * vertex_component_8bit) / 128.0f;   // 0x80 == identity
clamp to [0,255]; round; then BGR555()
```

Gouraud interpolation is barycentric in float (`gpu.c:326-328`) using the edge functions
`z0/z1/z2` divided by `area` (`gpu.c:294`).

### 1.5 Semi-transparency

Four modes, identical code in both live rasterizers (`gpu.c:410-431` for polygons,
`gpu.c:550-571` for rects). `B` = destination VRAM texel, `F` = incoming fragment, both
expanded to 8-bit:

| Mode | Equation | Poly lines | Rect lines |
|---|---|---|---|
| 0 | `0.5*B + 0.5*F` | 411-415 | 551-555 |
| 1 | `B + F` | 416-420 | 556-560 |
| 2 | `B - F` | 421-425 | 561-565 |
| 3 | `B + 0.25*F` | 426-430 | 566-570 |

Result is clamped to `[0,255]` (`gpu.c:433-435`) and re-packed with `BGR555`
(`gpu.c:443`).

**Mode selection** (`gpu.c:262-266`):
```c
if (data.attrib & PA_TEXTURED) transp_mode = (data.texp >> 5) & 3;
else                           transp_mode = (gpu->gpustat >> 5) & 3;
```
Rectangles always use GPUSTAT (`gpu.c:465`) — correct, since the texpage bits were
latched into GPUSTAT at `gpu.c:1115-1116`.

**Per-texel STP bit** (`gpu.c:364-365`, `509-510`):
```c
if (data.attrib & PA_TRANSP) transp = (texel & 0x8000) != 0;
```
So within a single semi-transparent textured primitive, blending is decided *per
fragment* by bit 15 of the texel. This is the hard constraint on fixed-function blending
— see §2.5.

For `PA_RAW` textured primitives the texel is written through verbatim (`gpu.c:367-368`),
bit 15 included.

**Interaction with the mask bit: none.** The mask bit is not implemented (§1.7), so
today the STP bit is read from textures but never *written* to the framebuffer except
incidentally via the raw path.

### 1.6 Dithering

Kernel (`gpu.c:17-22`) — the standard PS1 4×4 matrix:
```
-4  0 -3  1
 2 -2  3 -1
-3  1 -4  0
 3 -1  2 -2
```

Applied at `gpu.c:330-336`, and **only inside `if (data.attrib & PA_SHADED)`**
(`gpu.c:325`). Three deviations from hardware, all of which matter for a GPU port:

1. **Index is bounding-box relative:** `dy = (y - ymin) & 3; dx = (x - xmin) & 3`
   (`gpu.c:330-331`). Hardware indexes by absolute VRAM coordinate (`x & 3`, `y & 3`).
   This makes the dither pattern *move with the primitive*, so two adjacent primitives
   covering a gradient get discontinuous dither. The dead `gpu_render_shaded_triangle`
   has the same bug at `gpu.c:882-883`, plus a commented-out experiment at
   `gpu.c:885-889` that shows the author was already suspicious.
2. **Applied before modulation:** dither is added to the interpolated vertex colour
   (`gpu.c:334-336`) and then that dithered colour modulates the texel
   (`gpu.c:374-380`). Hardware dithers the *final* modulated colour.
3. **Not gated on GPUSTAT bit 9.** GP0(E1) stores bits 0-10 into GPUSTAT
   (`gpu.c:1941-1942`) so the dither-enable bit *is* captured, but nothing ever reads it.
   Flat and textured-flat primitives are never dithered; Gouraud ones always are.

Rectangles are never dithered (`gpu_render_rect` has no dither code) — this one *is*
correct: hardware does not dither sprites.

### 1.7 Mask bit — not implemented

`GP0(E6)` is an empty stub:
```c
case 0xe6: {
    /* To-do: Implement mask bit thing */      // gpu.c:1965-1967
} break;
```
There is no "set mask on draw" and no "check mask before draw" anywhere. Grepping for
`0x8000` in `gpu.c` yields only texture-STP reads (`245`, `365`, `510`) and the unrelated
GPUSTAT display-disable bit `0x800000` (`65`, `2199`).

**Consequence for the port:** the HW backend must implement the mask bit *from scratch*,
and doing so will change output versus the software renderer. That is a deliberate
divergence and must be behind a flag during Stage 1 parity testing. See §7.4.

### 1.8 Clipping, offset, and coordinate handling

*Drawing area* — GP0(E3)/(E4) at `gpu.c:1953-1960`:
`draw_x1/y1` = `(v>>0)&0x3ff`, `(v>>10)&0x1ff`; `draw_x2/y2` likewise.
Tested **per pixel, inclusive on both ends** (`gpu.c:298-299`, `489-490`, `611-612`,
`638-639`).

*Drawing offset* — GP0(E5), signed 11-bit (`gpu.c:1961-1964`):
```c
off_x = ((int32_t)(((buf[0] >> 0 ) & 0x7ff) << 21)) >> 21;
off_y = ((int32_t)(((buf[0] >> 11) & 0x7ff) << 21)) >> 21;
```
Added to vertices at `gpu.c:279-284` (polys) and `gpu.c:471-472` (rects).

*Rect coordinate quirk:* `gpu_render_rect` adds the offset and *then* re-sign-extends to
11 bits (`gpu.c:471-474`), then clamps the box to ±1024 (`gpu.c:480-483`). The double
`SE10` (once at parse time, `gpu.c:1033-1034`, once after offset) is a real behavioural
detail the backend must replicate if it wants parity.

*Primitive size rejection* (`gpu.c:291-292`):
```c
if (((xmax - xmin) > 2048) || ((ymax - ymin) > 1024)) return;
```
Hardware rejects at >1023 horizontal / >511 vertical. This core's thresholds are 2× too
permissive, so some primitives hardware would drop are drawn here.

*Fill rule and winding* — vertices are swapped to enforce positive area
(`gpu.c:271-277`), then a top-left rule is applied via the `TL` macro (`gpu.c:248-249`):
```c
#define TL(z, a, b) ((z < 0) || ((z == 0) && ((b.y > a.y) || ((b.y == a.y) && (b.x < a.x)))))
```
Loop bounds are half-open (`gpu.c:296-297`: `y < ymax`, `x < xmax`). GL and Vulkan both
use a top-left fill rule in practice, so this maps naturally — see §3.3.

### 1.9 VRAM transfer commands

| Command | Lines | Behaviour and quirks |
|---|---|---|
| GP0(02) fill | 1804-1857 | `x &= 0x3f0`, `y &= 0x1ff`, `w = ((w & 0x3ff) + 0x0f) & ~0xf`, `h &= 0x1ff` (`gpu.c:1821-1824`). Ignores drawing area — the check is commented out at `gpu.c:1842-1846` with the note "This shouldn't be needed", which is correct: hardware fills ignore clipping *and* the mask bit. Writes raw `BGR555(color)`. |
| GP0(80) VRAM→VRAM | 1859-1891 | Naive nested copy (`gpu.c:1877-1885`). **No wrapping** — out-of-range coordinates are dropped by a bounds test (`gpu.c:1879-1880`) rather than wrapped at 1024/512. **No overlap handling** — a self-overlapping copy will read already-written data. Hardware wraps and reads coherently. |
| GP0(A0) CPU→VRAM | 1260-1325 | Size normalised `((n-1) & mask) + 1` (`gpu.c:1276-1277`), total halfwords rounded even (`gpu.c:1278`). Writes two halfwords per 32-bit word with correct `& 0x3ff` / `& 0x1ff` wrapping (`gpu.c:1286-1314`). Ignores mask bit and drawing area — correct. |
| GP0(C0) VRAM→CPU | 1776-1802 + 93-140 | `gpu_cmd_c0` only sets up `c0_addr/c0_xsiz/c0_ysiz/c0_tsiz`; the actual read drains through `psx_gpu_read32` offset 0 (`gpu.c:98-118`), two halfwords per read. Note the read path indexes `vram[c0_addr + (c0_xcnt + c0_ycnt*1024)]` with **no wrapping** on `c0_xcnt`. |

`psx_gpu_read32` also multiplexes the GP1(10h) register-readback responses
(`gpu.c:120-137`) into the same port, overwriting `data` if a query is pending — a
pre-existing bug (a pending C0 transfer and a pending 10h query collide), worth noting
but out of scope.

### 1.10 Display / CRTC

Register writes, all in `psx_gpu_write32` GP1 (`gpu.c:2008-2058`):

| GP1 | Lines | Stored |
|---|---|---|
| 03 display enable | 2013-2018 | GPUSTAT bit 23 |
| 04 DMA direction | 2019-2020 | **no-op** |
| 05 display start | 2021-2025 | `disp_x = v & 0x3ff`, `disp_y = (v>>10) & 0x1ff` |
| 06 horizontal range | 2026-2030 | `disp_x1 = v & 0xfff`, `disp_x2 = (v>>12) & 0xfff` |
| 07 vertical range | 2031-2035 | `disp_y1 = v & 0x1ff`, `disp_y2 = (v>>10) & 0x1ff` |
| 08 display mode | 2036-2047 | `display_mode = v & 0xffffff`, fires `GPU_EVENT_DMODE` |
| 10 register query | 2049-2052 | `gp1_10h_req` |

**Missing entirely:** GP1(00) reset, GP1(01) reset command buffer, GP1(02) IRQ ack,
GP1(09) texture disable, and GP0(1F) IRQ request. GPUSTAT's DMA/ready bits are faked
unconditionally by `return gpu->gpustat | 0x1c000000` (`gpu.c:141`).

`display_mode` bit meanings, per how the frontend decodes them in `psx/psx.c`:

| Bit | Meaning | Consumed where |
|---|---|---|
| 0-1 | hres1 → `{256,320,512,640}` | `psx_get_dmode_width` `psx.c:72-82` |
| 2 | vres 480 | `psx_get_dmode_height` `psx.c:84-94` |
| 3 | PAL | `psx_gpu_is_pal_mode` `gpu.h:175-177` |
| 4 | 24bpp | `psx_get_display_format` `psx.c:68-70` |
| 5 | **interlace** | **never read anywhere** |
| 6 | hres2 → 368 | `psx.c:77-78` |

`psx_get_display_width` (`psx.c:55-62`) rewrites 368 → 384. `psx_get_dmode_height`
(`psx.c:84-94`) derives height from `disp_y2 - disp_y1` with a 240 fallback.

Scanout is just a pointer into VRAM — `psx_gpu_get_display_buffer` (`gpu.c:2198-2203`):
```c
if (gpu->gpustat & 0x800000) return gpu->empty;   // display disabled → black
return gpu->vram + (gpu->disp_x + (gpu->disp_y * 1024));
```
The frontend then uploads `width × height` rows at `PSX_GPU_FB_STRIDE` (2048 bytes,
`gpu.h:18`) as `SDL_PIXELFORMAT_BGR555`, or as `SDL_PIXELFORMAT_RGB24` when
`psx_get_display_format` reports 24bpp (`frontend/main.cpp:2054`), with a fallback to
uploading the whole VRAM when the display window would run off the bottom
(`main.cpp:2055`).

Timing: `psx_gpu_update` (`gpu.c:2171-2196`) advances a float cycle counter and derives
hblank edges; `gpu_hblank_event` (`gpu.c:2089-2169`) toggles GPUSTAT bit 31 on odd lines
during vdraw (`gpu.c:2094-2098`) and fires `GPU_EVENT_VBLANK` at line 240/288
(`gpu.c:2134-2155`). **`GPU_EVENT_VBLANK` is the only frame boundary the backend can
hook.**

---

## 2. GPU pipeline design

### 2.1 Resources

| Resource | Format | Size | Purpose |
|---|---|---|---|
| `vram_rt` | RGBA8 (colour) + S8 (stencil) | `1024*S × 512*S` | the render target; the upscaled VRAM |
| `vram_read` | `R16UI` (GLES3 core) | `1024 × 512` | native-resolution VRAM, sampled for textures and CLUTs |
| `vram_shadow` | host `uint16_t[512K]` | — | `gpu->vram`, still authoritative for CPU reads |
| `display_tex` | RGBA8 | `1024*S × 512*S` view | scanout source (a view/region of `vram_rt`) |

**Why a separate `vram_read` texture:** GLES3 forbids sampling the texture you are
rendering to. Textures live *in* VRAM, so every textured primitive samples the same
memory it may be writing. DuckStation solves this with a "VRAM read texture" that is a
native-resolution copy, refreshed on demand. We do the same. The refresh trigger is a
*texture-cache invalidation* raised by any write that lands in a region a later primitive
samples (§4.3).

`R16UI` is the right format for `vram_read`: it is colour-renderable and texture-filterable-
exempt in GLES 3.0, `texelFetch` works, and it lets the shader do exact integer bit
extraction with no float round-trip. `RGBA5551` would be tempting but sampling it as a
normalized float loses the ability to index a CLUT exactly.

### 2.2 Vertex format

One format for all primitives — sprites and lines are expanded to two triangles on the
CPU side, exactly as `gpu_poly` already splits quads (`gpu.c:1157-1162`).

```c
typedef struct psx_hw_vertex {
    float    x, y;        /* native VRAM pixel coords, drawing offset ALREADY applied */
    uint32_t color;       /* 0x00BBGGRR, as gpu.c stores it                            */
    uint16_t u, v;        /* native texel coords, 0..255                               */
    uint16_t texpage;     /* raw 16-bit texpage word (gpu.c:1106)                      */
    uint16_t clut;        /* raw 16-bit CLUT word   (gpu.c:1105)                       */
} psx_hw_vertex_t;        /* 20 bytes */
```

`x, y` are `float` rather than `int16_t` deliberately: it costs nothing today and is the
hook PGXP-style precision needs later (§6.6). At Stage 1 they always hold exact integers.

Per-*draw* (not per-vertex) uniforms: drawing area, texture window, semi-transparency
mode, dither enable, mask-bit set/check, resolution scale. Texpage and CLUT are
per-vertex so that a batch can span texpage changes without a state break — the fragment
shader reads them as flat-interpolated integers.

### 2.3 Vertex shader responsibilities

1. Scale to the upscaled render target: `pos * scale`.
2. Map to clip space over the full 1024×512 VRAM:
   `clip.x = pos.x*scale / (512.0*scale) - 1.0`, `clip.y = 1.0 - pos.y*scale / (256.0*scale)`.
   (Equivalently, set the viewport to the whole RT and divide by `1024*scale`/`512*scale`.)
3. Pass through colour, UV, texpage, CLUT as `flat` integers where they must not be
   interpolated (texpage, CLUT) and smooth where they must (colour, UV).
4. `gl_Position.z = 0`, `w = 1` — no depth, no perspective. (See §6.6 for why `w` becomes
   interesting later.)

### 2.4 Fragment shader responsibilities

In order, mirroring `gpu_render_triangle` (`gpu.c:322-446`):

1. **Texture window** — `u = (u & ~mask_u) | (off_u & mask_u)`, `& 0xff`
   (mirrors `gpu.c:176-179`).
2. **Texel fetch** — decode `texpage` into `tpx/tpy/depth` exactly as `gpu.c:254-258`,
   then:
   - 4bpp: `texelFetch(vram, ivec2((tpx + (u>>2)) & 1023, tpy + v))`, nibble select,
     CLUT fetch at `((clutx + index) & 1023, cluty)`.
   - 8bpp: same with `>>1` and byte select.
   - 15bpp: direct fetch.
   Note the `& 1023` — this *fixes* the wrap bug from §1.3 for free.
3. **`if (texel == 0u) discard;`** — mirrors `gpu.c:361-362`.
4. **STP capture** — `bool stp = (texel & 0x8000u) != 0u;`
5. **Modulate** unless `PA_RAW`: `c = (texel5 * 8 * vcolor8) / 128`, clamp — mirrors
   `gpu.c:370-392`.
6. **Dither** — see §2.7.
7. **Truncate to 5 bits per channel** unless "true colour" enhancement is on. The
   software renderer truncates implicitly via `BGR555` at `gpu.c:392/395/443`; the HW
   path must do it explicitly or upscaled output will drift from native.
8. **Semi-transparency scaling + alpha emission** — see §2.5.
9. **Mask bit** — see §2.6.

### 2.5 Semi-transparency mapping

All four modes map to fixed-function blending with **one blend func and two blend
equations**, provided the fragment shader pre-scales `F` and emits the right alpha:

| Mode | Target equation | Shader emits | GL blend state |
|---|---|---|---|
| 0 | `0.5B + 0.5F` | `rgb = F*0.5`, `a = 0.5` | `EQ_ADD`, `(ONE, SRC_ALPHA)` |
| 1 | `B + F` | `rgb = F`, `a = 1.0` | `EQ_ADD`, `(ONE, SRC_ALPHA)` |
| 3 | `B + 0.25F` | `rgb = F*0.25`, `a = 1.0` | `EQ_ADD`, `(ONE, SRC_ALPHA)` |
| 2 | `B - F` | `rgb = F`, `a = 1.0` | `EQ_REVERSE_SUBTRACT`, `(ONE, SRC_ALPHA)` |

`(ONE, SRC_ALPHA)` means `result = src*1 + dst*src_alpha`. For mode 0 that is
`0.5F + 0.5B` ✓; modes 1 and 3 give `F' + B` ✓; mode 2 under reverse-subtract gives
`B*1 - F` ✓.

**Per-texel STP within one primitive.** This is the real problem (`gpu.c:364-365`).
Opaque texels inside a semi-transparent textured primitive must be written *unblended*.
Two mechanisms:

- **ADD-family (modes 0, 1, 3):** emit `a = 0.0` and un-scaled `rgb = F` for opaque
  texels. `F + B*0 = F` — exactly a plain write. **Single pass, no state change, no
  discard.** This is the fast path and covers three of four modes.
- **Mode 2 (reverse subtract):** `B*0 - F = -F`, clamped to 0. There is no alpha value
  that makes reverse-subtract an identity write. Mode 2 therefore needs a **two-pass
  split** of the primitive:
  1. pass A: blending **off**, `discard` fragments where `stp == true`;
  2. pass B: blending on with `EQ_REVERSE_SUBTRACT`, `discard` where `stp == false`.
  Both passes use the same vertex buffer and the same shader with a `u_stp_pass` uniform,
  so the cost is one extra draw call per mode-2 batch, not per pixel.

  Alternative when available: `GL_EXT_shader_framebuffer_fetch` lets the shader read
  `gl_LastFragData` and do all four equations programmably in one pass, with no
  fixed-function blending at all. **Recommend keeping this behind a runtime feature flag,
  not enabling it by default on GLES.** Prior ARMSX2 experience is directly relevant:
  Mali drivers take a per-primitive tile-flush when framebuffer fetch is unavailable but
  software blending is enabled, and some Adreno GLES drivers reject two `inout` fbfetch
  attachments. Probe the extension, gate by `driverID`/renderer string, and always keep
  the two-pass path as the fallback.

- **Untextured semi-transparent primitives** have no STP bit; `transp` is constant for the
  whole primitive (`gpu.c:259`), so they always take the single-pass path including mode 2.

**Batching rule:** semi-transparency mode is fixed-function state, so a batch breaks when
the mode changes. Sort/segment the per-frame command stream into runs of identical
(blend mode, pass) and issue one draw per run. Do **not** reorder across the runs —
`B - F` and `B + F` are order-dependent, and so is the texpage latch (§1.2).

### 2.6 Mask bit

> **SUPERSEDED — see §0.5.16.** The mask bit is implemented, in both CPU rasterizers
> (§0.5.12) and in the GLES one, and it is **not** stencil work. The plan below is kept
> because its analysis of the *problem* is right and the alpha-channel conflict it names is
> exactly what had to be resolved; its **conclusion is wrong on two counts**. Stencil's
> reference value is per-DRAW while GP0(E6) changes between primitives, so it would break the
> batch constantly; and nothing in GLES 3.0 can read a stencil buffer back into `vram_tex` or
> into GP0(C0), so bit 15 could never leave the FBO. The shipped design gives alpha to the
> mask bit and moves the blend into the shader (framebuffer fetch), which the CHECK half needs
> regardless. Read this section for the reasoning, §0.5.16 for what exists.

Because the software renderer does not implement it at all (§1.7), this is net-new
behaviour. Map it to **stencil**, not to the alpha channel:

- Render target gets a `DEPTH24_STENCIL8` (or `STENCIL_INDEX8`) attachment. Stencil bit 0
  mirrors VRAM bit 15.
- **"Check mask before draw"** (GP0(E6) bit 1): `glStencilFunc(EQUAL, 0, 1)` — fragments
  are killed where the destination mask bit is set. This is exact, per-fragment, and costs
  nothing. Easy half.

- **"Set mask while drawing"** (GP0(E6) bit 0) is the awkward half. The bit written is
  `force_mask || texel_bit15`, i.e. it **varies per fragment**, but `glStencilFunc`'s
  reference value is per-draw. Three options:

  | Option | Cost | Verdict |
  |---|---|---|
  | Split each draw into two stencil-ref passes (like the mode-2 split, §2.5) | +1 draw per batch | correct, predictable |
  | Carry the mask bit in the render target's **alpha channel** and reconcile on download | free at draw time | **conflicts with blending — see below** |
  | `GL_EXT_shader_framebuffer_fetch` and do it all programmably | driver-gated | fallback only |

  **The alpha-channel conflict, stated plainly:** §2.5 already spends the alpha channel as
  the fixed-function `SRC_ALPHA` blend factor. A fragment cannot emit both a blend factor
  and a mask bit in one `.a`. The resolution:

  - **Opaque fragments** (not blending): `.a` carries the mask bit, and
    `glBlendFuncSeparate(ONE, SRC_ALPHA, ONE, ZERO)` stores it unmodified.
  - **Blending fragments**: `.a` must carry the blend factor, so the mask bit is written by
    the **stencil op alone**, and the alpha-channel copy for those pixels is stale.

  Therefore `download_vram` cannot trust alpha alone. It must either read the stencil
  buffer as well, or — simpler and recommended — **treat stencil as the single source of
  truth for the mask bit** and drop the alpha copy entirely, accepting the two-pass split
  for the varying-ref case. The alpha optimisation is only worth revisiting if profiling
  shows the extra draws matter.

  The drafted fragment shader carries the alpha-copy version with this conflict commented
  inline, so whoever implements it has to make the call consciously rather than discover
  it at debug time.
- VRAM upload (§4) must restore stencil from bit 15 of the uploaded data; VRAM download
  must recombine `alpha → bit 15`.
- GP0(02) fill and GP0(A0) upload **ignore** the mask bit; GP0(80) copy respects "set
  mask" on the destination. Keep those as explicit non-stencil-tested operations.

Implementation note: implementing GP0(E6) also requires adding the two GPUSTAT bits (11
and 12) that `gpu.c:1965-1967` currently never sets, so the software path and the HW path
agree on what the game asked for.

### 2.7 Dithering under upscaling

The classic mistake is to index the dither matrix by the *upscaled* fragment coordinate,
which produces a 4-pixel dither pattern at every resolution and therefore visible
high-frequency noise that gets worse as `S` grows, plus temporal shimmer.

Correct: index by the **native** VRAM coordinate.

```glsl
ivec2 native = ivec2(gl_FragCoord.xy) / u_scale;
int d = u_dither[(native.x & 3) + ((native.y & 3) << 2)];
```

This makes the dither pattern scale-invariant: at `S=4` each dither cell is a 4×4 block of
output pixels, matching what native output looks like when magnified, which is what users
expect from an upscaler.

Two deliberate choices to record:

- **Parity mode** must reproduce the bounding-box-relative index (`gpu.c:330-331`). That
  requires passing `(xmin, ymin)` of the primitive as a per-draw uniform — cheap, but it
  breaks batching across primitives, so parity mode is a debug/test mode only and is
  expected to be slow. Recommend: implement absolute indexing (correct) and accept the
  Stage-1 parity test failing on Gouraud dither, documenting it as a *known intentional
  divergence* rather than contorting the batcher.
- Gate on GPUSTAT bit 9, which `gpu.c:1941-1942` already latches but never reads. Also
  gate off for raw-textured primitives and for sprites (hardware never dithers sprites,
  and `gpu_render_rect` correctly does not).
- **Enhancement:** at `S > 1`, offering "disable dithering" is legitimate — the dither
  exists to hide 5-bit banding on a 320×240 CRT and at 4× on an 8-bit panel it is pure
  noise. Make it a setting, default *on* to preserve intent.

### 2.8 Lines

`gpu_render_flat_line` (`gpu.c:669`) uses integer Bresenham. On the GPU, expand each line
to a quad of width `1 * S` oriented along the line, or use `GL_LINES` with
`glLineWidth(S)` — the latter is unreliable (GLES 3.0 only guarantees width 1) so **expand
to a quad**. Because the software path applies neither dithering nor semi-transparency to
lines, Stage 1 can render them with a trivial flat shader; matching Bresenham's exact
pixel set at `S=1` is achievable by expanding to a quad with the correct half-pixel
offsets, but is genuinely fiddly. Recommend accepting a small divergence on diagonal
lines and noting it (lines are rare — mostly wireframe debug output and a few racing-game
HUDs).

---

## 3. The upscaling design

Let `S` be the integer resolution scale (1, 2, 3, 4, …, capped by
`GL_MAX_TEXTURE_SIZE` — `1024*S ≤ max`, so `S ≤ 8` on a 8192 limit, `S ≤ 16` on 16384).

### 3.1 What scales and what must not

| Quantity | Scales by S? | Why |
|---|---|---|
| Vertex X/Y positions | **Yes** | this is the whole point |
| Render target dimensions | **Yes** | `1024S × 512S` |
| Viewport / scissor (drawing area) | **Yes** | `draw_x1*S .. (draw_x2+1)*S - 1` |
| Drawing offset | **Yes** (applied before scaling) | it is a position |
| Texture UVs | **No** | texels are native; sampling stays 1:1 |
| Texpage / CLUT coordinates | **No** | VRAM addressing is native |
| Texture window mask/offset | **No** | operates in native texel space |
| `vram_read` texture | **No** | 1024×512 always |
| Dither matrix index | **No** (divide by S) | §2.7 |
| GP0(02) fill rectangle | **Yes** (as a scaled clear) | it is a screen-space op |
| GP0(80) VRAM→VRAM copy | **Yes** (blit at scale) | it copies rendered content |
| GP0(A0) CPU→VRAM upload | **No** → then replicate S× | source data is native |
| GP0(C0) VRAM→CPU download | **No** → must downsample | CPU expects native |
| Display/scanout rectangle | **Yes** | reads from the upscaled RT |

The rule of thumb: **anything derived from a coordinate the game gave as a screen position
scales; anything that addresses VRAM as memory does not.**

### 3.2 Seams between adjacent primitives

Three separate causes, three separate fixes:

1. **Rasterization gaps.** If two triangles share an edge with *identical* vertex
   coordinates, a top-left fill rule guarantees no gap and no double-cover, at any scale,
   because the scaled coordinates `x*S` remain exact. This is why positions must be
   scaled by an *integer* and fed as exact values — never `x * (float)S` where `S` came
   from a float setting, and never a fractional scale. **Fractional resolution scales are
   out of scope**; they reintroduce cracks.
2. **UV bleeding at magnification.** At `S>1` a fragment's interpolated UV can land on a
   neighbouring texel that belongs to a different sprite in the same texture page,
   producing a 1-texel halo. This is *the* classic upscaling artifact. Mitigations, in
   increasing cost: (a) point sampling only — the default, which mostly avoids it;
   (b) clamp interpolated UVs to the primitive's own UV bounding box (compute
   `min/max` UV per primitive on the CPU, pass as a per-draw uniform, clamp in the
   fragment shader) — this is cheap and very effective; (c) full "texture window"-aware
   clamping. Recommend (b) from Stage 4 onward.
3. **The PS1's own integer vertices.** The console had no subpixel precision; adjacent
   triangles in a model often have coordinates that differ by a rounded pixel, so the seam
   exists in the source data. Upscaling *magnifies* it. This cannot be fixed in the
   rasterizer — it requires PGXP-class work on the GTE side (§6.6, §7.6).

### 3.3 Fill rule and the half-pixel question

Map native integer coordinate `n` to render-target coordinate `n*S`, and rasterize with
the standard GL convention that fragment centres sit at `+0.5`. A native pixel `n` then
covers RT pixels `[n*S, n*S + S - 1]`, whose centres are `n*S + 0.5 … n*S + S - 0.5`, all
strictly inside `[n*S, (n+1)*S)`. So a primitive spanning native `[a, b)` covers exactly
`(b-a)*S` RT columns. No half-pixel fudge is needed — **do not add one**; adding `+0.5*S`
is a common bug that shifts everything by half a native pixel at `S=2` and is invisible at
`S=1`, which is exactly when it gets merged.

GL does not *mandate* a top-left fill rule, but every desktop and mobile GPU implements
one (it falls out of the standard fixed-point edge test). Vulkan mandates it outright.
Matching `gpu.c:248-249` therefore requires no work. Verify it in the Stage 1 parity test
rather than assuming.

### 3.4 Keeping 2D/UI crisp

Sprites and untextured rects are the UI. Two things keep them sharp:

1. **Point sampling by default** for all texture fetches. Do not enable bilinear on the
   `vram_read` texture — and note that the *software* renderer's unconditional bilinear
   on polygons (`gpu.c:359`) is the odd one out here, not the HW path.
2. **No resampling on present.** The final blit from `vram_rt` to the swapchain should be
   nearest when the output size is an integer multiple of `display_width * S`, and
   otherwise a mild sharp filter. The present path already distinguishes nearest and
   linear 555 decode (`frontend/render_gl.cpp:259`, `:270`), so this seam exists.

A useful refinement once the basics work: because sprites are always axis-aligned and
integer-sized, they can be rendered with UVs that are *exactly* `S`-replicated, which is
pixel-perfect magnification. That is automatic with point sampling; no special case
needed. It is worth an explicit test though — a 1×1 sprite at `S=4` must produce a 4×4
block of one colour, not a 4×4 block sampling four different texels.

### 3.5 What upscaling does NOT fix

State plainly, because users will ask:

- Texture resolution. Textures stay 256×256 4bpp; upscaling magnifies them.
- The PS1's affine (non-perspective-correct) texture mapping. Upscaling makes the warping
  *more* visible, not less. Fixing it requires PGXP.
- Polygon jitter/wobble from integer vertices. Same — PGXP territory.
- Anything that reads VRAM back (§4.6).

---

## 4. VRAM coherency

This is the hard part, and this codebase makes it harder than usual because
`gpu->vram` is a plain `uint16_t*` that is read and written directly from at least five
places with no accessor to intercept.

### 4.1 The two copies

- `gpu->vram` (host, `gpu.c:59`) — authoritative for **CPU-visible** VRAM: GPUREAD
  drains (`gpu.c:98-118`), the display buffer pointer (`gpu.c:2202`), and the frontend's
  fallback whole-VRAM upload (`main.cpp:2055`).
- `vram_rt` (GPU, `1024S × 512S`) — authoritative for **rendered** content.

They diverge the moment the first primitive is drawn on the GPU. The strategy is
**lazy, region-tracked, one-way-at-a-time ownership**, never a per-frame round trip.

### 4.2 Dirty tracking

Maintain two coarse region descriptors, both as a bounding rectangle plus a 64×32 tile
bitmap over VRAM (16×16 native texels per tile → 2048 bits, one cache line's worth of
work to test):

- `gpu_dirty` — regions the GPU has drawn into that the host copy does not have.
- `cpu_dirty` — regions the host has written that the GPU copy does not have.

A bounding rectangle alone is not enough: a game that draws its framebuffer at
`(0,0)-(320,240)` and its textures at `(320,256)-(640,512)` would produce a bounding box
covering all of VRAM and force a full sync every frame. The tile bitmap is what makes the
common case cheap.

### 4.3 The five sync triggers

| Trigger | Site | Action |
|---|---|---|
| **Texture sampled from a `gpu_dirty` region** | before any textured draw | flush pending batch, download those tiles from `vram_rt` → `vram_read` (GPU→GPU, no CPU stall) |
| **CPU→VRAM upload, GP0(A0)** | `gpu.c:1285-1323` | write to `gpu->vram` as today, mark `cpu_dirty`; upload to `vram_rt` (replicated S×) and `vram_read` at flush time |
| **VRAM→CPU download, GP0(C0)** | `gpu.c:1776-1802` setup, `gpu.c:98-118` drain | **the stall.** If the requested rect intersects `gpu_dirty`, flush and do a real `glReadPixels`/PBO readback, downsampled to native, into `gpu->vram`, then clear those tiles from `gpu_dirty` |
| **VRAM→VRAM copy, GP0(80)** | `gpu.c:1859-1891` | stays entirely on the GPU: flush, then blit `src*S → dst*S` within `vram_rt`. Also apply to `gpu->vram` so the host copy stays valid if it was already coherent; if `src` was `gpu_dirty`, mark `dst` `gpu_dirty` too |
| **Fill, GP0(02)** | `gpu.c:1804-1857` | scissored clear on `vram_rt` at scale; also write `gpu->vram` |

The critical property is that **only GP0(C0) forces a GPU→CPU readback**, and most games
never issue it during gameplay.

### 4.4 Scanout without a round trip

Today the frontend calls `psx_gpu_get_display_buffer` (`gpu.c:2198`) and uploads a
`width × height` window at 2048-byte stride (`main.cpp:2109-2145`). With a HW backend,
**do not download the framebuffer to feed that path** — that would be a full round trip
every frame and would erase the entire benefit.

Instead: the backend exposes `get_display_texture()` returning the `vram_rt` region
`(disp_x*S, disp_y*S, width*S, height*S)`, and the present layer samples it directly.
`frontend/render_gl.cpp` already owns a GL context and a fullscreen-quad present
(`OpPresent`, `render_gl.cpp:771`), so the change is: when a HW backend is active, bind
its texture instead of the uploaded streaming texture and skip `OpUploadFrame` entirely.
That is a small, additive change to `render.h` (one new entry point) and does not touch
the software path.

**24bpp display** (`display_mode` bit 4, `psx.c:68-70`): the scanout reinterprets VRAM
bytes as packed RGB888, so 2 VRAM texels ≈ 1.5 pixels. At `S>1` the upscaled RT does not
contain byte-addressable 24bpp data. Correct handling: **24bpp display always reads from
the native `vram_read` texture, never the upscaled RT**, and the 24bpp decode happens in
the present shader. Since 24bpp is only used for FMV playback (which the game uploaded via
GP0(A0) at native resolution anyway) this loses nothing. Call this out explicitly — it is
a real "why is my FMV not upscaled" question and the answer is "it never could be".

**Interlace** (`display_mode` bit 5) is currently ignored everywhere in this core
(§1.10). The HW backend should not try to fix that; it inherits the existing behaviour.
If interlace is implemented later, the standard approach is to render both fields into
the full-height RT and let scanout pick rows, which upscaling handles naturally.

### 4.5 Flush points

The batcher accumulates vertices into a growable array and flushes on:

1. `GPU_EVENT_VBLANK` (`gpu.c:2152-2155`) — the frame boundary.
2. Any blend-mode / mask-mode / stencil-state change (§2.5).
3. Drawing-area change, GP0(E3)/(E4) (`gpu.c:1953-1960`) — it is scissor state.
4. Texture-cache invalidation (§4.3 row 1).
5. Any of the four VRAM transfer commands.
6. Vertex buffer full.

Everything else batches. In practice a PS1 frame is 200–2000 primitives, so a well-behaved
game should land in 10–50 draw calls.

### 4.6 The fallback path for VRAM-abusing games

Some games read VRAM back constantly (framebuffer effects, software-composited transitions,
"grab the screen and warp it"). For these, every frame incurs a full round trip and the
HW backend is *slower* than software while also being less accurate.

Provide an explicit escape hatch, in this order of preference:

1. **Automatic downgrade.** Count GP0(C0) readback bytes per frame. If the running average
   over 60 frames exceeds a threshold (say 256 KB/frame ≈ a quarter of VRAM), log once and
   transparently fall back to the software rasterizer for the rest of the session,
   downloading `vram_rt` once to seed `gpu->vram`. This is more useful than it sounds
   because the alternative is a user filing a "the fast renderer is slow" bug.
2. **A per-game override** in config: `renderer = "software"` forced.
3. **A "software renderer for readbacks only" hybrid** — keep both rasterizers live,
   render everything twice. Correct, halves nothing, doubles CPU cost. Only worth it if a
   specific important game needs it; do not build it speculatively.

Be honest in the docs: DuckStation has the same problem and solves it with the same
combination of readback-on-demand plus a software fallback. There is no clever trick here.

---

## 5. Integration plan against this codebase

### 5.1 Widening the existing seam

`gpu.h:82-97` already has the shape:
```c
typedef void (*psx_gpu_render_triangle_t)(psx_gpu_t*, vertex_t, vertex_t, vertex_t, poly_data_t, int);
typedef struct { psx_gpu_render_triangle_t render_triangle; } psx_gpu_renderer_t;
```
Replace `psx_gpu_renderer_t` with the fuller `psx_gpu_backend_t` in
`frontend/gpu_hw_gl/gpu_hw.h` (drafted alongside this document). The `psx_gpu_t` gains one
pointer:
```c
struct psx_gpu_backend* backend;   /* NULL => software, current behaviour */
```
`NULL` must mean "software", so every hook is a two-line guarded call and the software
path is bit-identical when no backend is installed.

### 5.2 Hook sites, function by function

| # | Site | Lines | Hook |
|---|---|---|---|
| 1 | `gpu_poly` dispatch | 1155-1172 | already indirect; route to `backend->draw_poly(be, &poly)` passing the whole `poly_data_t` (not pre-split triangles — the backend wants the quad to emit 4 vertices as 2 triangles itself, and needs `attrib` for the raw/transp/shaded flags) |
| 2 | `gpu_rect` dispatch | 1059 | `if (be) be->draw_rect(be, &rect); else gpu_render_rect(...)` |
| 3 | `gpu_line` dispatch | 1252 | `if (be) be->draw_line(be, v0, v1, color); else gpu_render_flat_line(...)` |
| 4 | GP0(02) fill | 1839-1851 | `if (be) be->fill_vram(be, x, y, w, h, color);` — keep the host write too |
| 5 | GP0(80) copy | 1877-1885 | `if (be) be->copy_vram(be, sx, sy, dx, dy, w, h);` |
| 6 | GP0(A0) upload | 1289, 1304 | accumulate the transfer, and at completion (`gpu.c:1318`, `!gpu->tsiz`) call `be->upload_vram(be, xpos, ypos, xsiz, ysiz, src)` once for the whole rect rather than per halfword |
| 7 | GP0(C0) setup | 1785-1797 | `if (be) be->download_vram(be, x, y, w, h, gpu->vram + addr);` **before** the drain begins, so `psx_gpu_read32` reads valid host data with no further changes |
| 8 | GP0(E1) texpage | 1940-1946 | `be->invalidate_texture_cache(be)` is *not* needed here (texpage is per-vertex), but GPUSTAT bits 5-6 and 9 change blend/dither state → `be->set_draw_state(be, gpustat)` |
| 9 | GP0(E2) texture window | 1947-1952 | `be->set_texture_window(be, mx, my, ox, oy)` — batch break |
| 10 | GP0(E3)/(E4) drawing area | 1953-1960 | `be->set_drawing_area(be, x1, y1, x2, y2)` — batch break (scissor) |
| 11 | GP0(E5) drawing offset | 1961-1964 | `be->set_drawing_offset(be, off_x, off_y)`; the offset can stay CPU-side (applied to vertices before submission, as `gpu.c:279-284` does today) which avoids a batch break |
| 12 | GP0(E6) mask bits | 1965-1967 | currently a stub — implement: set GPUSTAT bits 11/12 and `be->set_mask_bits(be, set, check)` |
| 13 | `GPU_EVENT_VBLANK` | 2152-2155 | `be->end_frame(be)` — flush |
| 14 | `psx_gpu_get_display_buffer` | 2198-2203 | leave alone; add a *parallel* `psx_gpu_get_display_texture()` that returns non-NULL only when a backend is installed |
| 15 | `psx_gpu_init` | 71-73 | `gpu->backend = NULL;` (software default) |
| 16 | `psx_gpu_destroy` | 2205-2208 | `if (gpu->backend) gpu->backend->destroy(gpu->backend);` |

Sites 1–3 are the only ones that need care about the *existing* `USE_HARDWARE` block; the
rest are new.

### 5.3 Layering — where the code lives

```
psx/dev/gpu.c          — core; gains ~16 two-line guarded hooks, no GL/Vulkan
psx/dev/gpu_backend.h  — the ABI (pure C, no GL types); the drafted header, relocated
frontend/gpu_hw_gl/
    gpu_hw.h           — public creation entry point + config struct     [drafted]
    gpu_hw_gl.cpp      — GLES3 backend: FBO, programs, batcher, coherency
    shaders/*.glsl     — draft shaders                                    [drafted]
frontend/gpu_hw_vk/    — later; same ABI
```

The core must not include a GL header. The ABI header carries only `stdint` types and
opaque pointers, which is why the drafted `psx_gpu_backend_t` uses `void* impl` and no
`GLuint`.

**Threading constraint — RESOLVED, no work needed.** The concern was that the EGL context
in `render_gl.cpp` is made current on whichever thread creates the renderer
(`CreateEglContext`, `render_gl.cpp:527`) while GPU commands originate inside
`psx_gpu_write32`. Those turn out to be the same thread: `runVMThread` (in
`frontend/android_jni.cpp`) calls `external_main_ex()`, and `frontend/main.cpp` creates
the renderer, calls `psx_update()` and presents from that single loop. There is no
separate render thread and no hand-off to build. The remaining caveat is availability,
not threading: a GL context exists only when `gpu_backend = "opengl"`.

### 5.4 Config — do not overload `gpu_backend`

`[video] gpu_backend` (`config.c:79`, parsed `config.c:618-639`, stored `config.h:21`)
selects the **presentation** backend: `software | sdl-accelerated | opengl | vulkan`
mapping to 0/1/2/3. It is *not* a rasterizer selector, and `armsx_render_backend_t`
(`render.h:31-36`) confirms this — all four values are present backends.

Add two orthogonal keys:

```toml
[video]
    gpu_backend    = "opengl"    # presentation: software | sdl-accelerated | opengl | vulkan
    renderer       = "software"  # rasterizer:   software | hardware
    internal_scale = 1           # 1..8; hardware renderer only
```

with, in `armsx_config_t` (`config.h`):
```c
int renderer;         /* 0 = software rasterizer, 1 = hardware  */
int internal_scale;   /* 1..8, clamped against GL_MAX_TEXTURE_SIZE at init */
```

Validation rules, enforced at config load:
- `renderer = "hardware"` requires `gpu_backend` ∈ {opengl, vulkan}. If not, log once and
  force `renderer = software` — a HW rasterizer cannot present through an SDL software
  renderer without exactly the round trip we are trying to avoid.
- `internal_scale` clamps to `[1, min(8, GL_MAX_TEXTURE_SIZE / 1024)]`.
- `internal_scale > 1` with `renderer = software` is ignored with a warning, not an error.

**Beware the enum-clamp trap:** adding a value to a settings enum that a UI layer
`coerceIn`s will silently eat the new value, and there are often two independent pickers
for the same setting. Grep every consumer of `cfg->gpu_backend` before touching it —
`main.cpp:411,1434,1456,1485,1555` all reference `USE_HARDWARE` blocks today.

### 5.5 Retiring `frontend/gpu_hw.c`

`frontend/gpu_hw.c` (68 lines) is a vestigial SDL shim whose own comment says it
"never bypasses the core rasterizer", and whose `gpu_hw_render_triangle` (`gpu_hw.c:64-66`)
tail-calls `gpu_render_triangle`. It is referenced by the Makefile (`Makefile:269`, `:383`)
and by `tests/gpu_renderer_parity.c`. Keep it building until Stage 1 replaces it, then
delete it in the same commit that lands the real backend, and re-point the parity test at
the new ABI. Do **not** grow it — the new code goes under `frontend/gpu_hw_gl/`, which is
why the drafted header lives there despite the basename collision with `frontend/gpu_hw.h`.
(Give the new one a distinct include guard, `ARMSX_GPU_HW_GL_H`, which the draft does.)

---

## 6. Staged implementation plan

Effort estimates assume one developer familiar with the codebase but not with PS1 GPU
internals, and count *working* days including debugging, not ideal-world coding time.

### Stage 0 — Prep (2–3 days)

- Delete the dead rasterizers and command stubs (§1.1). ~500 lines, zero behaviour change,
  makes everything after this legible.
- Add `psx_gpu_backend_t` to the core, all hooks NULL-guarded, backend always NULL.
- Verify `make test` / `tests/gpu_renderer_parity.c` still passes and the software output
  is bit-identical (it must be — every hook is guarded).
- Resolve the GL-context-thread question with whoever owns `android_jni.cpp` (§5.3).

**Testable:** no visual change, no perf change, parity test green.

### Stage 1 — Untextured flat + Gouraud triangles at S=1 (5–8 days)

- FBO with an RGBA8 `1024×512` colour target, no stencil yet.
- Vertex buffer, one program, no textures, no blending.
- `draw_poly` for `PA_TEXTURED == 0`; everything else still falls through to software.
- `fill_vram`, `upload_vram`, `download_vram`, `copy_vram` implemented as
  straightforward, unbatched, correct-but-slow operations.
- Scanout still goes through the existing CPU path by downloading the whole RT each frame
  — deliberately slow, but it isolates rasterization bugs from coherency bugs.

**Testable:** extend `tests/gpu_renderer_parity.c` to diff software VRAM against
downloaded HW VRAM for a corpus of triangles: degenerate, zero-area, shared-edge pairs,
clipped against each drawing-area boundary, negative offsets, and the exact fill-rule
cases from `gpu.c:248-249`. Target: **bit-identical for flat**; Gouraud will differ on
dither (§2.7) and possibly ±1 on interpolation rounding — quantify and record the
tolerance rather than hiding it.

*Risk:* the fill rule and the `x < xmax` half-open loop (`gpu.c:296-297`) interact with
the winding swap (`gpu.c:271-277`). Expect to spend a day on single-pixel edge cases.

### Stage 2 — Textures + CLUT (6–10 days)

- `vram_read` as `R16UI`, refreshed wholesale at first (correctness before speed).
- 4bpp / 8bpp / 15bpp fetch, texture window, texel-0 discard, modulation, raw mode.
- Sprites (`draw_rect`), including the size classes and the texpage-inheritance rule
  (§1.2).
- Still no blending, no mask bit, S=1.

**Testable:** parity on a texture corpus. Then real games: anything 2D and opaque —
menus, Ridge Racer's HUD, Tekken character select.

*Risk:* the texpage/CLUT decode is easy to get subtly wrong (the `<<6` / `<<4` / `<<4`
shifts at `gpu.c:254-257`). The 4bpp nibble select `(texel >> ((tx&3)<<2)) & 0xf` has a
byte-order trap. Budget time for a shader-level texel dump tool.

### Stage 3 — Semi-transparency + mask bit (5–8 days)

- The four blend equations via fixed function (§2.5).
- The alpha-0 trick for opaque texels in ADD-family modes.
- The two-pass split for mode 2.
- Batch segmentation by blend state.
- Stencil attachment + GP0(E6) implementation (§2.6) — **note this changes software
  behaviour too**, since GP0(E6) is currently a stub; implement it in the software path in
  the same commit so the two agree.

**Testable:** Silent Hill fog, Final Fantasy VII/VIII/IX battle transitions and menus
(heavy mode-0 and mode-2), Crash Bandicoot water, any game with additive muzzle flashes.

*Risk:* highest-regression stage. Mode 2 two-pass splitting doubles draw calls in
transition-heavy scenes. Mask-bit implementation will change output in games nobody has
tested it in.

### Stage 4 — Upscaling (4–6 days)

- `internal_scale` plumbed through; RT resize; viewport/scissor scaling.
- Native-coordinate dithering (§2.7).
- UV bounding-box clamping (§3.2).
- Direct-from-RT scanout, removing the Stage-1 full download (§4.4).
- 24bpp forced to the native path (§4.4).

**Testable:** the whole point becomes visible. Check: 1×1 sprites are S×S solid blocks;
no seams on character models; UI text is not haloed; FMV still plays (via the native path).

*Risk:* the half-pixel bug (§3.3) is invisible at S=1 and obvious at S=2 — always test
at S=2 and S=3, never only at S=1 and S=4 (a `*0.5` error hides at even scales).

### Stage 5 — VRAM coherency (6–10 days)

- Tile-based dirty tracking (§4.2), lazy `vram_read` refresh, readback-on-demand.
- Batching with the flush points from §4.5.
- The automatic software downgrade for readback-heavy games (§4.6).

**Testable:** this is where perf arrives. Measure draw calls per frame and readback bytes
per frame in the diagnostics overlay (`frontend/diagnostics.c` already exists). Target
<50 draw calls and 0 bytes readback for a typical 3D game.

*Risk:* the widest-variance stage. Games that render to VRAM regions later used as
textures (very common — shadow maps, mirrors, the classic "render the car then use it as
a reflection") will expose every hole in the invalidation logic as flickering or stale
textures.

### Stage 6 — Enhancements (open-ended, 2–5 days each)

Independently shippable, each behind its own setting, each default-off except where noted:

| Enhancement | Effort | Notes |
|---|---|---|
| True colour (skip 5-bit truncation) | 2 days | trivially in-shader; interacts with dithering (turn dither off when on) |
| Bilinear / xBR texture filtering | 3–5 days | must respect the texel-0 discard, which naive bilinear breaks — filter *after* the alpha test, or filter only within the primitive's UV box |
| Widescreen hack | 3 days | needs GTE projection-matrix patching, **not** a renderer change; see §7.6 |
| PGXP-style precision | 5–15 days | see §7.6 |
| Downsampling / SSAA | 2 days | render at S, present at 1× — the best-looking option for 2D-heavy games |
| MSAA | 3 days | interacts badly with the mask bit and per-fragment blending; low priority |
| 24bpp upscale | — | **not possible**, §4.4 |

**Total to a shippable Stage 4:** roughly **22–35 working days**. To a *good* Stage 5,
**30–45**. Anyone quoting less has not accounted for §7.

---

## 7. Risk register

### 7.1 Stages most likely to produce visible regressions

Ranked:

1. **Stage 3 (semi-transparency + mask bit)** — highest. The mask bit is net-new
   behaviour in a codebase that has never had it; every game that uses it will change
   appearance, and some of those changes will be *corrections* that nonetheless look like
   regressions to someone who is used to the old output. Mode-2 subtraction is used for
   shadows and darkening effects and looks catastrophically wrong when the blend state
   leaks across a batch boundary.
2. **Stage 5 (coherency)** — flickering and stale-texture bugs, which are intermittent and
   therefore expensive to diagnose. Historically the largest source of "works on my
   machine" reports in every HW-renderer emulator.
3. **Stage 4 (upscaling)** — seams and haloing. Highly visible, but usually
   deterministic and therefore tractable.
4. **Stage 2 (textures)** — bugs here are loud and obvious (garbage textures), which
   makes them the *safest* kind.

### 7.2 Game classes that historically break these features

Not a list of confirmed bugs in this emulator — a list of what to test, drawn from the
problem domain:

| Feature | What breaks it |
|---|---|
| VRAM readback | Chrono Cross (screen-warp transitions), Final Fantasy VIII/IX (framebuffer effects on the world map), Silent Hill (fog/noise), anything with a "melt" or "mosaic" transition |
| Mask bit | Metal Gear Solid (the codec/radar overlays), Silent Hill, games that composite HUDs by pre-marking VRAM |
| Semi-transparency mode 2 | Shadow rendering in most 3D platformers; Crash Bandicoot, Spyro |
| Texture window | Games that tile a small texture across a large polygon — racing games' road surfaces, Wipeout tracks |
| Texpage inheritance by sprites (§1.2) | 2D fighters that interleave polys and sprites — the exact games the `gpu.c:1109-1110` comment names: Mortal Kombat II, Bubble Bobble, Driver 1 & 2 |
| Upscaling seams | Any character model with adjacent quads — i.e. all of them; worst on low-poly faces |
| Affine texture warping | Everything; more visible at higher `S` |
| 24bpp display | FMV-heavy games (any Squaresoft title); will visibly not upscale |
| Line rendering | Wipeout, Colony Wars, wireframe debug modes |

The `gpu.c:2106-2119` comment already lists games this core is known to be timing-sensitive
about (Street Fighter Alpha 2, Dead or Alive, NBA Jam, Doom, Devil Dice, Crash Bandicoot,
PaRappa, …). Those are CPU/timer issues, **not** GPU ones, but they will be blamed on the
new renderer if it ships in the same release. Ship the renderer behind a default-off flag
for at least one release cycle.

### 7.3 Things that get *worse* before they get better

- **Stage 1 will be slower than software.** A full RT download every frame plus unbatched
  draws is strictly worse than the current path. This is expected and must be communicated,
  or someone will conclude the project is a failure at the first checkpoint.
- **Draw-call count on Android.** Adreno and Mali both punish state changes hard. A naive
  implementation that breaks the batch on every texpage change will issue 1000+ draws per
  frame and run at single-digit FPS on a mid-range phone. The per-vertex texpage design
  (§2.2) exists specifically to avoid this — do not "simplify" it into a uniform.
- **Tile-based GPUs and FBO reloads.** Every readback, every `glReadPixels`, every
  render-target switch forces a tile flush on Mali/Adreno/PowerVR. The coherency design
  (§4) is not a nicety on mobile; it is the difference between 60 FPS and 12.

### 7.4 Parity vs correctness — the decision that must be made now

The software renderer has at least seven behaviours that differ from real hardware:

| # | Deviation | Site |
|---|---|---|
| 1 | Polygons are always bilinear-filtered | `gpu.c:359` |
| 2 | Sprites point-sample (inconsistent with #1) | `gpu.c:498` |
| 3 | Mask bit entirely unimplemented | `gpu.c:1965-1967` |
| 4 | Dither indexed bounding-box-relative, not absolute | `gpu.c:330-331` |
| 5 | Dither applied pre-modulation and only for Gouraud | `gpu.c:325-336` |
| 6 | GPUSTAT dither-enable bit never read | latched `gpu.c:1941-1942`, never used |
| 7 | Primitive size rejection at 2048/1024 instead of 1023/511 | `gpu.c:291-292` |
| 8 | No texture-page X wrap at 1024 | `gpu.c:184,193,202` |
| 9 | GP0(80) copy neither wraps nor handles overlap | `gpu.c:1877-1885` |
| 10 | Gouraud lines rasterize flat; polylines never drawn | `gpu.c:1236-1252`, `1193-1232` |

**Recommendation:** implement the HW backend *correctly* (fix 3, 4, 5, 6, 8 and match 1
only as an opt-in filter), and treat the parity test as a *diff report* rather than a
pass/fail gate for those specific behaviours. Write the expected divergences into the
test as named exceptions so a *new* divergence still fails the build. Chasing bit-parity
with a renderer that is itself wrong would bake the bugs in permanently.

The one place to be conservative: **#1, bilinear on polygons.** Users are used to how this
emulator currently looks. Point-sampling everything is more accurate but will read as "the
new renderer lost the smoothing". Ship point-sampling as default with a "smooth textures"
toggle that restores the current look.

### 7.5 Pre-existing bugs uncovered while reading (out of scope, worth filing)

- `gpu.c:1119-1132` — the `GPU_HW_DEBUG` call prints `poly.v[0..3]` *before* they are
  assigned at `gpu.c:1134-1153`. Reads uninitialized stack. Only active under `HW_DEBUG`.
- `gpu.c:98-137` — a pending GP0(C0) transfer and a pending GP1(10h) query both write
  `data` in `psx_gpu_read32`; the query wins and silently corrupts the transfer.
- `gpu.c:1106` — for a non-textured polygon, `texp_offset` is 0, so `poly.texp` is
  `buf[0] >> 16`, i.e. the command byte and the top of the colour. Harmless today because
  `texp` is only read under `PA_TEXTURED` (`gpu.c:262-263`), but it is a landmine for
  anyone who later reads `texp` unconditionally.
- `gpu.c:1550-1551` — `gpu_cmd_24` (a triangle command) renders a second triangle using
  `gpu->v3`, which it never assigns. Dead code, so harmless, but another reason to delete
  §1.1's list.

### 7.6 What cannot be achieved in the renderer alone

Three commonly requested features require touching the CPU/GTE side, not the rasterizer:

1. **PGXP-style precision (geometry wobble + perspective-correct textures).**
   The PS1 GTE returns integer screen coordinates with the fractional part discarded, and
   the GPU has no `W`. Recovering subpixel precision means intercepting the GTE's
   `RTPS`/`RTPT` results in the CPU core, keeping a parallel float/`W` value per vertex,
   and tracking it through the memory writes that carry the vertex into the display list.
   That is CPU-core surgery (`psx/cpu.c` and the GTE implementation), not GPU work. The
   renderer's only obligation is to *accept* float positions and a `W` — which the drafted
   vertex format (§2.2) already does, so nothing is foreclosed. **Effort: 5–15 days on top
   of a working Stage 4, and it is where most of the "looks like a modern game" impression
   actually comes from.**
2. **Widescreen.** The correct implementation patches the GTE projection so the game
   renders a wider frustum. Doing it in the renderer (stretching, or scaling X) produces
   distorted geometry and a HUD in the wrong place. `[video] wide_upscale` already exists
   in config (`config.c:85`, `:668-675`) as an output-resolution setting — do not confuse
   the two.
3. **Removing affine texture warping** — same as (1); it *is* PGXP.

Additionally, **interlace** (`display_mode` bit 5) is unimplemented in the core (§1.10)
and is a CRTC/timing concern; the HW renderer inherits whatever the core does and should
not attempt to fix it independently.

---

## 8. Drafted artifacts

Inert, not wired into any build:

- `frontend/gpu_hw_gl/gpu_hw.h` — the proposed `psx_gpu_backend_t` ABI and the GLES
  backend's creation entry point.
- `frontend/gpu_hw_gl/shaders/psx_hw.vert.glsl` — vertex shader.
- `frontend/gpu_hw_gl/shaders/psx_hw.frag.glsl` — fragment shader covering
  textured + Gouraud + semi-transparent with CLUT lookup, dithering, mask bit, and the
  per-texel STP handling from §2.5.

Neither is added to `Makefile` `C_SOURCES` / `CPP_SOURCES`, and the shaders are `.glsl`
data files, so the build is untouched.

## §0.5.8 — GP0(68h) fixed. Parity measurement attempts were CONFOUNDED; conclude nothing from them.

**Landed, and correct independent of any parity number:** GP0(68h) (Monochrome Rectangle 1x1 /
Dot) wrote `gpu->vram[]` directly and diverged from every other primitive three ways:

- no `SE10()` on the offset coordinates — an 11-bit signed coordinate that should wrap instead
  indexed out of the 1024x512 array. **That is an out-of-bounds write, live today.**
- no clip to the drawing area, so it drew where nothing may draw;
- no backend hook, so the rasterizer never learned the pixel existed — invisible while the
  software shadow backs VRAM, a missing pixel the moment the shadow goes.

Fixed by expressing it as the 1x1 rectangle it already is and routing it through the same
`draw_rect` / `gpu_render_rect` path as everything else. The offset must NOT be pre-added:
`gpu_render_rect()` adds `off_x/off_y` itself. `make test-gpu` 13/13, `make test-gpu-profile` green.

**Audit result: `gpu.c` is otherwise clean.** Every non-rasterizer VRAM write is hooked —
`fill_vram` (bounds-checked), `copy_vram` (fires before the host copy so a backend reading
`gpu->vram` still sees pre-copy contents), `upload_vram`. The writes at gpu.c:430-1042 are
inside `gpu_render_*` and are the shadow itself. GP0(68h) was the only unhooked one.

### ⚠ Measurement notes — four traps, all hit in one session

Four device runs returned **bit-for-bit identical** parity (`differing=1386491 (0.6268%)
blank=112962 extra=3748 near=241087 far=1028694 clustered=345875`) across changed code, an armed
vs disarmed `hwgl_gpu_resolve`, and a supposed fast-boot toggle. **No conclusion was drawn and
none should be.** What went wrong, so it is not repeated:

1. **`fast_boot` is NOT written into an existing settings.toml.** The key only appears in a
   freshly generated file, so `sed`-ing it to false matches nothing and fast boot stays ON (the
   C default is 1). Verify with `grep fast_boot files/settings.toml` — an EMPTY result means the
   setting is at its compiled default, not that it is off.
2. **The packaged .so never matches the staged one.** Gradle runs `stripDebugDebugSymbols`, so
   comparing `md5` of `jniLibs/.../libarmsx.so` against the APK's copy always mismatches. It is
   not staleness. Discriminate with `strings` on a symbol unique to the change instead.
3. **GP0(68h) is rare.** A game that never issues it makes this fix a no-op on parity, so
   identical numbers may be entirely correct. Confirm the command is actually exercised before
   using it as an A/B lever at all.
4. **Identical-to-the-digit across a code change is a signal, not a result.** It means the
   variable under test is not the variable that moved. Act on it the first time.

**Consequence: §4.3 row 1's regression is still UNMEASURED against this fix.** §0.5.7's
`0.4708 % -> 0.9986 %` stands as the last trustworthy figure. A valid A/B needs a game that
issues GP0(68h), a confirmed-active marker (check the `coherency:` line), and a fast-boot state
verified by reading it back rather than by writing it.

**Structural note that still stands:** the RGBA8 render target has nowhere to keep VRAM bit 15
(gpu_hw_gl.c:825, :1270, :1359), so any tile laundered GPU->GPU through it loses the mask/STP
bit and the high bit of a 4bpp palette index. That is a format limitation, not a marking bug,
and §2.6 (a stencil attachment or second target) is what addresses it.

## §PGXP — precise-vertex pipeline (core side landed; renderer patch proposed below)

The GTE computes screen X/Y internally at ~16.16 precision and truncates to 11-bit
signed integers the moment results land in the SXY FIFO. Games copy those integers
into primitive buffers in RAM and DMA them to GP0, so by the time `gpu_poly()` sees a
vertex the sub-pixel information is gone — that truncation IS the PS1 polygon wobble.
PGXP carries the pre-truncation values alongside the integers, end to end. Everything
below except the renderer consumption is in the tree, default OFF (`[video] pgxp`),
and inert to the last branch when off — `make test-gpu` (13/13) and `test-cpu` both
pass with the hooks compiled in.

### Capture points (all in psx/cpu.c)

| Site | Line | What happens |
|---|---|---|
| `GTE_PGXP_CAPTURE()` macro | cpu.c:2008 | Shared tail of `GTE_RTP_DQ` (:2035, RTPS + RTPT's third vertex) and `GTE_RTP` (:2058, RTPT vertices 0/1). Runs after the `R_SX2`/`R_SY2` assignments with the macro-local UNR quotient `div` still in scope, so the float is built from **exactly** the products the integer path truncated: `fx = ((int32)OFX + IR1*div) / 65536.0` in double (exact — |IR1·div| < 2^33 < 2^53), then clamped to the same [-1024, 1023] range `gte_clamp_sxy()` saturates to. `fw = SZ3`, the depth the projection divide used. |
| `psx_pgxp_gte_vertex()` | pgxp.c | Shifts the module's 3-deep FIFO shadow exactly as the hardware FIFO shifts and stores `{fx, fy, fw, packed_sxy}` in slot 2. |
| `gte_write_register()` hook | cpu.c:1602 | Direct SXY writes (MTC2/LWC2 regs 12–14) invalidate the matching shadow slot; a reg-15 SXYP push shifts the shadow and invalidates slot 2 (`psx_pgxp_gte_reg_write`). Keeps manual-FIFO-push games (and anything else that fabricates SXY contents) from ever pairing a stale float with a fresh integer. |

### Address tracking (RAM shadow)

`psx/pgxp.c` keeps a **direct table, not a hash**: one entry per word for the full
2 MiB of RAM (KUSEG/KSEG0/KSEG1 and the 4× mirror folded down, same masking the bus
does) plus the 1 KiB scratchpad — `{float x, y, w; u32 value; u32 valid}` per word,
~10 MiB, allocated on first enable and **never freed while a machine runs** (a JNI
toggle mid-frame must not race the emulation thread against `free()`; disable just
clears the flag). Entries are created/killed by:

* `SWC2` (cpu.c:1705) — the main road. Storing reg 12/13/14/15 attaches the FIFO
  shadow slot to the destination address *if the stored word equals the shadow's
  word*; storing any other COP2 reg, or a shadow mismatch, kills the entry at that
  address.
* `MFC2` (cpu.c:1722) → per-CPU-register shadow (32 entries), then `SW` (cpu.c:1081)
  attaches that register shadow to the stored-to address under the same
  value-equality rule, and kills the entry otherwise. This is the
  "read SXY into a temp, build the prim field" pattern.
* Anything that writes RAM *without* a hook (SH/SB/SWL/SWR, CDROM/MDEC/OTC DMA,
  GPU→RAM readback) is caught by the validation rule at consumption time instead —
  see below. That is why the hook set can stay this small.

### The DMA → GPU address queue

`dma.c` is the only place a GP0 word still has a RAM address, so:

* `psx_pgxp_note_gp0_word(addr)` immediately before the bus write to 0x1f801810 —
  linked-list walk (dma.c:261) and request-mode to-device transfers (dma.c:297).
* The bus write lands in `psx_gpu_write32()` **synchronously**, so a single pending
  latch is the whole queue. The GP0 intake consumes it into a 16-entry table
  parallel to `gpu->buf[]`: RECV_CMD → slot 0 (gpu.c:2148), RECV_ARGS → slot
  `buf_index` (gpu.c:2157), RECV_DATA → **discarded** (gpu.c:2169, image words carry
  no vertices).
* An MMIO GP0 write (CPU store to 0x1f801810) arrives with no note pending and
  records "no address" for its slot — a stale DMA address can therefore never attach
  to a word it did not travel with. This invariant is what makes wrong-attach
  (bizarre geometry, worse than no PGXP) structurally impossible rather than merely
  unlikely.

### Validation rule (do not weaken)

A cache hit is used **only** when the cached 32-bit truncated word equals the vertex
word being parsed (`pgxp.c: psx_pgxp_poly_vertex`, called from `gpu_poly` at
gpu.c:1225 for all four `v[]`s — on miss/off it doubles as the field initializer, so
`precise_valid` is always defined on the poly path). Same equality gates every
SWC2/SW/MFC2 hand-off above. If the game modified a value anywhere along the chain,
the floats are stale and the vertex silently stays integer, which is by construction
the with-PGXP-off behaviour. `vertex_t` (psx/dev/gpu.h) gained `px, py, pw,
precise_valid`; `gpu_save_vertex()` still writes the classic 10-byte layout, so the
save-state format is unchanged — the whole module is derived state and resets on
state load (state.c:830) and soft reset (psx.c:274) instead of serializing.

### Renderer consumption — PROPOSED PATCH, NOT APPLIED (gpu_hw_gl.c is under active
### parallel modification; integrate by content anchors, not line numbers)

Because this backend rasterizes a bounding box and does its own coverage/barycentric
math from the flat `tri0/tri1` attributes (see the `gl_emit_tri` header comment),
sub-pixel positions need **no new vertex-shader plumbing at all**: feeding precise
floats into `tri[]` moves both the shader's edge tests and its interpolation. The
design-artifact `psx_hw_vertex.w` slot (gpu_hw_gl/gpu_hw.h:52, "1.0 today; PGXP
hook") maps onto a **flat w-triple per triangle** (`triw[3]`), NOT a per-vertex
varying and NOT `gl_Position.w` — hardware interpolation is unused here by design,
and the submitted geometry is a bbox whose `gl_Position.w` must stay 1.0. Gating is
pure `precise_valid` (all three vertices, `pw > 0`), so the file needs no pgxp.h
include: the core only ever sets the flag when PGXP is on and validated. The full
diff is in the PGXP task report; summary of the five hunks:

1. `gl_vertex_t` + `kDrawAttribs` + `gl_setup_attribs()`: new `float triw[3]`
   (attrib 10, `a_triw`), stride grows 72 → 84 bytes.
2. `gl_fill_common(..., const float triw[3], ...)` copies it; `gl_emit_quad` passes
   `{1,1,1}` (sprites/lines stay integer).
3. `gl_triangle()`: when `a.precise_valid && b.precise_valid && c.precise_valid` and
   all `pw > 0`, build `tri[]` from `px/py + (int16_t)gpu->off_x/off_y` and pass
   `triw = {a.pw, b.pw, c.pw}`; else exactly today's integer `tri[]` and
   `{1,1,1}`. The winding swap stays decided by the INTEGER edge test (parity with
   the software shadow's decision); precise floats ride the same a/b/c structs. The
   write-flush/dirty-mark rectangle is widened 1px on each side in precise mode so a
   float triangle can never leak outside the marked tiles.
4. Vertex shader: `in vec3 a_triw; flat out vec3 v_triw;` pass-through (one line in
   `main()`); fragment shader declares `flat in vec3 v_triw`.
5. Fragment shader UV block (textured, non-sprite): when the three w's are not all
   equal, perspective-correct interpolation
   `iwN = zN / v_triw[N]; uv = Σ(iwN·uvN) / Σ(iwN)` replaces the affine
   `bdiv(Σ zN·uvN, area)`; the all-equal case (every non-PGXP triangle — {1,1,1})
   keeps `bdiv` and therefore keeps the 1x parity gate byte-identical. Colour stays
   affine on purpose (authentic Gouraud; perspective-correct colour is a separate
   later toggle if ever).

At 1x with PGXP OFF nothing changes (gate-proof); at 1x with PGXP ON coverage
follows sub-pixel edges, so 1x parity vs the software rasterizer no longer holds
while the toggle is on — that is the feature, and it is why the setting defaults off
and the parity harness must keep running with it off.

### Known gaps, ranked (v2 candidates)

1. **MMIO GP0 path carries no addresses** — a game that CPU-writes its command list
   to 0x1f801810 (rare; almost everything DMAs) gets integer vertices. Fixing it
   means tracking value→register→MMIO-store, i.e. the full DuckStation-style CPU
   value pipe.
2. **LW does not repopulate the register shadow** — a prim staged through scratchpad
   with LW/SW word copies (rather than SWC2 direct) loses precision at the second
   hop. Cheap to add: LW hook that adopts the cache entry when the loaded word
   matches, feeding the existing SW hook.
3. **Culling correction** — the GPU's 2048/1024 size reject and the winding decision
   still use integers; DuckStation also offers PGXP-aware NCLIP replacement to fix
   backface pops on near-degenerate triangles.
4. **disable-2d heuristics** — DuckStation optionally skips PGXP for screen-space UI
   quads (z uniform / w tiny) so HUD elements stay pixel-snapped; not needed until a
   game shows drifting UI with the toggle on.
5. **Lines and the polyline parser** keep integer endpoints (`gpu_line` zeroes the
   flags; the polyline path is parse-only today anyway).
6. Pre-existing, noticed while wiring: the polyline receiver increments
   `gpu->buf_index` without bounding it against `buf[16]` (gpu.c RECV_ARGS polyline
   branch) — PGXP masks its own slot table to 16 so it cannot be hurt, but the core
   buffer itself can overrun on a hostile/degenerate polyline. Worth an independent
   fix.

## §0.5.9 — The polygon seams: root cause, probe, and the coverage contract (FIXED)

**Symptom:** dark hairlines along polygon edges with the GLES rasterizer, reported at every
scale, absent in software. Four shader-reading fixes failed before any measurement existed.

**The two instruments that ended it (both permanent, marker-armed, zero cost when off):**

- `hwgl_upscale_parity` — box-averages each native pixel's S×S block back to one pixel and
  compares against the software shadow (busy-frame gated, frame ≥ 3000). Pre-fix verdict at 3x:
  bad=3765 (3.06%), every bad pixel DARKER on all channels.
- `hwgl_paint_reject` — the decisive probe, **v2 semantics: paints ACCEPTED geometry** (polys
  flat green, sprites flat blue, plain writes immune to blend state, before texture/STP so
  texel transparency cannot mask geometric coverage). v1 painted REJECTS magenta and was
  misleading by construction — a later primitive's rejected bbox fragments overpaint an earlier
  primitive's correct pixels, so dense meshes drown in magenta regardless of correctness.
  Paint accepts, never rejects.

**What the probe showed:** dark unclaimed hairlines BETWEEN abutting green (accepted) quads —
genuine coverage holes; the value path (texture/interp/blend) exonerated for these lines.

**Root cause:** mixed coverage granularity. The original shader tested the bounding box at
NATIVE granularity (pn) but the top-left edge rule at SUBPIXEL positions (pf); a later attempt
made both subpixel. Neither matches psx/dev/gpu.c, and both can strand subpixels that no
primitive claims. Software is the only rasterizer proven watertight here.

**The fix — the coverage contract (do not regress this):**
- COVERAGE is decided once per native pixel at `pc = vec2(pn)`, bbox and all three tl() edge
  tests, exactly mirroring gpu.c. The decision applies to the whole S×S block. Coverage
  therefore EQUALS software coverage at every scale; a hole requires a software hole.
- INTERPOLATION (colour + UV) stays at the subpixel position pf via z0..z2, with a fallback:
  if any pf-barycentric is negative (subpixel outside the triangle but its native pixel
  accepted), interpolation uses the native-centre barycentrics zc0..zc2 — inside by
  construction, value identical to software's for that pixel. No extrapolation, no weight-
  clamp darkening.
- At 1x, pc == pf, so 1x behaviour and the 1x parity gate are untouched by construction.

**The honest cost:** edge silhouettes quantize to native pixels when upscaling; textures and
Gouraud shading still gain full subpixel resolution. Smooth subpixel edges require real GPU
rasterization with the hardware's own watertight fill rule — that is the planned PGXP-era
renderer mode, not a per-fragment discard rule. PGXP integration MUST keep this contract for
the discard path or bring the full replacement, nothing in between.

**Also learned:** the mcd path-overwrite (borrowed dangling pointer → memcard flushed over the
user's .cue, see psx/dev/mcd.c comment) corrupted the test disc mid-hunt and mimicked a
rendering regression. When a renderer bug "suddenly gets worse", verify the DISC opens before
touching the renderer: `grep "Failed to open CD image" armsx.log`.

### §PGXP v1 status (post-integration, on-device)

Renderer patch integrated (positions + flat triw + perspective-correct UV branch, attrib 10).
First on-device run showed two defect classes; three guards landed in gl_triangle:

1. **Vanishing polys** (Crash's face) — a near-degenerate triangle whose PRECISE float area
   flips sign against the integer winding decision fails every top-left test and disappears.
   Fix: the winding swap follows the precise edge sign for precise triangles.
2. **Attach tolerance** — each precise coordinate must agree with its integer truncation within
   1.0px or the whole triangle falls back to integer (stale-attach teleports become no-ops).
3. **w-ratio guard** — wmax > 32·wmin keeps precise positions but falls back to affine UV
   (one outlier w otherwise smears the triangle's texture).

**Known remaining defect (v2 blocker for default-on):** cracks where a PRECISE triangle abuts
an INTEGER one — the precise edge sits at x+0.4 while the neighbour's sits at x, and the gap
shows. Inherent to per-triangle all-or-nothing gating with partial attach coverage. Crack-free
alternative for v2: keep COVERAGE on the integer coordinates (preserving the §0.5.9 watertight
contract exactly) and use precise data ONLY for perspective-correct UV + shading — loses the
wobble fix but keeps texture correction with zero cracks; offer both as sub-modes.
PGXP ships DEFAULT OFF; the 1x parity gate is only meaningful with it off.

## §0.5.10 — The Android surface generation had no publisher (black picture after a task switch)

**Symptom.** Background the app (HOME / slide out), wait, come back: the game image is black and
stays black until the game is restarted. The emulation loop keeps running (the OSD's FPS updates
normally, the pause menu opens and its Resume button works), and the pause menu's backdrop —
which is the last presented frame — is black too.

**Why it survives everything.** `armsx_render_set_native_window()` (frontend/render.cpp) bumps a
generation counter, and both GPU present backends watch it: `GlEnsureWindowSurface()` rebuilds
the EGLSurface and `BeginFrame()` rebuilds the VkSurfaceKHR + swapchain when it moves. All of
that was correct. Nothing called it. The **only** publishers were
`CreateHostWindowAndRenderer()` and `DestroyHostWindowAndRenderer()`, i.e. once at the start and
once at the end of a VM run — so the counter could not move *inside* a session no matter what
Android did to the window. `EmulationSurface.surfaceDestroyed()` → `onNativeSurfaceDestroyed()`
and `surfaceChanged()` → `onNativeSurfaceChanged()` both existed, both ran, and both updated only
the JNI-local ANativeWindow used by the CPU blit bridge.

Android destroys a SurfaceView's ANativeWindow when the activity stops and hands back a brand new
one on resume. The backend therefore kept presenting into the window that had already been
destroyed. Restarting the game re-enters `runVMThread()`, which republishes — which is exactly
why a restart is the only thing that brings the picture back.

**Fix (3 parts, all required together).**

1. `onNativeSurfaceChanged` / `onNativeSurfaceDestroyed` publish to the render layer. Both log the
   generation to the diag log, so `armsx.log` alone shows the window lifecycle next to the
   backend's own `surface rebuilt after resume` line. A change that reports the same window at
   the same size does not republish — every bump costs a real surface + swapchain rebuild.
2. `armsx_render_set_native_window(nullptr, …)` no longer clears `g_native_window_claimed`. The
   claim answers "does a GPU backend own presentation on this Surface", which a surfaceDestroyed
   does not change: the backend is alive and about to rebuild. Both backends raise it and lower
   it themselves. Clearing it from the publish path was a third writer with no matching setter on
   the rebuild path, and it would have un-parked the host's CPU blit bridge for the gap — two
   producers on one buffer queue, plus the bridge stamping WINDOW_FORMAT_RGBX_8888 over an
   EGLConfig's native visual (EGL_BAD_MATCH on the next `eglCreateWindowSurface`).
3. Both rebuild paths now swap their own ANativeWindow reference (release the old one after the
   surface it belonged to is destroyed, acquire the replacement) and re-raise the claim. The GL
   path also re-applies `ANativeWindow_setBuffersGeometry(0, 0, native_visual)` before
   `eglCreateWindowSurface`, exactly as the initial bring-up does — a replacement window arrives
   with the SurfaceView's own format.

**`resume_probe` — the marker that says which side came back empty.** "Black after a resume" has
two indistinguishable causes: the SOURCE is empty (the GLES render target / software framebuffer
lost its contents and a correct present layer is faithfully presenting nothing) or the PRESENT
side is empty (the source is intact and the window/swapchain/presented image is what died).
`touch files/logs/resume_probe` arms a scan of the first 8 frames the frontend uploads after
presentation resumes:

```
[hw] resume-probe left=7 source=backend-readback 320x240 stride=1920 format=SDL_PIXELFORMAT_BGR555 nonzero=0 first_row=-1 dirty=[0..239] scale=3
```

`nonzero=0` ⇒ the source came back empty ⇒ rasterizer side. `nonzero>0` while the screen is black
⇒ the source is intact ⇒ present side. `source=adopted-gl-texture` means the brokered seam is in
use, so there is no CPU frame to sample and the GL render target *is* the presented image.
Disarmed (the shipping default) the probe is one integer compare per frame.

---

## §0.5.10 — The display/video feature set (widescreen, filtering, downsampling, deinterlace, overscan, rotation, line detect)

Seven settings, all default OFF/neutral, all wired end to end: core hook → `settings.toml` →
JNI → Kotlin `Ps1Settings`/`Ps1SettingsStore` → a control on the Video tab. Nothing here changes
a single pixel until a user turns it on, which is also the no-regression proof: the three gates
(`test-gpu`, `test-gpu-profile`, `test-cpu`) run with every one of them off.

**The §0.5.9 coverage contract is intact.** Filtering acts on the texture SAMPLE and
downsampling on the RESOLVE; neither touches the coverage decision, which is still made once per
native pixel at `pc = vec2(pn)`. Line detection is the one that moves geometry — and it moves
the triangle's VERTICES before the rule runs, exactly as a game moving them itself would. The
rule is unchanged, coverage stays watertight, and it still equals the software rasterizer's for
the geometry actually submitted.

### Where each one lives

| Feature | Core hook | `[video]` key | Live JNI |
|---|---|---|---|
| Widescreen hack | `psx/cpu.c:2010` `GTE_WIDE()`, used by `GTE_RTP`/`GTE_RTP_DQ`/`GTE_PGXP_CAPTURE` | `widescreen_hack` | `setWidescreenHack` |
| Texture filtering | `gpu_hw_gl.c` `kDrawFS` — `fetch_smooth` / `fetch_xbr`, `u_filter` | `texture_filter` | `setGlVideoOptions` |
| Downsampling | `gpu_hw_gl.c` `kResolveFS` `u_box` + `gl_downsample_factor` / `gl_present_scale` | `downsample` | `setGlVideoOptions` |
| Line detection | `gpu_hw_gl.c` `gl_expand_thin_poly` in `gl_draw_poly` | `line_detect` | `setGlVideoOptions` |
| Deinterlacing | `main.cpp` `ArmsxSession::deinterlaceFrame` / `frameIsCombed` | `deinterlace` | `setDeinterlaceMode` |
| Overscan crop | `render.h` `crop_*` params → all three present backends | `overscan_crop` | `setOverscanCrop` |
| Display rotation | `render.h` `rotation` → `armsx_render_compute_dst` + each backend | `display_rotation` | `setDisplayRotation` |

### Facts worth not rediscovering

**The GTE hack belongs on the IR1 product, not on the result.** `OFX` is the projection centre;
scaling `OFX + IR1*div` would move the centre and swim the whole scene sideways. Scaling only
`IR1*div` by 3/4 shrinks X about the centre, which is what a 16:9 stretch then undoes. The PGXP
capture applies the same factor or the two paths disagree by 25% on every precise vertex.

**A downsample factor must DIVIDE the internal scale.** Otherwise the box straddles native pixel
edges and the result shimmers. `gl_downsample_factor()` treats the request as a ceiling and takes
the largest divisor at or below it, so at 1x the answer is 1 (off) and the setting says so in the
UI rather than silently doing nothing. Consequence: `gl_resolution_scale()` now returns
`scale / factor`, because what the core and `main.cpp` mean by "the multiplier in use" is the
multiplier of the image they are HANDED, and downsampling reduces that while the render target
stays at `g->scale`. Both parity harnesses are skipped while a factor is active — they assume a
stride of `w * g->scale` and would misread the smaller buffer.

**This core has no field concept at all.** Grep `interlace`/`field` across `psx/`: nothing. A
480-line mode returns height 480 from `psx_get_dmode_height()` and the frame is read as 480
consecutive VRAM rows — a weave of whatever the game left there. That is mode 0. Bob repeats the
even native lines over the odd ones and deliberately KEEPS the frame's geometry (width, height,
pitch) so the dirty-row scan, the snapshot, the crop rect and the aspect need to know nothing
about it. Deinterlacing declines the zero-copy GL seam, because it is a CPU pass over the
finished frame and the pixels have to come back to the host.

**Rotation had to go in `armsx_render_compute_dst`, not in the backends.** A quarter turn
transposes the rect AND inverts the aspect — `main.cpp` hands over a hard 4:3, and a rotated
frame presented at 4:3 is squashed. Doing it once in the shared function is what keeps every
backend letterboxing identically. `SDL_RenderCopyEx` then needs the dst rect handed back
UNROTATED (w/h swapped, same centre), because it maps src onto dst and spins the RESULT.
`vkCmdBlitImage` cannot rotate at all; that backend logs once and presents upright.

**Line detection only rescues the vanishing case in `quads` mode.** A polygon with zero extent in
one axis draws nothing — `gpu.c`'s `for (y = ymin; y < ymax; y++)` is half-open and
`gl_emit_tri()` bails on `hi_y <= lo_y` to match. Those are the lines that disappear. A polygon
that is already 1px thin does NOT vanish on this backend (coverage is native-granular, so it
stays 1 native pixel at every scale), which is why widening those is the separate, aggressive
`basic` mode and not the default behaviour.

**Four keys were parsed and never written back**, and the call-site grep is what caught it —
`psxe_cfg_load()` builds locals and copies them onto `cfg` at the end, and
`widescreen_hack`/`deinterlace`/`overscan_crop`/`display_rotation` had no copy line. The setting
would have appeared in `settings.toml`, been read correctly, and done nothing. All seven now sit
together outside the `USE_HARDWARE` guard. There is a throwaway harness pattern for this: build
`frontend/config.c` + `toml.c` + `argparse.c` against stubs and assert that (1) a file with none
of the keys yields every documented default, (2) every key round-trips, and (3) junk tokens land
on the default rather than on some other mode.

## §0.5.11 — `gpu_prim_dump`: one frame of GP0 traffic, on demand (instrumentation, no behaviour change)

**Why this exists.** Silent Hill (SLUS-00707) draws a hard-edged, axis-aligned rectangle around
the player character, lighter inside than the scene around it, on BOTH rasterizers. Two
reasoned fixes were shipped against it and neither moved it:

  1. `accurate_mask_bit` / `accurate_dither` were turned on by default (GP0(E6) was previously
     a stub). No change to the artifact.
  2. A genuine per-pixel bug was found and fixed in `psx/dev/gpu.c` — `transp` was declared
     once per primitive and then ASSIGNED inside the pixel loop from the texel's bit 15, so
     the first opaque texel latched semi-transparency off for the rest of the primitive. It is
     now `const int transp_default` plus a per-pixel `int transp = transp_default;` at the top
     of both pixel loops (triangle and rect). Real bug, keep the fix, `GPU_PARITY` still
     13/13 — but the box is unchanged, so it is not the cause.

Both attempts failed the same way: they reasoned from the rasterizer about a question the
rasterizer cannot answer. *Which* primitive covers that rectangle, what blend mode it carries,
what texture page it reads, and what clip rect was live when it was issued are properties of the
command stream, not of the code that consumes it. So: measure first.

**What it is.** `psx_gpu_debug_set_log_dir(gpu, dir)` (`psx/dev/gpu.h`) hands the core the
directory that holds `armsx.log`; `frontend/main.cpp` calls it once in `ArmsxSession::create()`
right after `psx_gpu_set_accuracy_flags()`. From then on the GPU probes for a marker file named
`gpu_prim_dump` in that directory, and when it finds one it writes exactly ONE frame of GP0
traffic to `gpu_prim_dump.txt` beside it.

Every primitive gets a line carrying its screen-space bounding box (drawing offset applied),
`attrib` decoded by name, the effective semi-transparency mode, texture page / colour depth /
CLUT, and — repeated on every line, deliberately — the full drawing context it was rasterised
against: draw area, drawing offset, texture window, GPUSTAT, mask bits, dither. A clip rect or
texture window that was set for one pass and never restored is invisible in a header printed
once; it is obvious when every line carries it. `GP0(E1..E6)`, the `GP1` display latches, and
the four VRAM transfer commands (`02` fill, `80` copy, `A0` upload, `C0` download) are
interleaved into the same stream so the ordering is readable.

**Why it is safe to ship enabled.** The project has already been burned by an error-level log
in `dma.c` that produced 89,000 lines in 105 seconds, so the budget is structural, not a
promise:

  * one frame per arming, by construction — the marker is `remove()`d the instant a capture
    starts and the file is closed at the very next vblank;
  * four captures per process, each needing its own fresh `touch`;
  * 8,000 lines per capture, then a single `... truncated` line;
  * idle cost is one `if (gpu->dbg_file)` per primitive against a NULL that is never written,
    plus one `fopen()` every 30 vblanks while captures remain. Once the budget is spent even the
    probe stops. Nothing runs per pixel.

**Three implementation constraints worth keeping.**

*The directory is PUSHED in, not pulled.* `frontend/gpu_hw_gl.c`'s `gl_debug_marker()` calls
`psxe_diag_log_path()` directly, which it can afford to because it only ever links against the
frontend. `psx/dev/gpu.c` cannot: `tests/gpu_renderer_parity.c` and `tests/cpu_differential.c`
both compile it WITHOUT `frontend/diagnostics.c`, so a direct call would break the parity gate
at link time. Hence the setter, and hence `char* dbg_dir` owned by the GPU (malloc + memcpy,
freed in `psx_gpu_destroy` along with any capture still in flight).

*Output goes to its own `FILE*`, not through `psxe_diag_logf()`.* It has to work with
diagnostics logging switched off (`psxe_diag_log_path()` is populated by
`psxe_diag_initialize()` regardless of the enabled flag, but `psxe_diag_logf()` writes nothing
when it is off), ~1000 timestamped lines would drown `armsx.log`, and the user then sends one
small file instead of the whole log.

*Writes use `fputs()`, never `fprintf`/`fputc`.* `frontend/diagnostics.h` `#define`s
`fprintf`, `fputc` and `putc` into the diag pipe, so the "own file" would have been mirrored
into `armsx.log` line by line anyway. For the same reason `<stdio.h>` is included in `gpu.c`
BEFORE `../log.h` — pulling it in after those macros exist is a redeclaration minefield.

**Using it.**

```
adb shell run-as com.nanodata.armsx touch files/logs/gpu_prim_dump
# ... within ~0.5 s the next frame is captured ...
adb shell run-as com.nanodata.armsx cat files/logs/gpu_prim_dump.txt > gpu_prim_dump.txt
```

Capture while the artifact is ON SCREEN. The primitive whose bounding box matches the
rectangle names the culprit directly, and its line already says whether it is textured, which
blend mode it uses, which texture page it reads, and what clip rect was live.

## §0.5.12 — Silent Hill's box around the player: the dropped texel mask bit (FIXED)

**Symptom.** A uniformly lighter rectangle on the ground around the player, moving with him.
Reported against §0.5.11's `gpu_prim_dump`; the capture is `gpu_prim_dump_capture.txt`
(capture #2, 902 primitives) with `gpu_prim_dump_screenshot.png`.

**Three plausible causes were eliminated first**, all correctly: no stale draw area (`draw=`
is the full frame on every line), no stale texture window (`texw` mask is 0 throughout), no
fill or VRAM copy near the box (one full-screen `GP0(02)` and two 2×1 `GP0(80)`).

**The box IS a primitive.** The search that missed it looked for a primitive matching the
*screenshot's* box coordinates inside a dump of a *different frame* — the two do not
correspond (the dump's camera is elsewhere; its character mesh, `tpage=(704,256)`, sits at
x 35..67 while the screenshot's character is at x ~150..192). Replaying the dump's geometry
offline and rendering per-primitive coverage puts a rectangle around the character and a
second one around the bird, and both are single quads:

```
00407 poly attrib=3a[TRANSP|SHADED|QUAD] bbox=(176,50)-(227,97)  ... maskset=1 maskchk=1 ... c=60535a  (bird)
00833 poly attrib=3a[TRANSP|SHADED|QUAD] bbox=(21,107)-(79,251)  ... maskset=1 maskchk=1 ... c=252022  (player)
```

Flat colour on all four vertices, axis-aligned, untextured, `tmode=1[B+F]` additive, and each
one bracketed by its own state change:

```
GP0(E6) mask set=1 check=1
<the quad>
GP0(E6) mask set=0 check=0
```

**What the game is doing.** This is distance fog, applied per object:

1. `00002` — a full-screen quad with `maskset=1` primes VRAM bit 15 to 1 everywhere.
2. The world is drawn with `maskset=0`. On hardware each write leaves bit 15 = *the source
   texel's* bit 15, so textured geometry whose texels carry STP=1 keeps the bit.
3. Per object, one flat semi-transparent quad over that object's screen bounding box with
   *check* mask on. It lands only where bit 15 is 0, and `set` mask on it stops overlapping
   objects double-fogging the same pixel. Nearer object → darker additive colour
   (`252022`), further → brighter (`60535a`), which is exactly a fog ramp.

**The bug.** Only the force-to-1 half of GP0(E6) bit 0 was implemented. §2.6 of this document
already states the rule — the written bit is `force_mask || texel_bit15` — but every
non-`PA_RAW` textured write in both live rasterizers ended in `color | mask_set`, and `color`
came from `BGR555(...)` with bit 15 clear. So step 2 *cleared* bit 15 across all textured
geometry, step 3 was never masked, and the fog quad painted its whole bounding box.

**The screenshot confirms it differentially.** Two fog quads, identical attributes and blend,
different results:

- over the **sky** — bit 15 still 1 from the step-1 primer — the quad is correctly masked out.
  Measured: the sky around the branch is a flat `0x6a` with no rectangle anywhere.
- over the **ground** — bit 15 wrongly 0 — the quad paints. Measured: a sharp-edged rectangle
  x 150..192, y 124..177 (native), interior = background + a near-constant `+0x21,+0x21,+0x26`,
  i.e. one additive pass of `252022`.
- the box's flat top edge sits at the fence's lower boundary, because the fence's *own* fog
  quads (the other 44 `attrib=3a` primitives, all `maskset=1`) did force bit 15 to 1 there.

**Fix.** `psx_gpu_mask_from_texel()` in `psx/dev/gpu.h`, applied at all four live write sites:
`gpu_render_triangle` and `gpu_render_rect` in `psx/dev/gpu.c`, and the two mirrors in
`frontend/gpu_hw_rt.c`. The written value is now
`color | mask_set | (stp & mask_from_texel)`, where `stp` is the source texel's bit 15,
tracked **per pixel** next to `transp`. Gated by `PSX_GPU_ACCURACY_MASK_BIT` like the rest of
the mask bit, so with the flag off the expression collapses to the old `color | mask_set` and
the shipping default path is byte-identical.

Two caveats, both pre-existing and deliberately not widened here:

- `gpu_fetch_texel_bilinear` ORs bit 15 across its 2×2 tap, so the written mask bit now
  spreads one texel outward on polygons. That is an artefact of the (non-hardware) bilinear
  filter, not of this rule; both rasterizers share the sampler so parity is unaffected.
- `gpu_render_flat_line` still ignores the mask bit entirely, and `GP0(80)` copy does not
  honour "set mask" on the destination (§2.6 says it should).

**Also fixed in passing:** `frontend/gpu_hw_rt.c` still declared `transp` **once per
primitive** in both `rt_render_triangle` and `rt_render_rect` and then assigned the texel's
bit 15 into it inside the loop — the exact latch bug `gpu.c:738-745` describes as fixed "on
both rasterizers". It was not fixed here. Now per-pixel in both.

**Regression test.** `tests/gpu_renderer_parity.c` `run_mask_from_texel_case()`, three cases:

| case | asserts |
|---|---|
| `mask-from-texel-stp-set` | an STP=1 texel writes bit 15, and a later check-mask quad is skipped |
| `mask-from-texel-stp-clear` | an STP=0 texel does not, and the quad draws (control) |
| `mask-from-texel-inert-by-default` | with the accuracy flag off the texel half is inert |

This had to be *behavioural*, not comparative: the corpus cases compare the two rasterizers
against each other and both were wrong the same way, which is why `GPU_PARITY` passed
throughout. Verified to fail before the fix with
`reason=mask-bit-not-taken-from-texel texel=8421 vram=0421 expected bit15=1`.

### §0.5.12b — Dither indexed by absolute VRAM coordinate (separate change)

§1.6 deviation 1: `dy = (y - ymin) & 3; dx = (x - xmin) & 3` re-phases the 4×4 kernel per
primitive, so a gradient split across adjacent Gouraud polygons picks up a seam at every
shared edge. Hardware indexes by the low two bits of the VRAM coordinate being written.

Changed to absolute in all three rasterizers, which must move together:
`psx/dev/gpu.c` (`y & 3`, `x & 3`), `frontend/gpu_hw_rt.c` (`(y / s) & 3`, `(x / s) & 3` —
still divided back to native per §2.7), and the GL draw shader (`pn.x & 3`, `pn.y & 3`; the
per-primitive `vec2 lo` it used is gone). Absolute native indexing is scale-invariant at the
sample points, so the scale-coherence cases are unaffected — confirmed, all 16 `GPU_PARITY`
cases still pass.

**This is landed on hardware-accuracy grounds only. It is NOT confirmed to fix the blocky
sky/treetop corruption.** That artifact is not present in the available capture — the sky in
`gpu_prim_dump_screenshot.png` is a single flat quad (`00002`, one colour `74646c`) and reads
as a uniform `0x6a`, so there is no multi-primitive gradient in it to be discontinuous.
Confirming or refuting the sky hypothesis needs a `gpu_prim_dump` **taken in the scene that
shows the blocky sky** — look for many adjacent `attrib=38`/`3a` Gouraud quads with `dither=1`
tiling the sky region with slowly-varying vertex colours. §1.6 deviation 2 (dither applied
before modulation rather than to the final colour) is untouched and remains a live candidate
for the same artifact.

---

## §0.5.13 — The polygon size cull was 2x too permissive (Silent Hill's stretched-cone smear)

**Symptom reported.** Silent Hill (SLUS-00707), the intro/loading screen where the character
runs on a black background at 320x224: the upper body renders correctly, and from roughly the
waist down the model is stretched into a long downward wedge that tapers to a point near the
bottom of the screen. The stretched region is shaded plausibly — it reads as the trousers'
colours dragged downward — but the *shape* is wrong. The user also reports smearing near
trees, which is **not** assumed here to be the same defect (see "What this does not explain").

**Three earlier fixes did not address it**, and one of them was aimed at the wrong subsystem
entirely: texture filtering was made to point-sample at `texture_filter = nearest` (§0.5.12's
sampler note), on the theory that atlas cells were bleeding. No texel sampling rule can
produce a cone, so that line of enquiry is closed for *this* symptom.

### The rule this core had wrong

psx-spx, "GPU Render Polygon Commands": the maximum distance between two vertices is **1023
horizontally and 511 vertically**, and polygons exceeding that are **not rendered at all**.
DuckStation states the same limit as `MAX_PRIMITIVE_WIDTH 1024` / `MAX_PRIMITIVE_HEIGHT 512`,
rejecting at `>=`.

All three rasterizers rejected at `> 2048` / `> 1024` — twice as permissive in both axes:

| rasterizer | site |
|---|---|
| software | `psx/dev/gpu.c`, `gpu_render_triangle` |
| internal-res CPU backend | `frontend/gpu_hw_rt.c`, `rt_render_triangle` |
| GLES backend | `frontend/gpu_hw_gl.c`, `gl_triangle` |

Two of the three already carried a comment saying so ("this core rejects at 2048/1024, twice
as permissive as hardware"), so the deviation was known and simply never acted on. Because all
three were wrong *identically*, `GPU_PARITY` passed throughout — the same blind spot §0.5.12
hit with the mask bit. A comparative test cannot catch a rule that every arm gets wrong.

### Why the difference is not academic — measured, not argued

The GTE saturates projected screen coordinates to `-1024..+1023` (and flags it). Geometry that
runs past the near plane therefore arrives at the GPU with vertices pinned at exactly `+1023`,
and games rely on the GPU throwing the resulting oversized primitive away. Drawing it instead
produces a triangle stretched from the model to the screen edge, tapering to the saturated
vertex — the reported shape.

Silent Hill does exactly this. Re-analysing the existing `gpu_prim_dump_capture.txt` (capture
#2, one frame of the foggy street, 899 polygon commands → 1540 rasterized triangles):

* **16 triangles exceed the hardware limit** and were drawn anyway. All 16 are ground quads.
* **14 vertices sit exactly on the saturation value** `+1023` — e.g. `(286,1023)`,
  `(-587,1023)`, `(-257,1023)`.
* **2 of the oversized triangles overlap the visible draw area** `(0,32)-(319,255)`, so even
  in the scene the user considers "fine", the emulator is painting a triangle hardware drops.
* The current `> 2048 / > 1024` guard fired on **0** primitives in that frame — it is
  effectively dead code.

The canonical shape is primitive `#00876`: `v=(-42,865), (51,302), (286,1023)`, yspan 721.
Two sane vertices and one at the clamp. That is a wedge tapering to a point.

### Fix

`psx_gpu_prim_oversize()` in `psx/dev/gpu.h` is now the single definition of the rule, called
from all three rasterizers so they cannot drift apart on which primitives get dropped. Gated
by a new `PSX_GPU_ACCURACY_PRIM_SIZE` (0x04) flag beside the mask bit and the dither gate:

* **on** — hardware's 1023x511.
* **off** — the historical 2048x1024, byte-identical to before.

Wired to `[video] accurate_prim_size`, **default true** (`frontend/config.c`, `config.h`,
`frontend/main.cpp`). Off is a genuine one-line A/B rather than a dead setting, which is the
point: the user can flip it and see the cone come back.

Spans are taken on the integer bounding box *after* the drawing offset. The offset is a pure
translation, so it cannot change a span; the accept/reject decision is identical either way,
and identical to the one the PGXP path makes (`gl_triangle` deliberately keeps the size reject
on the integer box even when precise coordinates widen the flush box).

### Instrumentation added to `gpu_prim_dump`

Every `poly` line now ends with a per-**triangle** size report, because the bbox on the line is
the whole quad's and the cull happens after the quad split:

```
span=(328,865)/(235,865) OVERSIZE culled=yes SAT
```

* `span=` one `(xspan,yspan)` per triangle, in the order the rasterizer sees them.
* `OVERSIZE` at least one triangle exceeds 1023x511, i.e. hardware would not render it.
  `culled=` says whether **this** build dropped it — that is `PSX_GPU_ACCURACY_PRIM_SIZE`'s
  live state, so a capture is self-describing.
* `SAT` at least one vertex sits exactly on a GTE saturation value (`-1024` or `+1023`).

`SAT` and `OVERSIZE` are independent on purpose: a saturated vertex inside the limit prints
`SAT` alone, so `SAT` is not merely a restatement of `OVERSIZE`. The capture header's
`accuracy_flags=` decode gained `prim_size=on[1023x511]` / `off[2048x1024]`.

Verified end-to-end against a synthetic replay of `#00876`, both flag states, rather than by
inspection.

### Regression test

`tests/gpu_renderer_parity.c` `run_prim_size_case()`, behavioural for the same reason
§0.5.12's had to be — all three rasterizers were over-permissive identically:

| case | asserts |
|---|---|
| `prim-size-oversize-culled` | yspan 923 with the flag on writes nothing |
| `prim-size-in-range-drawn` | yspan 400 still draws (control: the rasterizer did not simply stop) |
| `prim-size-inert-by-default` | with the flag off the same oversize triangle draws, so the A/B is real |

`PSX_GPU_ACCURACY_PRIM_SIZE` was also added to the `hw-1x-matches-software-accurate` and
`hw-2x-scale-coherent-accurate` flag set, so the software and backend rasterizers are gated
on agreeing about which primitives they drop. All 19 cases pass; `make test-cpu` passes.

### What this does NOT explain, stated plainly

* **CONFIRMED for the trees.** The user reports the foliage smearing is gone with this build.
  That is the win; the rest of this section's speculation about the intro screen was wrong and
  is corrected in §0.5.13a below.
* ~~Not confirmed against the intro screen.~~ **Refuted** — see §0.5.13a.
* ~~The trees/foliage smear is a separate report until data says otherwise.~~ It was the same
  defect, and it is **fixed and user-confirmed**. Worth recording why the reasoning that got
  there was still half wrong: the foliage in the street capture is small `TEXTURED|TRANSP` tris
  and quads out of 4bpp CLUT pages with a no-op texture window and none of them oversized, so
  that capture appeared to contain no evidence for a foliage fix at all. The oversized
  primitives it *did* contain were ground quads. Standing next to a tree evidently puts tree
  geometry past the near plane and into the same saturated/oversized state as the near ground —
  which the capture never showed, because the camera was not next to a tree.
* **The blocky white/lilac sky corruption** (§0.5.12b) is untouched and still unverified.
* `gpu_render_rect` and `gpu_render_flat_line` still have **no** size cull. psx-spx applies the
  limit to lines too. Left alone deliberately: Silent Hill draws 0 rects and 0 lines in the
  captured frame (the on-screen counter reads `0 rect 0 line`), so there is no evidence to fix
  against and widening the change would put other games at risk for nothing.

## §0.5.14 — `gpu_frame_summary`: many consecutive frames, one line each (instrumentation, no behaviour change)

**Why this exists.** Xenogears (disc 1, USA, `RES 320x216`) reports two artifacts that
`gpu_prim_dump` structurally cannot diagnose:

1. Violent whole-screen colour flashing — the same battle scene renders almost entirely dark
   RED on one frame and almost entirely dark BLUE on the next, geometry and layout identical.
2. A copy of the frame appearing offset vertically, clipping in and out repeatedly.

Both are **oscillations between consecutive frames**, and §0.5.11 captures exactly one frame
per arming. One frame of a two-frame cycle answers nothing no matter how detailed it is: the
question is not "what is in this frame" but "what *differs* between this frame and the next".
Arming the one-shot dump twice cannot land on adjacent frames either — the marker is probed
every `GPU_DUMP_POLL_FRAMES` (30) vblanks.

**Two further armings**, sharing all of §0.5.11's machinery and its budget:

| marker | frames | output | for |
|---|---|---|---|
| `gpu_prim_dump` | 1 | `gpu_prim_dump.txt` | unchanged, §0.5.11 |
| `gpu_prim_dump4` | 4 consecutive | `gpu_prim_dump.txt` | full detail once a suspect is named |
| `gpu_frame_summary` | 240 consecutive (~4 s) | `gpu_frame_summary.txt` | **what oscillates** |

**The census line.** In summary mode the per-primitive lines are replaced by ONE line per
frame. That is what makes 240 frames affordable — a full dump of 240 frames would be tens of
megabytes and would blow the line cap in the first second. Each line carries both halves of
the problem:

* the display state that decides the picture's **shape** — `display_mode`, `disp_start`
  (the VRAM read origin), the display window `disp_h`/`disp_v`, and `dheight`, which is the
  height `psx_get_dmode_height()` hands the frontend and therefore the height the texture is
  sized from. `dheight` moving between frames *is* symptom 2;
* the census that decides its **colour** — primitive counts by class, the semi-transparency
  census `tmode=[B/2+F/2, B+F, B-F, B+F/4]`, mask bits, and the VRAM ops, including the colour
  of the largest `GP0(02)` fill of the frame. A whole-screen clear colour that swings between
  frames is the cheapest possible explanation for a whole-screen colour that swings.

Primitives covering **at least half the display in both axes** get their own `BIG` line (max 4
per frame) with attrib, bbox, blend mode, mask bits and first-vertex colour. A global colour
transform — a screen flash, a fade, a fog pass — is one of these, so the thing most likely to
be responsible is itemised individually while everything else is only counted.

**Three implementation constraints worth keeping.**

*Summary mode holds its file open across the whole arming.* 240 `fopen`/`fclose` pairs is 240
more chances to interleave with whatever else writes to that directory, and the census line is
flushed per frame anyway, so a kill still leaves a readable file.

*`dbg_quiet` silences every other `gpu_dumpf()` while a summary runs.* `GP0(E1)` texpage
traffic alone can be hundreds of lines per frame; unsilenced it would swamp a 240-frame
capture and hit the line cap within the first second. The summary's own lines go through
`gpu_dump_rawf()`, which ignores the flag. Consequently the line cap in summary mode is the
budget for the **whole arming**, not for one frame — `gpu_dump_census_reset()` deliberately
does not touch `dbg_lines`.

*`gpu_dump_dmode_width/height()` are duplicates of `psx.c`'s, not calls to it.* `psx.c` is not
linked into `tests/gpu_renderer_parity.c` or `tests/cpu_differential.c`, so a direct call would
break both gates at link time — the same constraint that produced §0.5.11's pushed-in log
directory. **Keep the two in step.** What the census has to report is exactly the number the
frontend sizes its texture from.

**Cost when idle is unchanged.** The per-primitive dump helpers gained one predictable branch
on `gpu->dbg_summary` at the top; the four VRAM-transfer sites gained one increment each,
inside their existing `if (gpu->dbg_file)`. Nothing new runs per pixel, and nothing runs at all
until a marker is found. Marker probing is now three `fopen()` attempts every 30 vblanks
instead of one, and stops entirely once the budget is spent.

**Verified offline before shipping**, against a synthetic double-buffered 320x216 stream with a
full-screen quad whose colour alternates per frame: 240 consecutive frames captured (`F0000`..
`F0239`), `dheight=216` derived from `disp_v=(16,232)`, the alternating quad itemised with its
alternating colour, `disp_start` alternating `(0,0)`/`(0,256)`, an 8x8 sprite correctly not
itemised, self-disarm confirmed (budget 4 to 3), 484 lines total. `gpu_prim_dump4` produced
exactly 4 captures and `gpu_prim_dump` still produces exactly 1.

### What the capture decides

Ranked hypotheses for the colour flashing, and what in `gpu_frame_summary.txt` kills each:

| # | hypothesis | confirmed by | killed by |
|---|---|---|---|
| 1 | a full-screen effect quad whose **blend mode** or colour alternates | a `BIG` line present on both frames with differing `tmode=` or `c=` | `BIG` lines identical across the cycle |
| 2 | a full-screen **fill** whose colour alternates | `fillc=` swinging red/blue between frames | `fill=0`, or `fillc` constant |
| 3 | **24bpp/15bpp** flipping per frame | `mode=` bit 4 toggling; the `15bpp`/`24bpp` word alternating | that word constant |
| 4 | a **blend-mode census swing** with no single big primitive (mode 2 `B-F` applied where mode 1 `B+F` belongs) | `tmode=[...]` counts moving between frames while geometry counts stay equal | the four counts steady |
| 5 | the frontend presenting a **half-composited** frame (readback racing the draw) | every field steady across the cycle — i.e. the core is innocent and the fault is downstream of the GP0 stream | any of the above moving |

For the duplicated/offset frame, the discriminator is on the same line:

* `dheight` alternating (e.g. 216 vs 480) ⇒ the texture is being resized every frame and a
  480-row read of a VRAM holding two 216-tall buffers shows the frame twice. Note
  `psx_get_dmode_height()` short-circuits to 480 on `display_mode & 0x4` **before** it consults
  `disp_y1`/`disp_y2`, so a game toggling the vres bit produces exactly this.
* `disp_start` not alternating cleanly between two Y values ⇒ the double-buffer flip is being
  read at the wrong time.
* both steady ⇒ neither, and the fault is downstream of the core.

**Not yet run on the game.** Nothing here is a fix and nothing here is a diagnosis; §0.5.14 is
the instrument only. The stale-display-rect hypothesis (the app-backgrounding cure that works
on Castlevania SOTN) was **tested and ruled out** for both Xenogears symptoms before this was
written — backgrounding and returning does not change either artifact.


### §0.5.13a — The intro/loading screen is NOT geometry: it is a 24bpp MDEC blit

> **Superseded in its conclusion by §0.5.15.** The finding below — that this screen carries
> no geometry and that no rasterizer change could affect it — is correct as far as it goes,
> but the suspect it names (MDEC) is **wrong**. A four-frame capture showed the same
> artifact on a *15bpp* frame with ~200 polygons and a whole-frame feedback blit, and the
> real defect is in texture modulation rounding. Read §0.5.15. Keep this section for the
> 24bpp capture itself and for the scanout dimensions it rules out, both of which stand.

**The size-cull hypothesis is refuted for this symptom, on data.** A `gpu_prim_dump` taken on
the intro/loading screen with the stretched lower body visible, fresh process, `pgxp = false`:

```
OVERSIZE lines           : 0
SAT (vertex on GTE clamp): 0
```

The stated kill condition was "if there is no OVERSIZE line at all, this hypothesis is dead".
It is dead. The trees were a real and confirmed win; the intro screen is a different defect.

**What the capture actually contains — and it is not what anyone assumed.** 71 lines, 20
primitives, and **zero polygons, zero rects, zero lines**:

```
display_mode=000011 hres=320 vres=240 24bpp progressive NTSC
disp_start=(0,16) disp_h=(600,3160) disp_v=(32,240)
00001 upload GP0(A0) dst=(0,256)   size=24x208 words=4992
00002 upload GP0(A0) dst=(24,256)  size=24x208 words=4992
...
00020 upload GP0(A0) dst=(456,256) size=24x208 words=4992
```

Twenty `GP0(A0)` uploads, 24 halfwords wide each, x stepping 0, 24, 48 ... 456, all at y=256,
all 208 rows tall. That is **20 x 24 = 480 halfwords = 960 bytes = 320 pixels at 24bpp**, by
208 lines: one complete full-screen 24bpp image, uploaded as twenty 16-pixel-wide column
strips, into the back buffer at y=256 while y=16 is displayed. The display is in **24bpp
mode** (`display_mode` bit 4).

So Silent Hill's loading screen is an **MDEC-decoded 24bpp image blitted into VRAM**, not a 3D
scene. The "character with a stretched lower body" is a picture, not a model. **No rasterizer
change can affect it** — which retrospectively explains why the point-sampling fix, the mask
bit fix, the transparency latch fix and the size cull all left it untouched. Four rasterizer
fixes against an artifact that never went through the rasterizer.

**Scanout geometry spec-checked and RULED OUT.** The obvious follow-on suspicion — that the
24bpp scanout has the width, height or stride wrong — does not hold:

| quantity | derivation | value | agrees with the capture? |
|---|---|---|---|
| width | `psx_get_dmode_width`, hres table | 320 | yes: `(3160-600)/8 = 320` |
| height | `psx_get_dmode_height`, `disp_y2 - disp_y1` | 208 | yes: `240-32 = 208`, and the uploads are 208 rows |
| format | `psx_get_display_format`, `display_mode` bit 4 | RGB24 | yes |
| row stride | `PSX_GPU_FB_STRIDE` | 2048 bytes | yes: a VRAM row is 1024 halfwords, and 320 px x 3 B = 960 B of it is used |

`psx_get_dmode_height` derives from the display window rather than the mode, so there is no
208-vs-240 mismatch and no band of stale VRAM at the bottom. `frontend/main.cpp`'s
`updateTexture()` also correctly forces the **native** scanout for 24bpp
(`want_native_scanout`), so the internal-resolution backend is not in this path either. None of
this is the defect.

**The one ambiguity the capture leaves.** The frame ends immediately after upload #20, with no
trailing state change. A one-frame capture cannot rule out that the character's geometry is
submitted just after the vblank that closed the capture, or on alternate frames. That is
exactly what `gpu_prim_dump4` (§0.5.14, 4 consecutive frames at full per-primitive detail)
answers, and it decides between two disjoint next steps:

* **4 frames, all upload-only** -> the artifact is inside the uploaded image, i.e. `psx/dev/mdec.c`.
  The strips are 16 px wide x 208 tall = 13 macroblocks of 16x16 each, and MDEC codes DC
  coefficients *differentially*, so a decode that desyncs partway down a strip makes everything
  below it inherit a wrong DC — colours "dragged downward" from a correct upper region is the
  characteristic signature of exactly that.
* **any frame carries polygons** -> geometry is back in play with vertex data in hand, and the
  remaining candidates are the 11-bit sign extension in the vertex read path (`SE10`, which
  spot-checks correct: `0x7FF -> -1`) and the drawing-offset application.

Recorded so the next engineer does not spend a fifth fix on the rasterizer for this symptom.

---

## §0.5.15 — Silent Hill's loading screen: texture modulation rounded instead of truncating

**Symptom.** Silent Hill (SLUS-00707), the intro/loading screen where the character runs: the
upper body is correct, and from roughly the waist down the figure is smeared into a wedge that
tapers toward the bottom of the frame, "shaded plausibly, like the trousers dragged downward".

**Four rasterizer fixes had already failed on this symptom** — point sampling, the mask bit,
the per-pixel transparency latch, and §0.5.13's size cull. §0.5.13a then established from a
one-frame capture that the screen was a 24bpp MDEC blit with no geometry at all, and named
MDEC as the suspect. **That was also wrong**, and the four-frame capture is what corrected it.

### What the 4-frame capture showed

`gpu_prim_dump4`, artifact on screen. Four frames, ~200 primitives each, **15bpp**, and the
per-frame structure is identical every time:

```
fill  GP0(02) at=(0,32) size=320x224 c=000000
rect  attrib=64[TEXTURED|VARIABLE] size=256x224 uv=(0,0)  tpage=(512,256) depth=2[15bpp-direct] c=808080
rect  attrib=64[TEXTURED|VARIABLE] size=256x224 uv=(0,0)  tpage=(256,256) depth=2[15bpp-direct] c=808080
rect  attrib=64[TEXTURED|VARIABLE] size=256x224 uv=(0,0)  tpage=(0,256)   depth=2[15bpp-direct] c=808080
~196 poly ... tpage=(704,256)          <- the character mesh
```

Three things fall out of it, and together they name the defect:

1. **The three rects are a whole-frame FEEDBACK BLIT.** They re-draw the *other* framebuffer
   into this one as three texture-page-wide tiles. The game's side is exactly right: frames
   targeting buffer A (`draw y 32..255`) read `tpage=(x,256)` at `uv=(0,0)`; frames targeting
   buffer B (`draw y 256..479`) read `tpage=(x,0)` at `uv=(0,32)`. The 32-row offset is the
   one that makes buffer A's rows line up. Nothing is wrong with the blit.
2. **The modulator alternates `c=808080` and `c=7f7f7f`** between frames — 128/128 (identity)
   and 127/128 (decay). That is the game fading its own motion blur.
3. **There is no stretched geometry.** All ~196 polygons come from one page, `tpage=(704,256)`,
   the character mesh; their union is x 120..182, y 25..208 relative to the buffer, and the
   largest single primitive is 20x39 px. Nothing in the stream is tall enough to be the wedge.
   The wedge is not drawn. It is *accumulated*.

Also checked and cleanly ruled out: the two `GP0(80)` copies per frame are `src=(0,0)
dst=(0,0) size=2x1` — two pixels, source and destination identical. They cannot smear
anything.

### The defect

psx-spx, "Texture Color Blending": the 5-bit texture channel is expanded to 8 bits, multiplied
by the 8-bit primitive colour and divided by 128 — `(tex5 << 3) * mod8 >> 7`, **integer
division, truncating**.

All three rasterizers computed that product in float and then rounded:

| rasterizer | expression |
|---|---|
| software | `roundf((tr * mr) / 128.0f)`, `psx/dev/gpu.c` (triangle and rect) |
| internal-res CPU | the same two mirrors, `frontend/gpu_hw_rt.c` |
| GLES | `floor(clamp(t * md / 128.0, 0.0, 255.0) + 0.5)`, draw shader in `frontend/gpu_hw_gl.c` |

On its own that is nothing: rounding and truncation differ on **402 of the 32x256
(texel, modulator) pairs — 4.9%, always by exactly one level of 31**, and **never** at the
identity modulator `0x80`. No single primitive can show it, and because all three rasterizers
rounded identically, no comparative case could either. Same blind spot as §0.5.12 and §0.5.13.

**A feedback loop turns it into a permanent artifact.** With the modulator at `0x7f`:

```
round(t5 * 7.9375) == t5 * 8   for every t5 <= 8
```

so levels 1..8 are **fixed points**. Running the game's own alternating identity/decay
sequence from full white:

| | trail after 400 frames | frames to reach black |
|---|---|---|
| hardware (truncate) | **0** | **60** (~1 s at 60 Hz) |
| this core (round) | **8/31** | **never** |

Every pose the character has ever been in is burned in at 26% brightness and never fades. The
upper body barely moves, so its ghosts land on top of the current pose and look correct; the
legs sweep, so their ghosts fill in the region below the waist. That is the wedge.

### Fix

`psx_gpu_modulate_channel()` in `psx/dev/gpu.h` is the single definition of the blend, called
from all four C sites (triangle and rect in both CPU rasterizers). The GLES shader takes the
same branch on a new `u_tex_trunc` uniform, fed per primitive from the accuracy flags so it
cannot go stale. `t * md` is at most 63240 and 128 is a power of two, so `t * md / 128.0` is
exact in float and `floor()` cannot land a level low.

Gated by `PSX_GPU_ACCURACY_TEX_MODULATE` (0x08) -> `[video] accurate_tex_modulate`, **default
true**; off restores the float-and-round path bit-identically, which the
`default-path-unchanged` case still proves.

*(Note for whoever touches the GL backend: it currently declines to attach at all when
`accurate_mask_bit` is on, so on the shipping default configuration the CPU rasterizer is what
runs and the shader change is for consistency, not for this bug.)*

### Regression test

`tests/gpu_renderer_parity.c` `run_tex_modulate_case()` runs the actual loop through
`gpu_render_rect` — a 15bpp-direct page sampled into a destination pixel that is fed back as
the next source, alternating `0x808080` and `0x7f7f7f`:

| case | asserts | measured |
|---|---|---|
| `tex-modulate-trail-fades` | with the flag on the trail reaches black | level 0 after **61 frames** |
| `tex-modulate-inert-by-default` | with the flag off it does not | level **8** after 400 frames |

The second is the control: without it the first would also pass if the rasterizer had simply
stopped drawing. `PSX_GPU_ACCURACY_TEX_MODULATE` is also in the
`hw-1x-matches-software-accurate` / `hw-2x-scale-coherent-accurate` flag set, so the two CPU
rasterizers are gated on agreeing. 21 cases pass; `make test-cpu` passes.

### Standing lesson

Three of the five fixes attempted on this one symptom were aimed at whichever subsystem the
artifact *looked* like it belonged to. What actually resolved it was reading the frame
structure first — the fill/blit/geometry census — and noticing that the screen is a feedback
loop. **In a feedback loop, "too small to see" is not a valid reason to leave a rounding rule
wrong**: any per-frame error that is not a contraction becomes a permanent artifact, and the
size of the error only sets how long it takes to get there.

### Not confirmed

The mechanism is proven numerically and the loop is reproduced in a test, but **the user has
not yet seen this build**. §0.5.13's size cull is the precedent for landing on spec grounds and
confirming after: it was landed as an accuracy fix, did not fix the symptom it was aimed at,
and turned out to fix the foliage smearing instead.

### §0.5.14b — What two Xenogears captures actually showed

Two verified captures (`dheight=216` on 240/240 frames in both, the check that caught a
mislabelled Metal Gear Solid capture at `dheight=224` first time round):

* **capture A** — the scene with the colour flashing on screen. Renders every 3rd vblank
  (20 fps), `disp_start` alternating `(0,0)`/`(0,256)`.
* **capture B** — a battle with the duplicate/flicker on screen. Renders every 2nd vblank
  (30 fps), `disp_start` alternating `(0,0)`/`(0,224)`.

`disp_v=(26,242)` in BOTH, so `dheight` is 216 in both and never moves. `mode=000001` on every
frame of both. **The §0.5.14 `dheight` hypothesis is dead for Xenogears** — the 480 excursion
that appeared to support it belonged to the Metal Gear Solid capture.

**Capture B's census is invariant to a degree that is itself the finding.** Across all 120
drawing frames: `transp=87`, `tmode=[0,70,17,0]`, `big=1`, `gpustat=8000000f`, no fills at all
(`fill=0` on every frame), `maskset=0 maskchk=0`, and a clean 60/60 `disp_start` alternation
with no third value and no stutter. `prims` varies only 1624..1657. Nothing about colour,
blend mode, mask state or display state alternates.

**The one thing that moves is where the frame is drawn.** Capture B has exactly one
full-screen semi-transparent subtractive quad per frame — `transp=1 tmode=2[B-F] textured
c=04040c`, colour constant on all 120 — and its bounding box sits at exactly **three** Y
positions:

| ymin | count | always paired with |
|---|---|---|
| 262 | 60 | `disp_start=(0,224)` |
| 486 | 48 | `disp_start=(0,0)` |
| **374** | **12** | `disp_start=(0,0)` |

`486 - 262 = 224`, exactly the framebuffer pitch — that pair is the ordinary double-buffer
alternation. `374` is `262 + 112 = 486 - 112`, i.e. **exactly half the framebuffer pitch**, and
it only ever replaces a `486` frame. The user's description of the artifact is "the same frame
duplicated **halfway** above the main frame, clipping in and out" — 112 is exactly halfway.

**It is strictly periodic**, which cuts both ways and must not be glossed. The 12 frames are
`18, 30, 38, 46 | 98, 110, 118, 126 | 178, 190, 198, 206` — a period of exactly **80 frames**
with four occurrences at offsets `+0, +12, +20, +28`. Structure that regular is at least as
consistent with a periodic game animation as with an emulator fault, so the burst pattern is
evidence that the *effect* is real and repeatable, not evidence that the emulator caused it.

A second field also alternates — the `GP0(80)` VRAM copy is present on 51 of the 60
`disp_start=(0,0)` frames and absent on 9 — but it is **not the same event**: only 2 of the 12
halfway frames are also missing-copy frames, which is chance. The two must not be conflated.

**This is a lead, not a diagnosis, and the census as it stood could not close it.** A bounding
box is `vertex.y + gpu->off_y`, so a quad appearing 112 rows high can equally be the game
moving its geometry or the drawing offset moving under it, and the two have completely
different fixes. `off=(x,y)` and `draw=` are what separate them, and neither was on the line.
Both are now recorded on every `BIG` line and on the per-frame line.

**`GPU_SUMMARY_BIG_MAX` was raised 4 to 12 for a reason worth keeping.** Capture A reported
`big=10..13` while only 4 were itemised, and the 4 that won the cap were opaque textured
background terrain that happened to be submitted first — so the semi-transparent quads, the
only plausible carriers of a whole-screen colour swing, were precisely the ones crowded out.
Capture A therefore does **not** establish that the flashing is absent from the GP0 stream; it
establishes that the instrument could not see the candidates. An itemisation cap that fills up
with whatever is drawn first is a cap that hides the interesting primitive by construction.

**Still unknown, stated plainly.** Whether the `374` frames are the game's own drawing offset
or a mislatched `GP0(E5)`. Whether the colour flashing is in the command stream at all — the
fields that were recorded (fill colour, blend census, display mode, mask bits) do not alternate
in either capture, but the blind spot above means "not in the stream" is not yet earned.

---

## §0.5.16 — The GLES rasterizer implements the mask bit (FIXED — and it had been silently OFF all day)

**Symptom, as the user experienced it:** the "hardware rasterizer" toggle appeared to do
nothing. Every graphics diagnosis of 2026-08-01 was made on the CPU rasterizer without anyone
knowing, including every A/B that was supposed to be comparing the two.

**Cause.** `accurate_mask_bit` was flipped to default **true** earlier the same day, as part of
§0.5.12's confirmed Silent Hill fix (`frontend/config.c`, `Ps1Settings.kt`). `gpu_hw_gl.c`
carried an unconditional decline:

```c
if (psx_gpu_accuracy_flags(gpu) & PSX_GPU_ACCURACY_MASK_BIT) {
    gl_status("accurate mask bit is enabled; the GL rasterizer does not implement it yet...");
    return NULL;                       /* every launch, from that commit onward */
}
```

Two correct changes, each defensible alone, that between them turn a feature off. The decline
was even *logged* — `log_info("GLES rasterizer not used: %s", armsx_hw_gl_status())` — and the
line was still not the one anybody read. **A default flip must be checked against every
`return NULL` that tests the same flag.**

### What was actually built

The mask bit, properly, in the GLES rasterizer. Not the interim "attach anyway and accept the
deviation" option: that would render §0.5.12's bug back on the hardware path while the software
path was right, which is the worst of the three possible states.

**§2.6's stencil plan is wrong, and worth recording as wrong**, because it is the obvious
design and it does not work:

| §2.6 said | Why it fails |
|---|---|
| Stencil bit 0 mirrors VRAM bit 15 | `glStencilFunc`'s reference is **per draw**. GP0(E6) changes **between primitives** — Silent Hill's fog toggles it per object — so every toggle would break the batch, and §7.3 says batch count is the whole difference between 60 fps and 12 on a tiler. |
| "Check mask" is `glStencilFunc(EQUAL, 0, 1)`, free | True, and irrelevant on its own. |
| "Set mask" needs a two-pass split | Also true, and it doubles the draws it was trying to save. |
| VRAM download recombines the mask | **Not expressible.** GLES 3.0 cannot sample a stencil buffer (that is 3.1 + `DEPTH_STENCIL_TEXTURE_MODE`) and `glReadPixels(GL_STENCIL_INDEX)` does not exist. Bit 15 could never reach `vram_tex` (§4.3 row 1) or GP0(C0). Stencil cannot leave the FBO. |

The one thing GLES gives a fragment shader that stencil cannot is **the destination colour**:
`GL_EXT_shader_framebuffer_fetch`. That solves the CHECK exactly, per fragment, and once the
shader is reading the destination it may as well do the blend too — which is what frees the
**alpha channel**, and alpha is a colour channel, so it survives every resolve, readback,
upload and blit the backend already has.

### The design, as shipped

* **Storage** — the RGBA8 render target's **alpha is VRAM bit 15**, exactly 0.0 or 1.0.
* **Blend** — moves into the fragment shader (§2.6's "alpha-channel conflict", resolved in
  favour of the mask bit). The arithmetic is the *same* per-fragment `a` the fixed-function
  unit used: `F + dst*a`, or `dst*a - F` for mode 2, so the four modes need no second table
  and the existing `u_stp_pass` split stays correct and untouched. `GL_BLEND` is off.
* **Per-primitive state travels PER VERTEX** — `GLF_MASK_CHECK` / `GLF_MASK_SET` (0x0400 /
  0x0800) in the flags word, set from `psx_gpu_mask_check()` / `psx_gpu_mask_set()`, the same
  two helpers `gpu.c:1151` and `gpu_hw_rt.c:139` use. This is the decision that makes the
  feature free: no uniform, no state change, **no batch break at any GP0(E6)**.
* **One definition of the rule, in two languages.** `PSX_GPU_MASK_WRITE` and
  `PSX_GPU_MASK_SKIP` live in `psx/dev/gpu.h`; `PSX_GPU_MASK_GLSL` stringifies the *same macro
  bodies* into two `#define`s that `gl_build_draw_fs()` prepends to the shader. `||` and `&&`
  mean the same thing in C and GLSL ES, and macros are also what Adreno's compiler wants
  (`shader_helpers_must_be_macros`). The shader is compiled from the contract, not from a
  fourth hand-written copy of it.
* **Every write path carries alpha**: draws write it; GP0(A0)/the seed take it from bit 15 of
  the uploaded halfword (`kXferFS`, now a blend-disabled plain write); GP0(80)'s blit copies
  it; GP0(02)'s `glClear` sets 0, which is what `gpu.c` does (a fill colour is `BGR555` of 24
  bits and has no bit 15). Lines write 0, because `gpu_render_flat_line` has no mask code
  either — parity with the software path, not with hardware.
* **Every read path reconstructs it**: `kResolveFS` (scanout *and* GP0(C0)/downgrade-seed
  readback) and `kGResFS` (§4.3 row 1's GPU→GPU resolve) OR `0x8000` back in, behind a
  `u_mask` uniform that is 0 when the mask bit is off. That also closes the "bit 15 is written
  as 0" divergence §0.5.7 documented, for mask-mode sessions.
* **Bonus, deliberate:** the shader now re-truncates each blend result to 5 bits before
  storing (`floor(x/8)*8`), because `gpu.c` blends in 8 bits and then packs to BGR555. The
  file header's "a pixel blended more than once can differ by one 5-bit step" divergence does
  not exist in mask mode.

### Where framebuffer fetch is usable, and what happens where it is not

Asked for the reporting device specifically — **Adreno 740, Vulkan present on a custom Turnip
driver, OSD `fb-gl=y fb-vk=y dual=y push=n`:**

* **Yes, usable.** The two are independent stacks. The GL rasterizer's `fb-gl` is
  `GL_EXT_shader_framebuffer_fetch` on the **Qualcomm GLES blob** — Turnip is a Vulkan driver
  and never enters this path; when present is Vulkan, `gl_acquire_context()` makes its own EGL
  context against the system GLES driver. `fb-vk` (raster-order attachment access) belongs to
  the present/Vulkan side and is not consulted here. Adreno's GLES fetch is the one this
  project already trusts (`gpu_profile.c:176`), subject only to `single_fbfetch_attachment`
  — one `inout` attachment — and this shader declares exactly one.
* **`dual=y` is not needed.** Dual-source blending would be the other way to keep alpha as a
  mask bit while blending with a second source; the shader-side blend makes it moot, which is
  good, because Mali does not have it at all.
* **The gate is three-way**: the extension string must be present, `profile->fbfetch_gl` must
  be true (MediaTek Mali advertises fetch and returns zero or stale destination colour; the
  Mali-G57 likewise), and it must not be ANGLE (GLES 3.1 context; fetch has been seen to crash
  the compiler outright). Failing any of them, the backend **declines with a message naming
  which one** and `main.cpp` installs `armsx_hw_rt.c`, which implements the mask bit correctly
  at every internal scale. The user loses GPU rasterization, not accuracy.
* `armsx_gpu_profile_force_fbfetch("on")` overrides the profile veto for anyone who wants to
  try it on a device the profile distrusts.

### The behavioural test — three layers, because comparison alone is blind here

The standing lesson of §0.5.12, §0.5.13, §0.5.15 and the nearest-filter bug is that all three
rasterizers were wrong **identically**, so `GPU_PARITY` compared wrong against wrong and
passed. A GL mask stage has a *fourth* way to be silently wrong that none of them have: a
driver that advertises framebuffer fetch and returns stale or zero destination colour turns the
CHECK into a no-op and puts the box back around the player, with no error anywhere.

1. **Host, `make test-gpu`** — `mask-contract-accurate` / `mask-contract-inert-by-default`:
   all sixteen combinations of (set-mask, check-mask, texel STP, destination bit 15) in both
   accuracy states, comparing `psx/dev/gpu.c`'s **observed output** against the prediction of
   the two macros. Not tautological — the prediction comes from the contract, the observation
   from rendering. Both directions were control-tested: weakening `PSX_GPU_MASK_WRITE` to
   `(force)` fails 3 rows; weakening `PSX_GPU_MASK_SKIP` to `(0)` fails 4.
   *(And `psx/dev/gpu.h` is now a prerequisite of the test binary in the Makefile: it was not,
   so the first control run "passed" against a stale binary. A negative control that cannot
   fail is not a control — see the adb-silent-read lesson.)*
2. **On device, at attach** — `gl_mask_selftest()`. Before the first frame, through the real
   draw path: a "set mask" sprite, then a differently coloured one over it with "check mask"
   on (must be rejected, keeping colour **and** bit 15), then the same overdraw one pixel to
   the right **without** the check (must land, with bit 15 **clear** — the control that a
   backend forcing bit 15 on every write would fail). Read back through the ordinary resolve,
   so it also proves bit 15 survives the render-target→host round trip. Failure **refuses the
   attach**; it does not warn and continue.
3. **On device, continuously** — the 1x parity gate now compares bit 15 instead of masking it
   off, with its own `maskbit=` bucket, so a mask disagreement is never reported as a colour
   disagreement.

### Not regressed, checked explicitly

§0.5.9's coverage contract (coverage decided once per native pixel at `pc = vec2(pn)`; the
mask CHECK is a *destination* test placed after coverage, exactly where `gpu.c:1180` and
`gpu_hw_rt.c:170` put it, and is per fragment as `gpu_hw_rt.c` also does). §0.5.12's
mask-from-texel (`stp` is the fetched texel's bit 15, OR-ed across taps by the filtered
fetches, false for untextured — the same rule). §0.5.13's `accurate_prim_size` and §0.5.15's
`u_tex_trunc` are untouched. With the mask bit **off** every one of the changes above reverts
textually: no `#extension`, no `inout`, `u_mask` uniforms 0, the fixed-function blend back, and
the two new flag bits never set.

### Sequencing note

The Xenogears colour-flashing investigation (§0.5.14b) could not be run against the hardware
rasterizer at all while this decline was in place — the GL path never attached. It can be now.

---

## §0.5.14c — Xenogears: the frame is drawn at the wrong Y, and it is the drawing offset

> Instrumentation plus one measured result. The drawing-offset finding below is **measured
> from the two captures already on disk** and is not a hypothesis; the mechanism behind it is
> not yet measured and the census added here is the instrument for it. The colour-flashing
> symptom is **still open** and §0.5.14b's reason stands.

### The measurement that closes symptom 2 (the duplicated frame)

§0.5.14b stopped at "the quad sits at three Y positions, and only `off=` distinguishes a
game-set offset from a mislatched `GP0(E5)`". That is true of capture B taken alone. It is
**not** true once capture A is read the same way, because capture A itemises four BIG quads
per frame instead of one, and four quads over-determine the fit.

A bbox is `vertex.y + off_y`, one equation in two unknowns. The second equation comes from the
double buffer: `disp_start` alternates between the two buffer origins, so the drawing offset
must alternate with it, and the difference between the two parities' bboxes measures the
offset difference directly. In capture A that difference is **exactly 256 on every one of the
80 drawing frames** — the buffer pitch — which pins `off_y ∈ {0, 256}`, anti-phase with
`disp_start`. Reconstructed vertices then drift smoothly (0, ±1, ±2 per frame: a camera pan).

**Two frames break it, and they break it identically.** On `F0025` and `F0199` the
reconstructed vertex jumps by exactly −192 and returns the next frame. Solving instead for the
offset that makes those frames agree with their neighbours gives **`off_y = 64` where it
should be 256**, and the fit is exact rather than approximate:

| | `c=594848` | `c=5a4a4a` | `c=5b4b4b` |
|---|---|---|---|
| F0199 bbox ymin (`up=10`) | 14 | −21 | 8 |
| F0199 vertex if `off_y=64` | **−50** | **−85** | **−56** |
| F0202 vertex (`off_y=0`) | −50 | −85 | −56 |
| F0205 vertex (`off_y=256`) | −50 | −85 | −56 |

Three quads, three frames, exact to the row, over a stretch where the camera is static. F0025
fits the same way against F0028 (−160/−151/−186/194 vs −160/−150/−184/194, the 1–2 row spread
being the real camera drift over three frames).

**`off_x` is untouched.** Matching quads by modulation colour across F0199/F0202 gives
`dx = 0` on all three and `dy = 64` on all three. This is the load-bearing detail: `GP0(E5)`
packs X in bits 0–10 and Y in bits 11–21, so a corrupted or garbage-decoded command word would
move both. Only Y moved, and X survived exactly. **Whatever set that offset was a well-formed
`E5` carrying the same X and a different Y**, which points away from bit-level corruption and
towards the wrong `E5` being in force.

### What selects the bad frames

The anomalous frames are not random and are not a content change. In capture B, three census
fields separate them from their own parity with **zero overlap**:

| | anomalous (ymin=374) | normal, same parity |
|---|---|---|
| `rect` | 3 on 12/12 | 2 on 48/48 |
| `up` (VRAM uploads) | 10 or 11 on 12/12 | 0 or 1 on 48/48 (mean 0.40) |
| `prims` | mean 1650.2 | mean 1637.4 (t = 8.0) |

The selector is the upload burst. Listing every capture-B frame with `up ≥ 5` gives exactly 24
frames, and the quad's position on them is:

* `disp_start=(0,224)`, correct `off_y = 0` → ymin 262, **which is the normal position**;
* `disp_start=(0,0)`, correct `off_y = 224` → ymin 374, i.e. `off_y = 112`.

So an upload burst perturbs the offset only when the correct offset is non-zero, and 0 comes
through 0. Capture A's two anomalies are the same event (`up=16` and `up=10`; every other
drawing frame in that capture has `up=0`).

**The size of the error is not a fixed transform.** Capture A loses 256→64, capture B loses
224→112. Both are power-of-two reductions and both leave 0 at 0, but by different amounts, so
"the offset gets shifted right by k" does not fit two captures with one k. That is the reason
the mechanism is called *not yet measured* rather than guessed at here.

### Why this is the reported symptom, not a curiosity

An offset short by 112 or 192 rows draws the **entire** frame — all ~1650 primitives — that
many rows above where it belongs, which straddles the buffer being displayed. `GP0(02)` is in
absolute VRAM coordinates and ignores the drawing offset (gpu.c, `gpu_cmd_02`), so the clear
still lands on the correct buffer while the geometry does not: one buffer receives a second
complete scene composited on top of what it is currently showing, and the other is left holding
little more than its clear colour. In capture B the error is **exactly half the framebuffer
pitch**, which is the user's description word for word — the frame duplicated *halfway* above
the main frame — and it happens on 12 frames in 120, which is the "clips in and out".

### What is instrumented here, and what each field decides

Four additions to the `gpu_frame_summary` line (§0.5.14). All are counters inside branches
summary mode has already taken; none changes behaviour, and all are inert outside a capture.

| field | decides |
|---|---|
| `e5=<n>[(x,y)@prim …]` | every drawing offset the frame drew with, in order, tagged with the primitive index it took effect at. Distinguishes "the game issued `E5(0,64)` mid-frame" from "the `E5` that should have set 256 never arrived". |
| `unk=<n>` | GP0 words that fell through to the unhandled-opcode case. This is what a **desynchronised command stream** looks like from the inside, and it is the discriminator between losing the stream and executing it faithfully. Note `0x81-0x9f`, `0xa1-0xbf` and `0xc1-0xdf` alias `0x80/0xa0/0xc0` on hardware and currently land here, which would desync us for real. |
| `fillr=(x,y,WxH)` | the largest `GP0(02)` in absolute VRAM coordinates, i.e. **which buffer was actually cleared**, independent of where the geometry went. `fillr` and the geometry disagreeing *is* the wrong-offset frame. |
| `col=<class>:<mean RRGGBB>/<count>/<textured>` | see below. |

### The colour census, and why nothing else can see the flashing

§0.5.14b established that the colour-flashing capture records the colour of **no** whole-screen
primitive: `transp` averages 125.3 while `big` is 4–6, and at most 2 of those ~125 can pass the
size test (≥160×108 in both axes). The other ≥123 are excluded **by size, not by the cap**, so
raising `GPU_SUMMARY_BIG_MAX` cannot reveal one of them. Confirmed again here: across all 80
drawing frames of capture A the itemised BIG colours are 32 stable dark browns plus neutral
`808080`, `fillc` is `283028` on 80/80, and every other census field moves smoothly.

`gpu_dump_census()` is nevertheless handed `colour` for **every** primitive regardless of size.
The census accumulates sum-R/G/B and a count per blend class — class `op` for opaque, `t0`–`t3`
for the four semi-transparent modes — and reports the means. Splitting by class is what makes
it sensitive: a wash laid over a static scene moves one class's mean while the opaque geometry's
mean does not move at all. The textured count rides along because a textured class's mean is a
*modulation* colour, so `808080/N/N` means "the colour is in the texture, not in this field" —
which is itself a decisive negative that redirects to VRAM state.

**Discriminator**, calibrated from capture A: opaque modulation colours drift ≤2 per channel per
drawing frame. A class mean swinging **>16 per channel between consecutive drawing frames is
unambiguous.**

### Reading the next capture

* `grep -oE "dheight=[0-9]+" <file> | sort | uniq -c` — **first, always**. Xenogears is 216;
  a mislabelled Metal Gear Solid capture reads 224 and cost a full round.
* `grep -oE "e5=[0-9]+\[[^]]*\]" <file> | sort | uniq -c` — a normal double-buffered frame
  reports one offset. Anything else is the frame to read.
* `grep -c "unk=0" <file>` against the frame count — anything less is a desync.
* `grep -oE "col=[^ ]* [^ ]* [^ ]* [^ ]* [^ ]*" <file>` — per-class means, frame by frame.
* On the `up=1[01]` frames, `off=` on the BIG lines now states the offset directly rather than
  leaving it to be solved for, which is the confirmation of everything above.

### §0.5.14c(ii) — xg3 settles symptom 2: the offset moved, the geometry did not

The third capture has `off=` printed on every BIG line, so the vertex is no longer solved
for — it is `bbox_ymin - off_y` with both terms measured. Two independent results, and the
second is the stronger of the two.

**The geometry is static across the anomaly.** Tracking the `c=5a4a4a` background quad and the
full-screen tint rect through the transition:

| frame | bbox ymin | `off_y` | vertex | `draw_y1` |
|---|---|---|---|---|
| F0222 | −151 | 0 | **−151** | 0 |
| **F0225** | **−87** | **64** | **−151** | **256** |
| F0228 | −150 | 0 | **−150** | 0 |

F0225's vertex is identical to F0222's and within one row of F0228's. The camera does not move.
Only the offset does. The tint rect gives the same answer independently: its vertex is
`(0,0)-(320,224)` on **every** frame of the capture including F0225 — the game emits it in
screen-relative coordinates with origin 0 — while its bbox sits at 0, 64 or 256 purely
according to `off_y`.

**The game's own invariant is `off_y == draw_y1`, and it holds on 237/240 frames.** `GP0(E5)`
sets the offset and `GP0(E3)` sets the drawing area's top-left; on every normal frame in the
capture the two agree, because they are how the game names the same back buffer. The only
violations are F0225, F0226 and F0227, all reading `off=(0,64) draw=(0,256)-(319,479)`. **The
E3 arrived correctly and the E5 did not**, and they travel in the same command stream.

**What that renders.** Geometry is translated by 64 and then clipped to rows 256–479, so only
vertices with `y ≥ 192` survive — 32 of the tint rect's 224 rows. `fillr=(0,256,320x224)`
confirms the clear went to the correct buffer. The displayed buffer is therefore left holding
its clear colour `283028` across ~86% of its area: a near-solid dark frame, on a scene that is
otherwise identical to its neighbours. `off_y` then persists for two further frames (F0226,
F0227 draw nothing) and is only recovered at F0228, when the game sets the *other* buffer's
offset in the normal course. **The game never notices, because it never issued the bad value.**

### The E5 sequence, and two corrections to §0.5.14c

`e5=6[(0,0)@0 (0,256)@1 (0,0)@1 (0,128)@2 (0,0)@11 (0,64)@12]`, against `e5=2[… (0,256)@1]`
on every normal frame. **The game's correct E5 does arrive, first, at primitive 1, exactly
where it does on a normal frame.** Four more follow it, and 1076 of the frame's 1088
primitives draw under the last of them. Decoded, the added Y values are 0, 128, 0, 64; since
`GP0(E5)` carries Y in bits 11–21, the words are `e5000000`, `e5040000`, `e5000000`,
`e5020000` — one bit walking right, bit 19 → 18 → 17, with zeros interleaved.

Two things asserted earlier in §0.5.14c must be withdrawn:

* **"`off_x` is preserved, so the word cannot be garbage" is vacuous.** Every `E5` in the entire
  capture has `off_x = 0`. A garbage word whose low 11 bits happen to be zero is
  indistinguishable from a correct one on that evidence. The argument is withdrawn.
* **`unk=0` does not prove the command stream is in sync.** `unk` counts words reaching the
  unhandled-opcode case of the *second* dispatch switch only. Opcodes `0x20-0x7f` are taken by
  the first switch (`buf[0] >> 29`) into `gpu_poly`/`gpu_line`/`gpu_rect` and never reach it, so
  a desync landing on polygon opcodes is invisible to this counter. What `unk=0` *plus*
  `prims=1088` against neighbours at 1099 and 1048 does rule out is a **large** desync; a
  four-word one is still open.

### Leading hypothesis, and the measurement that decides it

`psx_dma_do_gpu_request()` (`psx/dev/dma.c`) pushes exactly `BCR_SIZE * BCR_BCNT` words into
GP0 and knows nothing about how many the `GP0(A0)` rectangle in flight actually wants;
`gpu_cmd_a0()` returns to `GPU_STATE_RECV_CMD` the moment its `tsiz` reaches zero. Any word the
block transfer pushes beyond the rectangle is therefore **executed as a command**. That is
consistent with every measured fact: it fires only on upload frames (`up=16`), it produces a
small number of extra commands rather than a storm, it leaves `E3` untouched (E3 rides the
ordering table), and image data pairing a transparent `0x0000` halfword with an `0xe5xx`
halfword decodes as exactly this command. **It is not yet confirmed.**

The discriminator is a transport tag. `st=<n>[RAWWORD@prim:src]` logs every `GP0(E3/E4/E5)`
verbatim with the primitive index and the channel it arrived on — `C` for a CPU store, `O` for
the ordering-table chain (`psx_dma_do_gpu_linked`), `B` for block DMA
(`psx_dma_do_gpu_request`). On the anomalous frame:

* extra `E5`s tagged **`B`** ⇒ surplus upload words executed as commands; fix is in the A0/BCR
  word accounting, and the raw words will be image data;
* tagged **`O`** ⇒ the ordering table genuinely contains them, and the bug is upstream in what
  the game was told;
* tagged **`C`** ⇒ a CPU store, which would mean the game really did issue them.

The raw word also closes the decode question outright: the 11-bit sign extension at
`gpu.c` `case 0xe5` is arithmetically correct for 256 (`0x100 << 21 >> 21 == 0x100`), so if the
logged word is `e5080000` the decode is exonerated and the word is the input.

### Symptom 1 — the colour census reads a clean negative

Across all 80 drawing frames of xg3, size-blind over every primitive:

| class | primitives/frame | max per-channel step between consecutive drawing frames | full-capture range |
|---|---|---|---|
| `op` opaque | ~920 | **1** | R 117-120, G 113-117, B 113-117 |
| `t1` additive `B+F` | ~131 | **4** | R 50-58, G 47-55, B 98-109 |
| `t2` subtractive `B-F` | **3-4** | 17 | R 109-159, G 117-159, B 100-159 |

Against a discriminator of >16 per channel, `op` and `t1` — the two classes that carry
essentially all the geometry — are flat. `t2`'s single 17 is a mean over **three or four**
primitives, where one primitive moving drags the mean, and it is directly attributable: the
itemised BIG tint rect ramps `63a06f → 63a07d → 63a08b → babac3` through the transition, which
is the game's own flash. It is not a wash.

**So no primitive colour swings.** The wash the user reports is therefore not in the command
stream, and the next instrument has to look at VRAM state or at rasteriser output rather than
at the command list. Note the constraint from §0.5.16: the user's earlier three-way bisect could
not distinguish the CPU rasterizer, because the GL path was declining to attach throughout, so
**the CPU rasterizer is still in scope** and so is VRAM accumulation.

---

## §0.5.17 — Texture dumping and replacement on a machine with no textures

**Status: implemented, default off, gated by `tests/gpu_texrep_parity.c`.** Core in
`psx/texrep.{c,h}` + `psx/texrep_png.{c,h}`; hooks in `psx/dev/gpu.{c,h}`,
`frontend/gpu_hw_rt.c` and `frontend/gpu_hw_gl.c`; settings `[video] texture_dump`,
`texture_replacements`, `texture_dir`.

### Why none of the PS2 machinery transfers

A PS2 emulator keys a replacement off a texture **object**: the GS is handed a base pointer, a
width, a height, a format and a palette, and that tuple names a thing that persists. The
PlayStation has no such object. GP0 hands the GPU a texture PAGE (64 halfwords × 256 rows into
the single 1 MB VRAM), a colour depth, a CLUT position and per-vertex UVs, and the "texture" is
whatever bytes happen to occupy the rectangle those UVs sweep at the instant the primitive is
drawn. Ten primitives later the same page holds something else. Address is not identity here;
**content is**, so the key has to be a hash of the bytes.

### The key

64-bit FNV-1a over, in this order:

| # | Bytes |
|---|-------|
| 1 | 9-byte header: depth (0=4bpp, 1=8bpp, 2=15bpp), w lo, w hi, h lo, h hi, `texw_mx`, `texw_my`, `texw_ox`, `texw_oy` |
| 2 | the CLUT — 16 halfwords at 4bpp, 256 at 8bpp, none at 15bpp |
| 3 | the VRAM halfwords backing the sampled rectangle, row by row, only the halfwords the rectangle covers |

Halfwords are folded in as (lo, hi) so the key does not depend on host endianness. The CLUT is
in the key because a palette swap — the same indices, a different palette — is a different
texture and is exactly the case a pack author cares about. The header is in it because a 32×32
region and the 64×16 region sharing its bytes are different textures.

Filenames are `ps1-<16 hex>-<W>x<H>-<bpp>.png`. Everything after the key is documentation: the
loader takes the first 16-hex-digit run in the basename and ignores the rest, so
`ps1-3f2a…-aerith-dress.png` still matches, and a bare `<key>.png` works.

### The texture window, which is the interesting part

`GP0(E2)` rewrites every texcoord as `t = (t AND NOT(mask*8)) OR (offset AND mask)*8`. That is
many-to-one: a polygon whose UVs sweep 0..255 through mask `0xF8` samples eight distinct
columns. Keying the RAW UV bounding box would give the same 8×8 wall a different key in every
game that tiles it over a different span, and the dump would be those eight columns repeated 32
times.

So the rectangle is **folded through the window first**. `psx_texrep_fold_axis()` applies the
transform to every value in the sampled range and takes min/max. It enumerates rather than
reasons, which matters: the mask hardware permits is not always a low-contiguous window —
`mask = 1` gives `NOT(8) = 0xF7`, freeing bits 0-2 and 4-7 but not bit 3 — and any closed-form
derivation gets that wrong. At most 256 iterations per axis, once per keyed primitive, only
when the feature is on. The transform is idempotent, so the sampler can fold an incoming UV
again at fetch time and land inside the image by construction.

### Where the hook lands in each of the three rasterizers

The decision is made **once per primitive**, in `gpu_poly()`/`gpu_rect()`
(`psx/dev/gpu.c`), *before* the backend hook and *before* the software rasterizer. That is what
makes "did all three replace the same texels" answerable at all: they do not each decide.

| Rasterizer | Site | Form |
|---|---|---|
| `psx/dev/gpu.c` (software) | `gpu_fetch_texel()` first statement; polygon path calls `gpu_fetch_texel_f()` | `if (gpu->texrep_bind.img) return psx_texrep_sample(...)` |
| `frontend/gpu_hw_rt.c` (CPU internal-res) | calls straight into the same `gpu_fetch_texel()`; triangle path uses `gpu_fetch_texel_f()`, sprite path passes the `S×S` sub-texel | same branch, same field |
| `frontend/gpu_hw_gl.c` (GLES) | `fetch_repl()` in `kDrawFS`, selected by the per-vertex `a_repl` attribute | atlas `texelFetch`, mapping from `PSX_TEXREP_GLSL` |

The CPU pair share **one** branch on **one** struct field, so they cannot disagree by
construction. The GLES path cannot call C, so the mapping is written once as
`PSX_TEXREP_GLSL` in `psx/texrep.h` and the shader is compiled from that text — the same
construction `PSX_GPU_MASK_GLSL` uses, and for the same reason: §0.5.12, §0.5.13 and §0.5.15
were each one formula written three times.

GL residency is a 2048² RGBA8 shelf-packed atlas, created on first use. No eviction: when a
frame needs more replacement pixels than it holds, the **pending batch is flushed** — the only
thing that still needs those pixels — and the packer starts over. Residency is a generation
counter, so a reset evicts everything without touching anything.

### Cost when disabled

`psx_gpu_t::texrep` is `NULL` and `texrep_bind.img` is `NULL`.

* per primitive — one `if (gpu->texrep)` in `gpu_poly()`/`gpu_rect()`, never taken;
* per texel — one `if (gpu->texrep_bind.img)` in `gpu_fetch_texel()`, a load from a struct the
  function is about to touch anyway and a branch that is never taken;
* per frame — one increment at vblank, guarded by the same NULL;
* per vertex (GL) — 16 bytes of zero in the vertex, and `v_repl.y >> 16 == 0` in the shader.

Nothing allocates, scans, hashes or opens a file. There is no per-frame work and no atlas.

### Interaction with the existing accuracy work

* **§0.5.9 coverage contract** — untouched. The hook is on the texture SAMPLE, downstream of
  every coverage decision, exactly as the `texture_filter` work was (§0.5.10's note).
* **§0.5.12 mask-from-texel** — a replacement returns a BGR555 texel with bit 15 set from its
  alpha, so `stp` and `mask_from_texel` read the same bit they always did.
* **§0.5.13 `accurate_prim_size`** — unaffected; the size cull runs before any fetch.
* **§0.5.15 `accurate_tex_modulate`** — a replacement re-enters at exactly the point a VRAM
  texel would have, so `psx_gpu_modulate_channel()` truncates it identically. **A
  higher-resolution replacement therefore gets more texels, not more colour precision**: every
  one is still a 15-bit value through the same truncating multiply. Deliberate — the render
  target is BGR555 in all three rasterizers, so extra precision would be discarded at the write
  anyway, and keeping the pipeline identical is what lets the round-trip identity below hold.
* **`[video] texture_filter`** — a bound replacement **bypasses the filter** and is
  point-sampled at its own resolution. `psx_gpu_filter_active()` is the shared predicate; the
  shader takes the `repl` branch ahead of `u_filter`. The filter exists to hide the size of a
  native texel; a replacement S times finer has already done that, and filtering it would blur
  detail the author drew. It also avoids making three filter kernels agree over the atlas as
  well as over VRAM.

### The alpha contract

There is no partially transparent PS1 texel; there is bit 15 (STP), which selects whether the
primitive's blend equation applies. So alpha carries that, not a blend factor:

| A | Meaning |
|---|---|
| 0 | the transparent texel (VRAM `0x0000`) — nothing is drawn |
| 255 | opaque, STP = 0 |
| 1..254 | STP = 1, the primitive's blend mode applies |

The dumper writes exactly 0, 128 and 255. 5-bit channels expand as `(v<<3)|(v>>2)` and contract
as `v>>3`, which round-trips all 32 levels exactly — which is what makes the identity below a
real assertion rather than an approximation. One consequence for pack authors: **opaque black
is transparent**, because `000000` packs to `0x0000`, which is the console's transparent texel.
Paint `000008` for a near-black that draws.

### The behavioural test

`make test-texrep` → `TEXREP_PARITY all cases passed`. Nothing in it is a
rasterizer-versus-rasterizer comparison, because that is precisely the blind spot §0.5.12,
§0.5.13 and §0.5.15 all hid in. Every case pins output against the feature's own contract:

| Case | What breaks it |
|---|---|
| `disabled-is-inert` | any cost or behaviour change with both flags off |
| `dump-writes-png` | wrong filename, wrong size, wrong alpha rule; **two files** would mean the key depends on the primitive (quad vs sprite) rather than the texels |
| `roundtrip-is-identical` | the central one. Dump → replace must be **bit-identical to the original frame**, in both CPU rasterizers. A wrong hash, rectangle, sub-texel, alpha rule or colour expansion all break it, and none can break it "the same way in both" because the reference is the original frame |
| `substitution-reaches-both` | the hook not being reached; also proves the REPLACEMENT's alpha decides transparency, since the repaint makes formerly transparent texels draw |
| `wrong-hash-is-ignored` | negative control — the same repainted file under a bogus key must be inert, or "matched" would mean "a file exists" |
| `subtexel-at-scale` | a 2× replacement at internal scale 2 rendering **flat** per texel, i.e. the fractional UV being dropped. Invisible to any per-texel comparison |
| `glsl-matrix` | 2709 samples of `psx_texrep_sample()` against a hand transcription of `PSX_TEXREP_GLSL`, over scales × windows × rectangles × sub-texels |
| `fold-axis` | the window fold, including the non-contiguous `mask = 1` case |
| `png-roundtrip` / `png-rejects-garbage` | the codec: exact round trip, plus every truncation and every single-byte corruption of a real file, clean under ASan and UBSan |

The GLES shader cannot be executed on the build machine. `glsl-matrix` closes that from the
other side — two independent transcriptions of the one text the shader compiles. It is strictly
better than the situation §0.5.12/§0.5.13/§0.5.15 were found in, where the formula existed three
times and no test compared any two of them.

### The PNG codec

`psx/texrep_png.c`: full inflate (stored / fixed / dynamic Huffman) plus the five row filters
for decode; stored-deflate encode with real zlib framing, adler32 and per-chunk crc32. Reads
colour types 0/2/3/4/6 at depth 8, plus indexed at 1/2/4. Interlaced and 16-bit are rejected by
name so the author is told to re-save. Not `psx/thumbnail.c`'s `psx_png_encode_rgb()` — that is
RGB-only, memory-only and has no decoder, and alpha is not optional here.

`psx/` links nothing (see `psx/dev/gpu_backend.h`'s layering contract) and builds for six
platforms; taking a zlib dependency for a default-off feature is not a trade worth making.
Encoding uncompressed costs disk on a dump — a debugging/authoring activity — and buys a
decoder that is the actual requirement.

### The PS2 texture downloader

Removed. `ui/textures/TextureManagerScreen.kt`, `TextureManagerViewModel.kt` and
`TexturePackInstallState.kt` are gone, along with the drawer entry, the pause-menu button, the
`AppRoute.TextureManager` / `InGameScreen.Textures` destinations, five dead `EmuCore/GS` fields
in `config/Settings.kt` and their eight serialize/parse/diff sites, the empty
`NativeApp.reloadTextureReplacements()` and `NativeApp.toggleTextureDumping()` stubs, and 43
orphaned `renderer.*` / `textures.online.*` strings across `I18n.kt` and 19 locale JSONs. It
was a PS2 feature — the online catalog it existed to serve had already been deleted, leaving a
screen whose switches drove `NativeApp.setSetting`, itself an empty stub.

The `TEXTURE_DUMP` hotkey is now **real** and is in `ps1Hotkeys`: it flips `[video]
texture_dump` through the settings store and re-pushes, so the state the hotkey leaves behind is
the state the Video tab shows and the state the next launch restores. There is no live native
toggle by design — one source of truth.

### Where the user finds it

`Settings → Video`, immediately below Texture filtering: **Texture replacements**, **Dump
textures**, a read-only **Texture folder** row that appears whenever either is on and shows the
resolved per-game path, and a **Custom texture folder** override. All four are on a MAIN tab,
not behind a manager screen — which is where the PS2 feature was, and why nobody found it.

The folder is `<data>/textures/<serial>/`, with `dump/` and `replacements/` beneath it. The
serial is resolved in Kotlin (`core/Ps1Textures.kt`), not in the core: the core has never been
told which disc is running, and a shared folder would let one game's pack match another game's
textures whenever two discs happen to hold the same texel bytes.
