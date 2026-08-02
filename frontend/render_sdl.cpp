/*
    ARMSX — SDL_Renderer presentation backend.

    This is a behaviour-preserving lift of what frontend/main.cpp used to do inline:
    SDL_CreateTexture(STREAMING) + dirty-row SDL_UpdateTexture + SDL_RenderCopy into a
    letterboxed destination rect + SDL_RenderPresent. It stays the default and the fallback
    for every platform, and it is also the shape an embedder's SDL_Renderer is adopted into
    (the Android in-process host hands us a software renderer over an SDL_Surface).
*/

#include "render_internal.h"

#include <cstring>

namespace {

struct SdlRenderer {
    armsx_renderer_t base;

    SDL_Renderer* renderer = nullptr;
    bool owns_renderer = false;

    SDL_Texture* texture = nullptr;
    int texture_width = 0;
    int texture_height = 0;
    Uint32 texture_format = SDL_PIXELFORMAT_UNKNOWN;
    int scale_mode = -1; /* -1 unset, 0 nearest, 1 linear */

    char driver_name[64] = {0};
    bool accelerated = false;

    /* Last logged present geometry. The GL backend has always traced `out=... dst=...` and this
       one traced nothing, so when the picture came out inset there was no way to tell whether
       the destination rect was wrong or the thing it was measured against was — and the answer
       was the second one (the host's blit-bridge framebuffer, not the window). Logged on CHANGE
       rather than per frame: it is a handful of lines per session, including one on every
       resolution change, which is exactly the transition worth seeing. */
    int logged_out_w = -1;
    int logged_out_h = -1;
    int logged_src_w = -1;
    int logged_src_h = -1;
    SDL_Rect logged_dst{-1, -1, -1, -1};
};

SdlRenderer* Self(armsx_renderer_t* base) {
    return static_cast<SdlRenderer*>(base->impl);
}

void RefreshRendererInfo(SdlRenderer* self) {
    SDL_RendererInfo info{};
    if (self->renderer && SDL_GetRendererInfo(self->renderer, &info) == 0) {
        SDL_strlcpy(self->driver_name, info.name ? info.name : "(unknown)", sizeof(self->driver_name));
        self->accelerated = (info.flags & SDL_RENDERER_ACCELERATED) != 0;
    } else {
        SDL_strlcpy(self->driver_name, "(unknown)", sizeof(self->driver_name));
        self->accelerated = false;
    }
}

void DestroyTexture(SdlRenderer* self) {
    if (self->texture) {
        SDL_DestroyTexture(self->texture);
        self->texture = nullptr;
    }
    self->texture_width = 0;
    self->texture_height = 0;
    self->texture_format = SDL_PIXELFORMAT_UNKNOWN;
    self->scale_mode = -1;
}

/* "Software" for the plain path (which is what the Android host's blit bridge adopts), and
   the SDL driver name when SDL really is on a GPU — the two are visibly different things and
   the OSD should not call both "SDL". */
void PublishActiveName(SdlRenderer* self) {
    if (!self->accelerated) {
        armsx_render_set_active_name("Software");
        return;
    }
    char name[96];
    SDL_snprintf(name, sizeof(name), "SDL accelerated (%s)", self->driver_name);
    armsx_render_set_active_name(name);
}

const char* OpDriverName(armsx_renderer_t* base) {
    return Self(base)->driver_name;
}

bool OpIsAccelerated(armsx_renderer_t* base) {
    return Self(base)->accelerated;
}

SDL_Renderer* OpSdlHandle(armsx_renderer_t* base) {
    return Self(base)->renderer;
}

void OpResize(armsx_renderer_t* base, int width, int height) {
    /* SDL_Renderer tracks the window/target size itself; nothing to do. */
    (void)base;
    (void)width;
    (void)height;
}

void OpOutputSize(armsx_renderer_t* base, int* width, int* height) {
    SdlRenderer* self = Self(base);
    int w = 0;
    int h = 0;
    if (self->renderer) {
        SDL_GetRendererOutputSize(self->renderer, &w, &h);
    }
    if (width) {
        *width = w;
    }
    if (height) {
        *height = h;
    }
}

void OpSetVsync(armsx_renderer_t* base, bool enabled) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SdlRenderer* self = Self(base);
    if (self->renderer && self->owns_renderer) {
        SDL_RenderSetVSync(self->renderer, enabled ? 1 : 0);
    }
#else
    (void)base;
    (void)enabled;
#endif
}

