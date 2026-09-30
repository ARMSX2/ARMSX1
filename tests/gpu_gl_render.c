#define PSXE_DIAG_STDIO_DISABLE 1
#define SDL_MAIN_HANDLED
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/gpu.h"
#include "../psx/dev/gpu_backend.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

void psx_ic_irq(psx_ic_t* ic, int id) {
    (void)ic;
    (void)id;
}

#include "../frontend/gpu_hw_gl.c"
void psxe_diag_logf(const char* tag,const char* fmt,...) { va_list a; va_start(a,fmt); vprintf(fmt,a); puts(""); va_end(a); }
const char* psxe_diag_log_path(void) { return NULL; }
int armsx_render_active_name(char* b,int n) { snprintf(b,n,"OpenGL ES (system)");return 1; }
/* Run on Android with a real GLES driver. Include the backend above so these checks
   render directly, without the software shadow hiding missing primitives. */
int main(void) {
    setbuf(stdout, NULL);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, count;
    EGLConfig config;
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint surface_attrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    if (!eglInitialize(display, &major, &minor) ||
        !eglChooseConfig(display, attrs, &config, 1, &count) || count != 1)
        return 10;
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    if (!eglMakeCurrent(display, surface, surface, context)) return 11;

    int failures = 0;
    for (int scale = 1; scale <= 3; scale++) {
        psx_gpu_t* gpu = psx_gpu_create();
        psx_gpu_init(gpu, NULL);
        gpu->draw_x1 = gpu->draw_y1 = 0;
        gpu->draw_x2 = 1023;
        gpu->draw_y2 = 511;
        psx_gpu_backend_t* backend = armsx_hw_gl_create(gpu, scale);
        if (!backend) return 12;
        hw_gl_t* g = gl_self(backend);
        uint16_t* pixels = calloc(1024 * 512, sizeof(uint16_t));
        if (!pixels) return 13;

        for (int depth = 0; depth <= 2; depth++) {
            gpu->texp_x = 0;
            gpu->texp_y = 256;
            gpu->texp_d = depth;
            for (int y = 256; y < 512; y++)
                for (int x = 0; x < 256; x++)
                    gpu->vram[y * 1024 + x] = depth == 0 ? 0x1111 : depth == 1 ? 0x0101 : 31;
            gpu->vram[240 * 1024 + 1] = 31;
            gl_mark_dirty(g, 0, 240, 256, 272);
            rect_data_t sprite = {0};
            sprite.attrib = RA_TEXTURED | RA_RAW;
            sprite.width = 100;
            sprite.height = 30;
            sprite.v0.x = sprite.v0.y = 100;
            sprite.v0.c = 0x808080;
            sprite.clut = 240 << 6;
            gl_draw_rect(backend, gpu, &sprite);
            gl_flush(g);
            gl_readback_rect(g, 100, 100, 100, 30, pixels, 1024);
            int bad = 0;
            for (int y = 100; y < 130; y++)
                for (int x = 100; x < 200; x++)
                    bad += (pixels[y * 1024 + x] & 0x7fff) != 31;
            printf("GLES sprite scale=%d depth=%d missing=%d/3000\n", scale, depth, bad);
            failures += bad != 0;
        }

        poly_data_t triangle = {0};
        triangle.attrib = PA_TEXTURED | PA_RAW;
        triangle.texp = 0x110;
        triangle.v[0].x = 300; triangle.v[0].y = 100;
        triangle.v[1].x = 400; triangle.v[1].y = 100;
        triangle.v[2].x = 300; triangle.v[2].y = 200;
        triangle.v[1].tx = 99; triangle.v[2].ty = 99;
        gl_draw_poly(backend, gpu, &triangle);
        gl_readback_rect(g, 310, 110, 20, 20, pixels, 1024);
        int bad = 0;
        for (int y = 110; y < 130; y++)
            for (int x = 310; x < 330; x++)
                bad += (pixels[y * 1024 + x] & 0x7fff) != 31;
        printf("GLES textured triangle scale=%d missing=%d/400\n", scale, bad);
        failures += bad != 0;

        /* A smooth gradient must vary INSIDE a native pixel at 3x. A full-frame
           native-image copy would pass the visibility checks but fail this one. */
        triangle.attrib = PA_SHADED;
        triangle.v[0].x = 10; triangle.v[0].y = 10; triangle.v[0].c = 0;
        triangle.v[1].x = 22; triangle.v[1].y = 10; triangle.v[1].c = 255;
        triangle.v[2].x = 10; triangle.v[2].y = 22; triangle.v[2].c = 255 << 8;
        gl_draw_poly(backend, gpu, &triangle);
        gl_flush(g);
        gl_bind_rt(g);
        uint8_t block[3 * 3 * 4] = {0};
        g->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        g->gl.ReadPixels(13 * scale, 13 * scale, scale, scale, GL_RGBA, GL_UNSIGNED_BYTE, block);
        int varied = 0;
        for (int i = 1; i < scale * scale; i++)
            varied |= memcmp(block, block + i * 4, 3) != 0;
        if (scale == 3) {
            printf("GLES 3x subpixel gradient varied=%d\n", varied);
            failures += !varied;
        }
        free(pixels);
        armsx_hw_gl_destroy(backend);
        psx_gpu_destroy(gpu);
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf("GLES rendering failures=%d\n", failures);
    return failures ? 1 : 0;
}