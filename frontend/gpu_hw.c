#include "gpu_hw.h"

#ifdef USE_HARDWARE

#include <stdlib.h>

/*
 * ARMSX keeps the PlayStation GPU's 16-bit VRAM authoritative. SDL hardware
 * acceleration is used only to upload, scale, composite, and present that
 * software-accurate image. This compatibility object remains for embedders
 * built against the earlier experimental renderer API; it never bypasses the
 * core rasterizer.
 */
struct armsx_hw_renderer {
    SDL_Renderer* renderer;
    int width;
    int height;
};

bool armsx_hw_renderer_is_supported(void) {
    return true;
}

armsx_hw_renderer_t* armsx_hw_renderer_create(SDL_Renderer* renderer) {
    if (!renderer)
        return NULL;

    armsx_hw_renderer_t* hw = (armsx_hw_renderer_t*)calloc(1, sizeof(*hw));
    if (hw)
        hw->renderer = renderer;
    return hw;
}

void armsx_hw_renderer_destroy(armsx_hw_renderer_t* hw) {
    free(hw);
}

void armsx_hw_renderer_set_renderer(armsx_hw_renderer_t* hw, SDL_Renderer* renderer) {
    if (hw)
        hw->renderer = renderer;
}

void armsx_hw_renderer_set_output_size(armsx_hw_renderer_t* hw, int width, int height) {
    if (!hw)
        return;

    hw->width = width;
    hw->height = height;
}

void armsx_hw_renderer_begin_frame(armsx_hw_renderer_t* hw) {
    (void)hw;
}

void armsx_hw_renderer_end_frame(armsx_hw_renderer_t* hw) {
    (void)hw;
}

SDL_Texture* armsx_hw_renderer_overlay_texture(armsx_hw_renderer_t* hw) {
    (void)hw;
    return NULL;
}

void gpu_hw_render_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge) {
    gpu_render_triangle(gpu, v0, v1, v2, data, edge);
}

#endif
