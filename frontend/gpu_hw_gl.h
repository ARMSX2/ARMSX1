#ifndef ARMSX_GPU_HW_GL_H
#define ARMSX_GPU_HW_GL_H

/*
    ARMSX — GLES 3.0 internal-resolution rasterizer backend.

    Implements the psx/dev/gpu_backend.h ABI on the GPU. Same vtable as the CPU backend in
    frontend/gpu_hw_rt.c, so it inherits every hook site, the config plumbing and the
    coordinate model that tests/gpu_renderer_parity.c pins down — see
    the backend for the decision record.

    WHY GLES AND NOT VULKAN
    -----------------------
    frontend/render_vk.cpp exports exactly one symbol; its device, queue, queue family and
    command pool are all in an anonymous namespace and no external-memory extension is
    enabled, so a Vulkan rasterizer needs either a large brokered accessor or a second
    device. frontend/render_gl.cpp's EGL context is already current on the emulation thread
    (runVMThread -> external_main_ex -> main.cpp drives psx_update() and present from one
    thread), and ANGLE — which is GLES *on Vulkan* — is bundled and selectable, so "GL" here
    still runs on the device's Vulkan driver when the ANGLE provider is picked.

    Hard requirement: this backend only has a context when the PRESENTATION backend is
    OPENGL. armsx_hw_gl_create() probes for one and returns NULL if there is none, which
    leaves the caller on the software (or CPU) rasterizer.

    WHY THE SOFTWARE SHADOW IS STILL SET
    ------------------------------------
    PSX_GPU_BACKEND_SOFTWARE_SHADOW keeps gpu->vram authoritative, so the software
    rasterizer runs alongside. That costs one constant unit of CPU — exactly what the
    software renderer costs today — and in exchange:

      * the S^2 CPU cost of the internal-resolution CPU backend disappears entirely,
        which is the whole reason upscaling was unusable;
      * GP0(C0) readback never stalls and never needs a fallback ladder;
      * textures come from a native-resolution mirror of gpu->vram, which is ALWAYS
        correct, so the backend collapses from two-way ownership to a one-way
        upload and dirty tracking becomes an optimisation, not a correctness requirement.

    The visible consequence, so nobody debugs it later: render-to-texture content is sampled
    at NATIVE resolution. Dropping the shadow is the next optimisation, not this one.
*/

#include <stdint.h>

#include "../psx/dev/gpu.h"

#ifdef USE_HARDWARE

#include "../psx/dev/gpu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same ceiling as the CPU backend. 1024*8 = 8192 is also the GL_MAX_TEXTURE_SIZE floor we
   can rely on; create() clamps against the real limit as well. */
#define ARMSX_HW_GL_MAX_SCALE 8

/* Returns NULL when there is no usable GL context, when the entry points cannot be
   resolved, or when any resource creation fails. Every one of those paths logs why —
   a silent fallback to the software rasterizer is the failure mode this whole file exists
   to avoid. The GPU does not take ownership. */
psx_gpu_backend_t* armsx_hw_gl_create(psx_gpu_t* gpu, int scale);

/* NULL-safe. Call psx_gpu_set_backend(gpu, NULL) first. */
void armsx_hw_gl_destroy(psx_gpu_backend_t* backend);

/*
    Whether this backend may serve a session that has PSX_GPU_ACCURACY_MASK_BIT on.

    A PURE PREDICATE, extracted from armsx_hw_gl_create() so it can be tested without a GL
    context — the same reason tests/gpu_profile_rules.c exists for gpu_profile.c: the
    interesting inputs are hardware nobody here owns, so the rule is otherwise unverifiable
    until a user reports a regression. tests/gpu_rasterizer_select.c pins the truth table.

    WHY THIS IS A SEPARATE FUNCTION AND NOT AN `if` INSIDE create()

    Which rasterizer gets CHOSEN had no test at all. Every gate in tests/gpu_renderer_parity.c
    compares rasterizer OUTPUT, so 23 of them stayed green through an evening in which this
    backend started attaching on a device where it had always declined, and the PS1 BIOS lost
    the ability to draw its own text. Output parity cannot see a selection bug; only this can.

    `opt_in` is the ARMSX_GL_MASK_BIT escape hatch. The framebuffer-fetch mask path remains
    opt-in because the mask-from-texel fixes were validated against the CPU rasterizer.
    Returns 1 only when the GL path may attach.
*/
int armsx_hw_gl_mask_bit_supported(int have_fbfetch, int driver_trusted, int is_angle,
                                   int opt_in);

