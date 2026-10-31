#define PSXE_DIAG_STDIO_DISABLE 1
#define SDL_MAIN_HANDLED
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/gpu.h"
#include "../psx/dev/gpu_backend.h"
#include "../frontend/gpu_hw_rt.h"

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

static int bridge_adopted;
bool armsx_renderer_adopt_gl_texture(armsx_renderer_t* r,unsigned int t,int w,int h,uint32_t f) { return false; }
armsx_render_backend_t armsx_renderer_backend(const armsx_renderer_t* r) { return ARMSX_RENDER_BACKEND_VULKAN; }
bool armsx_renderer_prepare_hardware_buffer(armsx_renderer_t* r,void* b,int w,int h) { return true; }
bool armsx_renderer_import_hardware_buffer_fence(armsx_renderer_t* r,int fd) { return false; }
bool armsx_renderer_adopt_hardware_buffer(armsx_renderer_t* r,void* b,int w,int h) { bridge_adopted++; return true; }
int main(void) {
    setbuf(stdout, NULL);
    unsetenv("ARMSX_GL_MASK_BIT");
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
        gpu->disp_y1 = 0;
        gpu->disp_y2 = 240;
        if (scale > 1)
            psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT);
        psx_gpu_backend_t* backend = armsx_hw_gl_create(gpu, scale);
        if (!backend) return 12;
        hw_gl_t* g = gl_self(backend);
        if (scale > 1) {
            printf("GLES accurate upscale scale=%d guard=%d shadow=%d fetch=%d\n",
                   scale, g->mask_guard, g->shadow, g->mask_mode);
            failures += !g->mask_guard || !g->shadow || g->mask_mode || g->gpu_own;
        }
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
            int scan_w = 0, scan_h = 0;
            if (!gl_scanout_resolve(g, 0, 0, &scan_w, &scan_h) ||
                !gl_scanout_read(g, scan_w * scale, scan_h * scale)) return 16;
            const uint16_t* scan = (const uint16_t*)g->readback;
            const int scan_ok = (scan[(100 * scale) * scan_w * scale + 100 * scale] & 0x7fff) == 31;
            printf("GLES scanout scale=%d depth=%d correct=%d packed_read=%d\n",
                   scale, depth, scan_ok, g->read_packed);
            failures += !scan_ok;
            gl_release(g);
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

    /* Exercise real GP0 dispatch, not just hooks: bit 15 must survive GPU
       rendering, native readback and the transition to the scaled CPU target. */
    for (int kind = 0; kind < 4; kind++) {
        psx_gpu_t* gpu = psx_gpu_create();
        psx_gpu_init(gpu, NULL);
        gpu->draw_x1 = gpu->draw_y1 = 0;
        gpu->draw_x2 = 1023; gpu->draw_y2 = 511;
        psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT);
        memset(gpu->vram, 0, 1024 * 512 * sizeof(uint16_t));
        psx_gpu_t* reference = psx_gpu_create();
        psx_gpu_init(reference, NULL);
        memset(reference->vram, 0, 1024 * 512 * sizeof(uint16_t));
        reference->draw_x1 = reference->draw_y1 = 0;
        reference->draw_x2 = 1023; reference->draw_y2 = 511;
        psx_gpu_set_accuracy_flags(reference, PSX_GPU_ACCURACY_MASK_BIT);
#define GP0(word) do { psx_gpu_write32(gpu, 0, word); psx_gpu_write32(reference, 0, word); } while (0)
        psx_gpu_backend_t* backend = armsx_hw_gl_create(gpu, 3);
        if (!backend) return 14;
        psx_gpu_set_backend(gpu, backend);
        hw_gl_t* g = gl_self(backend);
        GP0(0xe6000001); /* Force bit 15, no check. */
        GP0(0x600000f8);
        GP0(0x00100010);
        GP0(0x00040010);
        int good = !g->failed && gpu->vram[16 * 1024 + 16] == 0x801f;
        /* A masked native read needs no GPU readback and retains bit 15. */
        GP0(0xc0000000);
        GP0(0x00100010);
        GP0(0x00010002);
        good &= psx_gpu_read32(gpu, 0) == 0x801f801f;
        good &= psx_gpu_read32(reference, 0) == 0x801f801f;
        if (kind == 3) {
            /* A different automatic downgrade must not seed maskless GPU alpha
               over accurate native VRAM either. */
            gl_downgrade(g, "test readback downgrade");
            good &= gpu->vram[16 * 1024 + 16] == 0x801f;
        }
        GP0(0xe6000002); /* Check existing mask. */
        GP0(kind == 0 ? 0x60f80000 :
                                   kind == 1 ? 0x20f80000 : 0x40f80000);
        GP0(0x00100010);
        if (kind == 0) GP0(0x00040010);
        else {
            GP0(0x00100020);
            if (kind == 1) GP0(0x00140010);
        }
        good &= g->failed && memcmp(gpu->vram, reference->vram,
                                   1024 * 512 * sizeof(uint16_t)) == 0;
        const uint16_t expected = reference->vram[16 * 1024 + 16];
        psx_gpu_set_backend(gpu, NULL);
        armsx_hw_gl_destroy(backend);
        backend = armsx_hw_rt_create(gpu, 3);
        if (!backend) return 15;
        psx_gpu_set_backend(gpu, backend);
        uint32_t stride = 0;
        const uint16_t* frame = backend->display_buffer(backend, 0, 0, &stride);
        good &= frame && frame[48 * (stride / 2) + 48] == expected;
        GP0(0xe6000000);
        GP0(0x60f80000);
        GP0(0x00100010);
        GP0(0x00040010);
        frame = backend->display_buffer(backend, 0, 0, &stride);
        good &= gpu->vram[16 * 1024 + 16] == 0x7c00 &&
                frame && frame[48 * (stride / 2) + 48] == 0x7c00;
        printf("GLES mask accuracy handoff kind=%d passed=%d\n", kind, good);
        failures += !good;
        psx_gpu_set_backend(gpu, NULL);
        armsx_hw_rt_destroy(backend);
        psx_gpu_destroy(gpu);
        psx_gpu_destroy(reference);