bool OpUploadFrame(armsx_renderer_t* base,
                   const void* pixels,
                   int width,
                   int height,
                   int pitch,
                   Uint32 sdl_format,
                   int dirty_first_row,
                   int dirty_last_row) {
    SdlRenderer* self = Self(base);
    if (!self->renderer || !pixels || width <= 0 || height <= 0 || pitch <= 0) {
        return false;
    }

    if (width != self->texture_width || height != self->texture_height ||
        sdl_format != self->texture_format || !self->texture) {
        DestroyTexture(self);
        self->texture = SDL_CreateTexture(self->renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                                          width, height);
        if (!self->texture) {
            armsx_render_log("renderer", "SDL_CreateTexture failed (%dx%d %s): %s", width, height,
                             SDL_GetPixelFormatName(sdl_format), SDL_GetError());
            return false;
        }
        self->texture_width = width;
        self->texture_height = height;
        self->texture_format = sdl_format;
        /* A fresh texture has undefined contents; force a full upload this frame. */
        dirty_first_row = 0;
        dirty_last_row = height - 1;
    }

    if (dirty_last_row < dirty_first_row) {
        return true;
    }

    if (dirty_first_row < 0) {
        dirty_first_row = 0;
    }
    if (dirty_last_row > height - 1) {
        dirty_last_row = height - 1;
    }

    SDL_Rect dirty{0, dirty_first_row, width, dirty_last_row - dirty_first_row + 1};
    const auto* source = static_cast<const Uint8*>(pixels);
    SDL_UpdateTexture(self->texture, &dirty,
                      source + (static_cast<size_t>(dirty_first_row) * static_cast<size_t>(pitch)),
                      pitch);
    return true;
}

void ApplyScaleMode(SdlRenderer* self, bool linear) {
    const int wanted = linear ? 1 : 0;
    if (!self->texture || self->scale_mode == wanted) {
        return;
    }
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(self->texture, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
    self->scale_mode = wanted;
}

void OpPresent(armsx_renderer_t* base, const armsx_render_frame_params_t* params) {
    SdlRenderer* self = Self(base);
    if (!self->renderer || !self->texture) {
        return;
    }

    ApplyScaleMode(self, params && params->linear_filter);

    int out_w = 0;
    int out_h = 0;
    SDL_GetRendererOutputSize(self->renderer, &out_w, &out_h);

    SDL_Rect dst{0, 0, 0, 0};
    armsx_render_compute_dst(out_w, out_h, self->texture_width, self->texture_height, params, &dst);

    if (out_w != self->logged_out_w || out_h != self->logged_out_h ||
        self->texture_width != self->logged_src_w || self->texture_height != self->logged_src_h ||
        dst.x != self->logged_dst.x || dst.y != self->logged_dst.y || dst.w != self->logged_dst.w ||
        dst.h != self->logged_dst.h) {
        self->logged_out_w = out_w;
        self->logged_out_h = out_h;
        self->logged_src_w = self->texture_width;
        self->logged_src_h = self->texture_height;
        self->logged_dst = dst;
        /* `out` is the RENDERER's output size, which on Android is the host blit bridge's
           offscreen framebuffer — NOT the window. A dst that fills `out` is correct here even
           when the window is bigger; what has to hold is that the bridge's buffer geometry
           matches `out` (android_jni.cpp PresentToSurface). */
        armsx_render_log("renderer",
                         "SDL present out=%dx%d dst=%d,%d %dx%d src=%dx%d stretch=%s aspect=%.4f",
                         out_w, out_h, dst.x, dst.y, dst.w, dst.h, self->texture_width,
                         self->texture_height, (params && params->stretch) ? "true" : "false",
                         params ? params->aspect : 0.0f);
    }

    /* [video] overscan_crop — a source sub-rectangle, which is exactly SDL_RenderCopy's
       srcrect. Clamped against the live texture size: the display resolution changes
       mid-game and a crop computed for the previous mode must never sample outside. */
    SDL_Rect src{0, 0, 0, 0};
    const SDL_Rect* src_ptr = nullptr;

    if (params && params->crop_w > 0 && params->crop_h > 0 && self->texture_width > 0 &&
        self->texture_height > 0) {
        src.x = params->crop_x < 0 ? 0 : params->crop_x;
        src.y = params->crop_y < 0 ? 0 : params->crop_y;
        if (src.x > self->texture_width - 1) src.x = self->texture_width - 1;
        if (src.y > self->texture_height - 1) src.y = self->texture_height - 1;
        src.w = params->crop_w > (self->texture_width - src.x) ? (self->texture_width - src.x)
                                                               : params->crop_w;
        src.h = params->crop_h > (self->texture_height - src.y) ? (self->texture_height - src.y)
                                                                : params->crop_h;
        src_ptr = &src;
    }

    SDL_SetRenderDrawColor(self->renderer, 0, 0, 0, 255);
    SDL_RenderClear(self->renderer);

    /* [video] display_rotation. SDL_RenderCopyEx maps src onto dst and THEN spins the result
       about dst's centre, so dst must be handed over UNROTATED (width and height swapped
       back) and centred where the rotated rect sits — otherwise a 4:3 image is squeezed into
       the 3:4 box armsx_render_compute_dst() produced and comes out distorted. */
    const int rotation = params ? (params->rotation & 3) : 0;

    if (rotation == 0) {
        SDL_RenderCopy(self->renderer, self->texture, src_ptr, &dst);
    } else {
        SDL_Rect spun = dst;

        if (rotation & 1) {
            const int cx = dst.x + dst.w / 2;
            const int cy = dst.y + dst.h / 2;
            spun.w = dst.h;
            spun.h = dst.w;
            spun.x = cx - spun.w / 2;
            spun.y = cy - spun.h / 2;
        }

        SDL_RenderCopyEx(self->renderer, self->texture, src_ptr, &spun,
                         90.0 * (double)rotation, nullptr, SDL_FLIP_NONE);
    }

    SDL_RenderPresent(self->renderer);
}

void OpPresentBlank(armsx_renderer_t* base) {
    SdlRenderer* self = Self(base);
    if (!self->renderer) {
        return;
    }
    SDL_SetRenderDrawColor(self->renderer, 0, 0, 0, 255);
    SDL_RenderClear(self->renderer);
    SDL_RenderPresent(self->renderer);
}

void OpShutdown(armsx_renderer_t* base) {
    SdlRenderer* self = Self(base);
    armsx_render_set_active_name("");
    DestroyTexture(self);
    if (self->owns_renderer && self->renderer) {
        SDL_DestroyRenderer(self->renderer);
    }
    self->renderer = nullptr;
    delete self;
}

const armsx_render_ops_t kSdlOps = {
    OpDriverName,
    OpIsAccelerated,
    OpSdlHandle,
    OpResize,
    OpOutputSize,
    OpSetVsync,
    OpUploadFrame,
    OpPresent,
    OpPresentBlank,
    OpShutdown,
};

Uint32 RendererFlags(bool accelerated, bool vsync) {
    Uint32 flags = accelerated ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE;
#if !defined(__EMSCRIPTEN__)
    if (vsync) {
        flags |= SDL_RENDERER_PRESENTVSYNC;
    }
#endif
    return flags;
}

} // namespace