/* Reads ARMSX_GL_MASK_BIT. Exact "1" enables; anything else (including unset) does not.
   Split out so the environment-to-decision chain is testable on the host. */
int armsx_hw_gl_mask_bit_opt_in(void);

/*
    Whether an unavailable GLES rasterizer should fall back to the scaled CPU backend.

    `hardware` (mode 1) at 1x must return to the original software rasterizer: the CPU
    internal-resolution backend adds work without adding resolution there and can turn a
    full-speed game into a 60-70% one. At 2x+ it remains the useful API-independent upscale
    fallback. `hardware-cpu` (mode 2) is explicit and always honours the request;
    `hardware-gl` (mode 3) never substitutes a different hardware backend.
*/
int armsx_hw_gl_use_cpu_fallback(int rasterizer_mode, int internal_scale);

/*
    [video] texture_filter / downsample / line_detect — the three video options this backend
    owns. Deliberately NOT a create() parameter and NOT stored on the instance: the backend
    is destroyed and recreated behind the frontend's back (checkRasterizerHealth, a scale
    change), and an option living on the instance would silently revert there.

    Takes no backend pointer for the same reason — it is valid before one exists, after one
    dies, and while one runs. Safe from any thread; see the note in gpu_hw_gl.c.

      texture_filter  0 nearest (default) · 1 bilinear · 2 xBR-style. Textured POLYGONS only.
      downsample      0/1 off (default) · 2..8 requested box factor; the effective factor is
                      the largest divisor of the internal scale that is <= this, so it is
                      1 (off) at 1x.
      line_detect     0 disabled (default) · 1 quads · 2 basic.

    Out-of-range values are coerced to the default rather than rejected.
*/
void armsx_hw_gl_set_video_options(int texture_filter, int downsample, int line_detect);

/* Non-zero once the backend has hit an unrecoverable GL error and disabled itself. The
   frontend polls this per frame and swaps back to a working rasterizer — see
   the backend: where the GPU path cannot serve a game, it falls back
   explicitly and logged, never silently. */
int armsx_hw_gl_failed(const psx_gpu_backend_t* backend);

/* Human-readable one-liner describing the last create/teardown outcome. Never NULL. */
const char* armsx_hw_gl_status(void);

/* The brokered seam (the backend), caller side.

   Resolves this frame's display region into the backend's packed-BGR555 scanout texture and
   hands THAT texture to the present layer through armsx_renderer_adopt_gl_texture(), so the
   frame never leaves the GPU: no glReadPixels, no re-upload, and — the expensive half on a
   tiler — no full pipeline sync per frame. Both costs are S^2, which is why 3x collapsed
   while 2x was free.

   Returns 1 only when the present layer adopted the texture, and the caller MUST then skip
   armsx_renderer_upload_frame() for this frame. Returns 0 whenever the seam is not usable —
   a non-OpenGL present backend, a display-disabled frame, a backend that had to make its own
   GL context, or a failed backend — and 0 is never an error: psx_gpu_get_display_surface()
   still produces the same pixels through the readback path, which stays the fallback for
   every present backend that cannot take a texture.

   `renderer` is an armsx_renderer_t* (frontend/render.h); it is spelled as the struct here
   so this header does not have to pull in SDL. */
struct armsx_renderer;
int armsx_hw_gl_present_texture(psx_gpu_backend_t* backend, struct armsx_renderer* renderer);

#ifdef __cplusplus
}
#endif

#endif

#endif
