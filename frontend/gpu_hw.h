#ifndef ARMSX_GPU_HW_H
#define ARMSX_GPU_HW_H

#include <stdbool.h>

#include <SDL.h>

#include "../psx/dev/gpu.h"

#ifdef USE_HARDWARE
typedef struct armsx_hw_renderer armsx_hw_renderer_t;

#ifdef __cplusplus
extern "C" {
#endif

bool armsx_hw_renderer_is_supported(void);
armsx_hw_renderer_t* armsx_hw_renderer_create(SDL_Renderer* renderer);
void armsx_hw_renderer_destroy(armsx_hw_renderer_t* hw);
void armsx_hw_renderer_set_renderer(armsx_hw_renderer_t* hw, SDL_Renderer* renderer);
void armsx_hw_renderer_set_output_size(armsx_hw_renderer_t* hw, int width, int height);
void armsx_hw_renderer_begin_frame(armsx_hw_renderer_t* hw);
void armsx_hw_renderer_end_frame(armsx_hw_renderer_t* hw);
SDL_Texture* armsx_hw_renderer_overlay_texture(armsx_hw_renderer_t* hw);
void gpu_hw_render_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge);

#ifdef __cplusplus
}
#endif
#endif

#endif
