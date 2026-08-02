/*
    ARMSX — internal contract between frontend/render.cpp (dispatch) and the individual
    presentation backends. Not part of the public renderer API; nothing outside
    frontend/render*.cpp should include this.
*/

#ifndef ARMSX_RENDER_INTERNAL_H
#define ARMSX_RENDER_INTERNAL_H

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct armsx_render_ops {
    const char* (*driver_name)(armsx_renderer_t* self);
    bool (*is_accelerated)(armsx_renderer_t* self);
    SDL_Renderer* (*sdl_handle)(armsx_renderer_t* self);
    void (*resize)(armsx_renderer_t* self, int width, int height);
    void (*output_size)(armsx_renderer_t* self, int* width, int* height);
    void (*set_vsync)(armsx_renderer_t* self, bool enabled);
    bool (*upload_frame)(armsx_renderer_t* self,
                         const void* pixels,
                         int width,
                         int height,
                         int pitch,
                         Uint32 sdl_format,
                         int dirty_first_row,
                         int dirty_last_row);
    void (*present)(armsx_renderer_t* self, const armsx_render_frame_params_t* params);
    void (*present_blank)(armsx_renderer_t* self);
    void (*shutdown)(armsx_renderer_t* self);
    /* Take an already-GPU-resident frame instead of a CPU upload, so an upscaled renderer
       does not have to glReadPixels its render target back and re-upload it every frame
       (a full pipeline sync on a tiler, and a second S^2 cost on top of rasterisation).
       NULL on backends that cannot: the dispatcher then returns false and the caller keeps
       using upload_frame().

       MUST STAY LAST. kGlOps / kVkOps / kSdlOps are positional aggregate initialisers, so
       inserting a slot anywhere above silently reassigns every function pointer after it.
       Appending lets the two backends that do not implement this omit it entirely and be
       value-initialised to nullptr. */
    bool (*adopt_gl_texture)(armsx_renderer_t* self,
                             unsigned int texture,
                             int width,
                             int height,
                             Uint32 sdl_format);
} armsx_render_ops_t;

/* Every backend's state object embeds this header as its first member and points `impl`
   back at itself. Going through `impl` rather than reinterpret_cast-ing the base pointer
   keeps the up-cast well-defined even for backend structs that are not standard-layout
   (the GL/Vulkan ones hold std::string / std::vector members). */
struct armsx_renderer {
    const armsx_render_ops_t* ops;
    armsx_render_backend_t backend;
    void* impl;
};

/* Shared destination-rect math. This is a verbatim port of the aspect/stretch/letterbox
   logic the SDL_RenderCopy path used, so every backend letterboxes identically. */
void armsx_render_compute_dst(int output_width,
                              int output_height,
                              int source_width,
                              int source_height,
                              const armsx_render_frame_params_t* params,
                              SDL_Rect* out_dst);

/* Number of bytes one source pixel occupies for a format a GPU backend understands.
   Returns 0 for unsupported formats. */
int armsx_render_source_bytes_per_pixel(Uint32 sdl_format);

void armsx_render_log(const char* category, const char* fmt, ...);

/* Backend constructors. Each returns NULL when unavailable. */
armsx_renderer_t* armsx_render_create_sdl(SDL_Window* window,
                                          const armsx_render_config_t* config);
armsx_renderer_t* armsx_render_adopt_sdl(SDL_Renderer* renderer);
armsx_renderer_t* armsx_render_create_gl(SDL_Window* window,
                                         const armsx_render_config_t* config);
armsx_renderer_t* armsx_render_create_vk(SDL_Window* window,
                                         const armsx_render_config_t* config);

void armsx_render_gl_prepare_attributes(void);

/* Set by a GPU backend that binds itself to the registered ANativeWindow. */
void armsx_render_set_native_window_claimed(bool claimed);

/* Published by each backend once it is genuinely up, and cleared on teardown. This is what
   armsx_render_active_name() hands the UI, so it must describe what is REALLY running —
   a custom Vulkan driver that failed to load and fell back to the system loader has to be
   distinguishable here, or the driver manager lies. */
void armsx_render_set_active_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif
