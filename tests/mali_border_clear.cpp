// Uses the production SDL clear implementation with unused frontend hooks stubbed.
#define SDL_MAIN_HANDLED
#include "frontend/render_sdl.cpp"
#include <cassert>
#include <cstdio>
#include <vector>

extern "C" {
const armsx_gpu_profile_t* armsx_gpu_profile_get(void) { std::abort(); }
void armsx_render_log(const char*, const char*, ...) { std::abort(); }
void psxe_diag_pacingf(const char*, ...) { std::abort(); }
void armsx_render_set_active_name(const char*) { std::abort(); }
void armsx_render_compute_dst(int, int, int, int, const armsx_render_frame_params_t*, SDL_Rect*) {
    std::abort();
}
}

static std::vector<unsigned char> Render(SDL_Renderer* renderer, SDL_Surface* surface,
                                         SDL_Texture* texture, SDL_Rect dst,
                                         bool optimized, bool mali, int rotation) {
    // Poison every pixel to expose stale borders across aspect/size transitions.
    SDL_SetRenderDrawColor(renderer, 213, 37, 99, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    if (optimized) {
        SdlRenderer self;
        self.renderer = renderer;
        self.mali_native_pixels = mali;
        ClearBackground(&self, dst, surface->w, surface->h, rotation);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
    }
    SDL_BlendMode blend;
    assert(SDL_GetRenderDrawBlendMode(renderer, &blend) == 0 && blend == SDL_BLENDMODE_ADD);
    assert(SDL_RenderCopyEx(renderer, texture, nullptr, &dst, rotation * 90.0,
                            nullptr, SDL_FLIP_NONE) == 0);
    SDL_RenderPresent(renderer);
    const auto* pixels = static_cast<const unsigned char*>(surface->pixels);
    return {pixels, pixels + surface->pitch * surface->h};
}

int main() {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 682, 512, 32, SDL_PIXELFORMAT_RGBA32);
    assert(surface);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    assert(renderer);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_STREAMING, 32, 24);
    assert(texture);
    unsigned char pixels[32 * 24 * 4];
    for (unsigned i = 0; i < sizeof(pixels); ++i) pixels[i] = (i % 4 == 3) ? 255 : i % 251;
    assert(SDL_UpdateTexture(texture, nullptr, pixels, 32 * 4) == 0);
    assert(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) == 0);
    // Actual Mali dimensions, all border combinations, out-of-bounds fallback,
    // and both filtering modes. Rotated/non-Mali frames must retain a full clear.
    const SDL_Rect cases[] = {{0,0,682,511}, {0,0,682,512}, {1,1,680,510},
        {0,90,682,300}, {120,0,400,512}, {20,40,320,240}, {-1,0,683,512}};
    unsigned checks = 0;
    for (auto dst : cases) for (int linear = 0; linear < 2; ++linear)
        for (int rotation = 0; rotation < 4; ++rotation) for (int mali = 0; mali < 2; ++mali) {
            assert(SDL_SetTextureScaleMode(texture, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest) == 0);
            auto expected = Render(renderer, surface, texture, dst, false, mali, rotation);
            auto actual = Render(renderer, surface, texture, dst, true, mali, rotation);
            assert(expected == actual);
            ++checks;
        }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    std::printf("%u production SDL border-clear pixel comparisons passed\n", checks);
}