#undef GP0
    }

    /* Exercise the production shared shader and resource lifecycle, not a
       separately rewritten approximation. Vulkan import is tested by the
       cross-API benchmark; this check isolates resolve colour/order and gates. */
    psx_gpu_t* shared_gpu = psx_gpu_create();
    psx_gpu_init(shared_gpu, NULL);
    shared_gpu->display_mode = 1;
    shared_gpu->disp_y1 = 16; shared_gpu->disp_y2 = 255;
    shared_gpu->gpustat &= ~0x800000;
    psx_gpu_backend_t* shared_be = armsx_hw_gl_create(shared_gpu, 3);
    if (!shared_be) return 30;
    hw_gl_t* shared = gl_self(shared_be);
    shared->egl_library = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    shared->egl_display = display;
    shared->owns_context = 1;
    armsx_gpu_profile_note_vk(0x5143, 8, "Adreno (TM) 740", "Qualcomm", "test");
    const int width = gl_display_width(shared_gpu)*3;
    const int height = gl_display_height(shared_gpu)*3;
    uint8_t* input = malloc((size_t)width*height*4);
    uint8_t* output = malloc((size_t)width*height*4);
    for (int p=0;p<width*height;p++) {
        unsigned v=(unsigned)p&32767;
        input[p*4+0]=(v&31)*8;
        input[p*4+1]=((v>>5)&31)*8;
        input[p*4+2]=((v>>10)&31)*8;
        input[p*4+3]=255;
    }
    shared->gl.BindTexture(GL_TEXTURE_2D,shared->rt_tex);
    shared->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,input);
    size_t mismatch=0;
    for (int cycle=0;cycle<3;cycle++) {
        int good=gl_present_shared(shared,(armsx_renderer_t*)1);
        if (!good) { printf("shared production adoption failed cycle=%d\n",cycle); failures++; break; }
        shared->gl.BindFramebuffer(GL_FRAMEBUFFER,shared->shared_fbo);
        shared->gl.ReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,output);
        for (int p=0;p<width*height;p++) {
            unsigned v=(unsigned)p&32767,r=v&31,g=(v>>5)&31,b=(v>>10)&31;
            mismatch += output[p*4] != ((r<<3)|(r>>2));
            mismatch += output[p*4+1] != ((g<<3)|(g>>2));
            mismatch += output[p*4+2] != ((b<<3)|(b>>2));
            mismatch += output[p*4+3] != 255;
        }
        gl_release(shared);
        if(cycle==1)gl_shared_destroy(shared);
    }
    printf("production shared resolve checked_pixels=%d mismatched_bytes=%zu adopted=%d\n",width*height*3,mismatch,bridge_adopted);
    failures += mismatch != 0 || bridge_adopted != 3;
    shared->scale=1; failures += gl_present_shared(shared,(armsx_renderer_t*)1)!=0;
    shared->scale=2; failures += gl_present_shared(shared,(armsx_renderer_t*)1)!=0;
    shared->scale=3; shared_gpu->gpustat|=0x800000; failures += gl_present_shared(shared,(armsx_renderer_t*)1)!=0;
    shared_gpu->gpustat&=~0x800000; shared_gpu->display_mode|=0x10;failures+=gl_present_shared(shared,(armsx_renderer_t*)1)!=0;
    shared_gpu->display_mode&=~0x10; armsx_gpu_profile_note_vk(0x13b5,9,"Mali-G57","ARM","test");
    failures+=gl_present_shared(shared,(armsx_renderer_t*)1)!=0;
    printf("shared exclusions 1x/2x/blank/24bit/Mali checked\n");
    shared->owns_context=0;
    void* shared_lib=shared->egl_library; shared->egl_library=NULL;
    armsx_hw_gl_destroy(shared_be); dlclose(shared_lib);
    psx_gpu_destroy(shared_gpu); free(input); free(output);
    psx_gpu_t* classic = psx_gpu_create();
    psx_gpu_init(classic, NULL);
    psx_gpu_set_accuracy_flags(classic, PSX_GPU_ACCURACY_MASK_BIT);
    psx_gpu_backend_t* classic_backend = armsx_hw_gl_create(classic, 1);
    printf("GLES accurate classic selection unchanged=%d\n", !classic_backend);
    failures += classic_backend != NULL;
    armsx_hw_gl_destroy(classic_backend);
    psx_gpu_destroy(classic);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    printf("GLES rendering failures=%d\n", failures);
    return failures ? 1 : 0;
}