extern "C" {

armsx_renderer_t* armsx_render_create_sdl(SDL_Window* window, const armsx_render_config_t* config) {
    if (!window) {
        return nullptr;
    }

    const bool want_accelerated = config && config->backend == ARMSX_RENDER_BACKEND_SDL_ACCELERATED;
    const bool vsync = config && config->vsync;

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, RendererFlags(want_accelerated, vsync));
    armsx_render_backend_t effective =
        want_accelerated ? ARMSX_RENDER_BACKEND_SDL_ACCELERATED : ARMSX_RENDER_BACKEND_SDL_SOFTWARE;

    if (!renderer && want_accelerated) {
        armsx_render_log("renderer",
                         "SDL accelerated renderer unavailable error=%s; retrying software renderer.",
                         SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, RendererFlags(false, vsync));
        effective = ARMSX_RENDER_BACKEND_SDL_SOFTWARE;
    }

    if (!renderer) {
        armsx_render_log("renderer", "Renderer initialization failed requested_vsync=%s error=%s",
                         vsync ? "true" : "false", SDL_GetError());
        return nullptr;
    }

    auto* self = new SdlRenderer();
    self->base.ops = &kSdlOps;
    self->base.impl = self;
    self->base.backend = effective;
    self->renderer = renderer;
    self->owns_renderer = true;
    RefreshRendererInfo(self);

    armsx_render_log("renderer", "SDL backend ready driver=%s accelerated=%s vsync=%s",
                     self->driver_name, self->accelerated ? "true" : "false",
                     vsync ? "true" : "false");
    PublishActiveName(self);
    return &self->base;
}

armsx_renderer_t* armsx_render_adopt_sdl(SDL_Renderer* renderer) {
    if (!renderer) {
        return nullptr;
    }

    auto* self = new SdlRenderer();
    self->base.ops = &kSdlOps;
    self->base.impl = self;
    self->renderer = renderer;
    self->owns_renderer = false;
    RefreshRendererInfo(self);
    self->base.backend = self->accelerated ? ARMSX_RENDER_BACKEND_SDL_ACCELERATED
                                           : ARMSX_RENDER_BACKEND_SDL_SOFTWARE;

    armsx_render_log("renderer", "Adopted external SDL renderer driver=%s accelerated=%s",
                     self->driver_name, self->accelerated ? "true" : "false");
    PublishActiveName(self);
    return &self->base;
}

} // extern "C"
