/*
    ARMSX PS1 core — save-state preview thumbnails.

    ============================================================================
    WHY THIS IS NOT THE FRONT-END'S SCREENSHOT PATH
    ============================================================================

    frontend/main.cpp already knows how to grab the scanned-out frame, but it
    hands it to SDL_SaveBMP() — the output is a BMP whatever extension the caller
    asked for. Android's BitmapFactory does not reliably decode BMP: it returns
    null with no error, which in a save-state picker is indistinguishable from
    "this slot has no thumbnail". So the preview that goes INTO a state is
    encoded here, as a PNG, by an encoder small enough to live in the core.

    The encoder is deliberately self-contained (no zlib, no miniz, no vendored
    stb): the core builds for six platforms and none of them should grow a new
    third-party dependency to get a 320x240 preview. It emits a single fixed-
    Huffman DEFLATE block with greedy LZ77 matching, which is enough to get a
    game frame down to roughly a quarter of its raw size.

    ============================================================================
    WHAT IS CAPTURED
    ============================================================================

    The frame the host is scanning out, not the whole of VRAM: the same source
    selection frontend/main.cpp's updateTexture()/saveScreenshot() make, so the
    thumbnail matches what was on screen.

      - 16bpp (the normal case) reads BGR555, either from gpu->vram at the
        display origin or, when a hardware rasterizer backend is installed, from
        its internal-resolution render target (which is plain host memory —
        psx_gpu_get_display_surface() hands back the already-offset pointer).
      - 24bpp scanout (FMV playback) reinterprets the same bytes as packed
        RGB888 and therefore always reads native VRAM: the upscaled target does
        not contain those bytes (HW_RENDERER_DESIGN.md 4.4).
      - A display window that would run off the bottom of VRAM falls back to the
        VRAM origin, exactly as saveScreenshot() does.

    The result is box-downscaled by an integer factor to at most
    PSX_THUMBNAIL_MAX_W x PSX_THUMBNAIL_MAX_H, so a 1x native frame is stored
    pixel-for-pixel and a 4x internal-resolution frame is averaged down instead
    of storing sixteen times the pixels.

    ============================================================================
    THREADING
    ============================================================================

    psx_thumbnail_capture_png() reads the GPU's framebuffer, so it must run
    where that framebuffer is coherent: the emulation thread. It is called from
    psx_save_state_to_memory(), which is already bound to that thread by the
    psx_state_request_slot() handshake (state.h), so there is nothing extra for
    a caller to arrange.
*/

#ifndef PSX_THUMBNAIL_H
#define PSX_THUMBNAIL_H

#include <stdint.h>
#include <stddef.h>

struct psx_t;

/* Upper bound on the stored preview. 384 covers the widest native PS1 mode
   (368, reported as 384) without downscaling it, and 288 leaves the common
   240- and 256-line modes untouched too, so the usual case is a byte-exact
   copy of the scanned-out frame. */
#define PSX_THUMBNAIL_MAX_W 384
#define PSX_THUMBNAIL_MAX_H 288

/* Encodes an RGB888 image (tightly packed, w*h*3 bytes) as a PNG.

   On success returns 0 and hands back a malloc()'d buffer in *out_data that the
   caller frees, and its length in *out_size. Returns non-zero and leaves both
   outputs untouched-but-cleared on a bad argument or an allocation failure. */
int psx_png_encode_rgb(const uint8_t* rgb, int width, int height,
                       void** out_data, size_t* out_size);

/* Grabs the currently scanned-out frame and encodes it as above, reporting the
   downscaled size it settled on through out_width/out_height (either may be
   NULL). Emulation thread only. Returns 0 on success; non-zero simply means "no
   preview this time", which every caller has to tolerate — a state without one
   is still a perfectly good state. */
int psx_thumbnail_capture_png(struct psx_t*, void** out_data, size_t* out_size,
                              int* out_width, int* out_height);

#endif
