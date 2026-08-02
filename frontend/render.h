/*
    ARMSX — presentation backend abstraction.

    The PlayStation GPU stays a pure software rasterizer (psx/dev/gpu.c, 16-bit VRAM
    authoritative). Nothing in here rasterizes PS1 primitives. This layer only owns the
    *presentation* half: create a drawable/context on the host window, upload the emulated
    framebuffer once per frame, scale it to the output with the requested aspect/filter,
    and present.

    Backends:
      * SDL          — SDL_Renderer (software or accelerated). The historical path; also the
                       one used when an embedder hands us a ready-made SDL_Renderer.
      * OPENGL       — GLES 3.0 (Android) / GL 3.3 core (desktop fallback). Real GPU present.
      * VULKAN       — instance/device/swapchain + staging upload + vkCmdBlitImage present.
                       EXPERIMENTAL: compile-verified only, see frontend/render_vk.cpp.

    All entry points are safe to call with a NULL renderer.
*/

#ifndef ARMSX_RENDER_H
#define ARMSX_RENDER_H

#include <stdbool.h>

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum armsx_render_backend {
    ARMSX_RENDER_BACKEND_SDL_SOFTWARE = 0,
    ARMSX_RENDER_BACKEND_SDL_ACCELERATED = 1,
    ARMSX_RENDER_BACKEND_OPENGL = 2,
    ARMSX_RENDER_BACKEND_VULKAN = 3
} armsx_render_backend_t;

typedef struct armsx_render_config {
    armsx_render_backend_t backend;
    bool vsync;
    bool linear_filter;
} armsx_render_config_t;

/* Per-frame presentation state. `aspect` is the target display aspect (width / height);
   <= 0 falls back to the uploaded frame's own aspect. */
typedef struct armsx_render_frame_params {
    bool stretch;
    bool linear_filter;
    float aspect;
    /* Snap the destination to a WHOLE multiple of the source resolution, so every emulated
       pixel becomes an exact NxN block and none is duplicated or dropped. That is the whole
       point: at a fractional scale some rows are 3 device pixels tall and their neighbours 4,
       which reads as uneven shimmer on scrolling 2D. Ignored while `stretch` is set (the user
       asked to fill the window, which cannot generally be an integer multiple) and whenever
       the window is smaller than one full source frame. */
    bool integer_scaling;

    /* PORTRAIT layout. In portrait the image is TOP-aligned rather than centred, so the space
       below it belongs to the on-screen controls instead of having them sit on the game.
       portrait_top_inset is the punch-hole/notch height in surface pixels: the render starts
       below it, because a centred-then-shifted image ends up under the camera.
       Ignored in landscape, where the cutout is on a side and moving vertically helps nothing. */
    bool portrait_top;
    int  portrait_top_inset;

    /* ---- [video] overscan_crop -----------------------------------------------------------
       Sub-rectangle of the uploaded/adopted frame to present, in TEXTURE pixels (so already
       multiplied by the internal scale by the time it gets here). All four zero means the
       whole texture, which is the default and what every caller wrote before this existed.

       Cropping does NOT change `aspect`: the point of an overscan crop is to zoom the kept
       region up to the same destination rect, not to letterbox a smaller picture. */
    int crop_x, crop_y, crop_w, crop_h;

    /* ---- [video] display_rotation ---------------------------------------------------------
       QUARTER TURNS CLOCKWISE, 0..3. Applied by the present backend to the finished image;
       armsx_render_compute_dst() transposes the destination rect for the odd values, so a
       4:3 frame at 90 degrees letterboxes as 3:4.

       Not every backend can do this — vkCmdBlitImage has no rotation — and the ones that
       cannot ignore it and say so once in the log rather than presenting a wrong frame. */
    int rotation;
} armsx_render_frame_params_t;

typedef struct armsx_renderer armsx_renderer_t;

/* ---- backend metadata ------------------------------------------------------------- */

const char* armsx_render_backend_name(armsx_render_backend_t backend);
const char* armsx_render_backend_token(armsx_render_backend_t backend);
bool armsx_render_backend_compiled_in(armsx_render_backend_t backend);

/* SDL_WINDOW_* bits the backend needs on the window it will be attached to. Must be OR-ed
   into the flags passed to SDL_CreateWindow(). */
Uint32 armsx_render_window_flags(armsx_render_backend_t backend);

/* Sets the SDL GL/Vulkan attributes the backend wants. MUST run before SDL_CreateWindow(). */
void armsx_render_prepare_window_attributes(armsx_render_backend_t backend);

/* ---- lifetime --------------------------------------------------------------------- */

/* init(window, config). Returns NULL on failure; the caller is expected to fall back to a
   lower backend. */
