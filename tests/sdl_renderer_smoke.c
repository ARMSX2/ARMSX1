#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_RENDERER_SMOKE failed stage=init error=%s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "ARMSX SDL renderer smoke",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        64,
        64,
        SDL_WINDOW_HIDDEN
    );
    if (!window) {
        fprintf(stderr, "SDL_RENDERER_SMOKE failed stage=window error=%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE
    );
    if (!renderer) {
        fprintf(stderr, "SDL_RENDERER_SMOKE failed stage=renderer error=%s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RendererInfo info = {0};
    SDL_GetRendererInfo(renderer, &info);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_BGR555,
        SDL_TEXTUREACCESS_STREAMING,
        64,
        64
    );
    uint16_t pixels[64 * 64] = {0};
    pixels[0] = 0x7c00;
    if (!texture ||
        SDL_UpdateTexture(texture, NULL, pixels, 64 * (int)sizeof(uint16_t)) != 0 ||
        SDL_RenderCopy(renderer, texture, NULL, NULL) != 0) {
        fprintf(stderr, "SDL_RENDERER_SMOKE failed stage=texture error=%s\n", SDL_GetError());
        if (texture) SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_RenderPresent(renderer);

    printf(
        "SDL_RENDERER_SMOKE passed driver=%s accelerated=%s target_texture=%s\n",
        info.name ? info.name : "(unknown)",
        (info.flags & SDL_RENDERER_ACCELERATED) ? "true" : "false",
        (info.flags & SDL_RENDERER_TARGETTEXTURE) ? "true" : "false"
    );

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