armsx_renderer_t* armsx_renderer_create(armsx_render_backend_t backend,
                                        SDL_Window* window,
                                        const armsx_render_config_t* config);

/* Adopt an SDL_Renderer created by an embedder (the Android in-process host does this).
   The returned renderer never destroys the borrowed SDL_Renderer. */
armsx_renderer_t* armsx_renderer_create_from_sdl(SDL_Renderer* renderer);

/* shutdown() */
void armsx_renderer_destroy(armsx_renderer_t* renderer);

/* ---- queries ---------------------------------------------------------------------- */

armsx_render_backend_t armsx_renderer_backend(const armsx_renderer_t* renderer);
const char* armsx_renderer_driver_name(const armsx_renderer_t* renderer);
bool armsx_renderer_is_accelerated(const armsx_renderer_t* renderer);

/* Non-NULL only for the SDL backend. Lets code that genuinely needs an SDL_Renderer
   (diagnostics) keep working; everything on the hot path must go through this header. */
SDL_Renderer* armsx_renderer_sdl(const armsx_renderer_t* renderer);

/* ---- frame path ------------------------------------------------------------------- */

void armsx_renderer_resize(armsx_renderer_t* renderer, int width, int height);
void armsx_renderer_output_size(const armsx_renderer_t* renderer, int* width, int* height);
void armsx_renderer_set_vsync(armsx_renderer_t* renderer, bool enabled);

/* upload_frame(). `pitch` is in bytes and may exceed width * bpp (the PS1 framebuffer is
   addressed with a 2048-byte VRAM stride). `dirty_first_row`/`dirty_last_row` describe an
   inclusive row range; pass (0, height - 1) for a full upload, or last < first for "no
   change". Supported formats for the GPU backends: SDL_PIXELFORMAT_BGR555 and
   SDL_PIXELFORMAT_RGB24. The SDL backend accepts anything SDL does. */
bool armsx_renderer_upload_frame(armsx_renderer_t* renderer,
                                 const void* pixels,
                                 int width,
                                 int height,
                                 int pitch,
                                 Uint32 sdl_format,
                                 int dirty_first_row,
                                 int dirty_last_row);

/* present() */
void armsx_renderer_present(armsx_renderer_t* renderer, const armsx_render_frame_params_t* params);

/* Present a black frame (no session running) so an embedded surface keeps getting posted. */
void armsx_renderer_present_blank(armsx_renderer_t* renderer);

/* Present an already-GPU-resident frame instead of uploading one, so an upscaled renderer
   need not glReadPixels its render target back and re-upload it every frame — on a tiler
   that readback is a full pipeline sync, and it is a second S^2 cost on top of rasterising.

   `texture` names a GL_TEXTURE_2D and is only meaningful while the active backend is
   ARMSX_RENDER_BACKEND_OPENGL: it lives in the presentation context's object namespace, so
   the caller MUST have created it on that same context. `sdl_format` describes the contents
   and takes the values upload_frame() accepts (SDL_PIXELFORMAT_BGR555 in GL_RG8, or
   SDL_PIXELFORMAT_RGB24 in GL_RGB8). Texel row 0 is the TOP row, matching upload_frame().

   `width`/`height` are the texture's real pixel dimensions and feed
   armsx_render_compute_dst() exactly as an uploaded texture's do, so an upscaled texture
   letterboxes identically to its 1x equivalent.

   The adoption lasts until the next upload_frame(), or until texture 0 is passed; either
   returns the backend to the CPU path. Returns false and changes NOTHING when the active
   backend cannot do this — that is the caller's cue to keep using upload_frame(), not a
   fatal error, because the present backend is user-selectable at runtime. */
bool armsx_renderer_adopt_gl_texture(armsx_renderer_t* renderer,
                                     unsigned int texture,
                                     int width,
                                     int height,
                                     Uint32 sdl_format);

/* ---- Android surface hand-off ------------------------------------------------------ */

/* Register the raw ANativeWindow the Compose host owns. When set, the OPENGL backend binds
   EGL straight to it (no SDL video driver involved) and the VULKAN backend builds its
   VkSurfaceKHR from it. Pass NULL to clear. Safe to call from any thread before the
   renderer is created. */
void armsx_render_set_native_window(void* native_window, int width, int height);
void* armsx_render_native_window(void);

/* Increments every time the host publishes a window, including the null on surfaceDestroyed.
   A renderer records this when it builds its window surface and re-checks it each frame; a
   change means the ANativeWindow it is presenting to is gone and the surface must be rebuilt
   (see armsx_render_set_native_window for why nothing else can detect this). */
unsigned long armsx_render_native_window_generation(void);

/* The Surface geometry the host reported alongside the window, in the app's own (rotated)
   coordinate space — i.e. 1920x1080 for a landscape-locked activity, whatever the panel's
   native orientation is. Returns false when no window has been registered.

   This is the only orientation-authoritative size a GPU backend has: everything queried back
   out of the window (ANativeWindow_getWidth, VkSurfaceCapabilitiesKHR::currentExtent) is
   whatever the last producer asked for, and its orientation relative to the display is
   driver-dependent when currentTransform is ROTATE_90/270. */
bool armsx_render_native_window_size(int* width, int* height);

/* True once a GPU backend has taken ownership of the registered ANativeWindow. The host's
   CPU blit bridge must stop posting frames while this is true. */
bool armsx_render_native_window_claimed(void);

/* The size the host's CPU blit bridge renders at, for a Surface of `surface_width` x
   `surface_height`, capped to `max_height` on the short axis (aspect preserved, both axes
   rounded down to even). `max_height <= 0` means the default cap.

   Lives here rather than in android_jni.cpp because it is half of a TWO-SIDED contract and
   the other half is the ANativeWindow's buffer geometry. The bridge blits its framebuffer
   into the dequeued buffer with an unscaled row copy, so the two sizes MUST agree; when they
   do not, the picture lands 1:1 in a corner of a larger buffer and the rest stays black. That
   is not hypothetical — it is the bug this function was extracted for, and it is also why
   this is a pure rule with a host-side gate (tests/present_dst_rect.c) instead of Android-only
   code that can only be checked by shipping it. */
void armsx_render_host_framebuffer_size(int surface_width,
                                        int surface_height,
                                        int max_height,
                                        int* out_width,
                                        int* out_height);

/* Default cap for armsx_render_host_framebuffer_size(). 1080p costs the software rasterizer
   and the per-frame row copy 2.25x what 720p does for no visible gain at PS1 source
   resolutions, so the bridge renders at 720p and lets SurfaceFlinger scale. */
#define ARMSX_RENDER_HOST_FB_MAX_HEIGHT 720

/* ---- OpenGL driver selection --------------------------------------------------------- */

/* Which GLES implementation the OPENGL backend binds to. ANGLE is Google's GLES-on-Vulkan
   translator, shipped in the APK as libEGL_angle.so + libGLESv2_angle.so; it is an
   implementation of the same backend, not a backend of its own, which is why it lives here
   rather than in armsx_render_backend_t.

   ANGLE advertises GLES *3.1*, not 3.2. This backend targets GLES 3.0 (`#version 300 es`,
   no compute, no geometry/tessellation, no framebuffer-fetch, no ES-3.2 entry point in the
   loader table), so it is inside ANGLE's envelope with a full minor version to spare.
   Anything added above ES 3.0 here needs an ES-3.1 fallback or an explicit gate. */
typedef enum armsx_render_gl_driver {
    ARMSX_RENDER_GL_DRIVER_SYSTEM = 0,
    ARMSX_RENDER_GL_DRIVER_ANGLE = 1
} armsx_render_gl_driver_t;

void armsx_render_set_gl_driver(armsx_render_gl_driver_t driver);
armsx_render_gl_driver_t armsx_render_gl_driver(void);
const char* armsx_render_gl_driver_token(armsx_render_gl_driver_t driver);

/* Applies a choice ONLY if nothing explicit has been selected yet. Two sources feed this and
   they arrive in the wrong order: the host UI pushes its pick through JNI before the VM
   thread starts, and settings.toml is not parsed until afterwards, inside the core. The file
   is therefore the DEFAULT (this call) and the UI is the OVERRIDE
   (armsx_render_set_gl_driver). */
void armsx_render_set_gl_driver_default(armsx_render_gl_driver_t driver);

/* ---- active backend reporting --------------------------------------------------------- */

/* Name of the backend that actually survived the fallback ladder, for the OSD and the
   driver UI — never what was requested. Copies at most `size` bytes (always NUL-terminated)
   and returns the length written. Empty while no renderer is up.

   Shapes: "Vulkan (Adreno (TM) 740)", "Vulkan (Adreno (TM) 740, Turnip)",
           "OpenGL ES (ANGLE)", "OpenGL ES (system)", "SDL accelerated (opengles2)",
           "Software". */
int armsx_render_active_name(char* buffer, int size);

/* ---- Vulkan custom-driver seam ------------------------------------------------------ */

/* Replaces the default dlopen() of the system Vulkan loader. This is the drop-in point for
   adrenotools: hand back the handle from adrenotools_open_libvulkan(). Must be called
   before the Vulkan backend is created. */
typedef void* (*armsx_vk_library_open_fn)(void* user);
void armsx_render_set_vulkan_loader(armsx_vk_library_open_fn open_fn, void* user);

#ifdef __cplusplus
}
#endif

#endif
