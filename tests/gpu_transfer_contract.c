#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/gpu.h"
#include "../psx/dev/gpu_backend.h"
#include "../frontend/gpu_hw_rt.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level; (void)file; (void)line; (void)format;
}
void psx_ic_irq(psx_ic_t* ic, int id) { (void)ic; (void)id; }
void psx_sw_u8(psx_state_writer_t* w, uint8_t v) { (void)w; (void)v; }
void psx_sw_u16(psx_state_writer_t* w, uint16_t v) { (void)w; (void)v; }
void psx_sw_u32(psx_state_writer_t* w, uint32_t v) { (void)w; (void)v; }
void psx_sw_i32(psx_state_writer_t* w, int32_t v) { (void)w; (void)v; }
void psx_sw_f32(psx_state_writer_t* w, float v) { (void)w; (void)v; }
void psx_sw_u16_array(psx_state_writer_t* w, const uint16_t* v, size_t n) { (void)w; (void)v; (void)n; }
uint8_t psx_sr_u8(psx_state_reader_t* r) { (void)r; return 0; }
uint16_t psx_sr_u16(psx_state_reader_t* r) { (void)r; return 0; }
uint32_t psx_sr_u32(psx_state_reader_t* r) { (void)r; return 0; }
int32_t psx_sr_i32(psx_state_reader_t* r) { (void)r; return 0; }
float psx_sr_f32(psx_state_reader_t* r) { (void)r; return 0; }
void psx_sr_u16_array(psx_state_reader_t* r, uint16_t* v, size_t n) { (void)r; (void)v; (void)n; }

static int failures;
static int assertions;

static void expect(const char* name, unsigned int actual, unsigned int expected) {
    ++assertions;
    if (actual != expected) {
        fprintf(stderr, "GPU_CONTRACT %s: got=%04x expected=%04x\n", name, actual, expected);
        ++failures;
    }
}

static void pixel(psx_gpu_t* gpu, const char* name, unsigned int x, unsigned int y, uint16_t expected) {
    expect(name, gpu->vram[x + y * 1024], expected);
    if (gpu->backend) {
        uint32_t stride;
        const uint16_t* rt = gpu->backend->display_buffer(gpu->backend, 0, 0, &stride);
        const int scale = gpu->backend->resolution_scale(gpu->backend);
        for (int by = 0; by < scale; ++by)
            for (int bx = 0; bx < scale; ++bx)
                expect(name, rt[(y * scale + by) * (stride / 2) + x * scale + bx], expected);
    }
}

static void sync_vram(psx_gpu_t* gpu) {
    if (gpu->backend)
        gpu->backend->upload_vram(gpu->backend, 0, 0, 1024, 512, gpu->vram, 1024);
}

static void gp0(psx_gpu_t* gpu, uint32_t value) { psx_gpu_write32(gpu, 0, value); }
static uint32_t xy(unsigned int x, unsigned int y) { return x | (y << 16); }

static void copy(psx_gpu_t* gpu, unsigned int sx, unsigned int sy,
                 unsigned int dx, unsigned int dy, unsigned int w, unsigned int h) {
    gp0(gpu, 0x80000000);
    gp0(gpu, xy(sx, sy)); gp0(gpu, xy(dx, dy)); gp0(gpu, xy(w, h));
}

static void test_sampler(psx_gpu_t* gpu) {
    gpu->vram[0] = 0x1234;
    gpu->vram[1024] = 0x5678;
    expect("15-bit x wrap", gpu_fetch_texel(gpu, 64, 0, 960, 0, 0, 0, 2), 0x1234);
    gpu->vram[511 * 1024] = 0x4321;
    expect("15-bit last row", gpu_fetch_texel(gpu, 64, 255, 960, 256, 0, 0, 2), 0x4321);
    expect("texture y wrap", gpu_fetch_texel(gpu, 0, 255, 0, 257, 0, 0, 2), 0x1234);
    gpu->vram[960] = 16;
    gpu->vram[400 * 1024] = 0x2abc;
    gpu->vram[401 * 1024] = 0x1357;
    expect("8-bit palette x wrap", gpu_fetch_texel(gpu, 0, 0, 960, 0, 1008, 400, 1), 0x2abc);
    gpu->vram[0] = 0x12;
    gpu->vram[400 * 1024 + 18] = 0x3456;
    expect("8-bit page x wrap", gpu_fetch_texel(gpu, 128, 0, 960, 0, 0, 400, 1), 0x3456);
    gpu->vram[512] = 0x3210;
    gpu->vram[400 * 1024 + 3] = 0x7654;
    expect("4-bit nibble", gpu_fetch_texel(gpu, 3, 0, 512, 0, 0, 400, 0), 0x7654);
}

static void test_upload(psx_gpu_t* gpu) {
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    gpu->vram[100] = 0x8001;
    gpu->vram[101] = 0x0002;
    gpu->vram[102] = 0x4321;
    sync_vram(gpu);
    gp0(gpu, 0xe6000003);
    gp0(gpu, 0xa0000000); gp0(gpu, 100); gp0(gpu, xy(2, 1)); gp0(gpu, 0x001f03e0);
    pixel(gpu, "upload preserves masked destination", 100, 0, 0x8001);
    pixel(gpu, "upload consumes masked source", 101, 0, 0x801f);
    gp0(gpu, 0xe6000000);
    gp0(gpu, 0xa0000000); gp0(gpu, 101); gp0(gpu, xy(1, 1)); gp0(gpu, 0x7c008123);
    pixel(gpu, "upload source mask", 101, 0, 0x8123);
    pixel(gpu, "upload ignores odd padding", 102, 0, 0x4321);
    gp0(gpu, 0xa0000000); gp0(gpu, xy(1023, 511)); gp0(gpu, xy(2, 2));
    gp0(gpu, 0x00020001); gp0(gpu, 0x00040003);
    pixel(gpu, "upload bottom right", 1023, 511, 1);
    pixel(gpu, "upload x wrap", 0, 511, 2);
    pixel(gpu, "upload y wrap", 1023, 0, 3);
    pixel(gpu, "upload xy wrap", 0, 0, 4);
    expect("upload returns to command state", gpu->state, GPU_STATE_RECV_CMD);
}

static void test_copy(psx_gpu_t* gpu) {
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    gp0(gpu, 0xe6000000);
    for (unsigned int x = 0; x < 4; ++x) gpu->vram[100 + x] = (uint16_t)(x + 1);
    sync_vram(gpu);
    copy(gpu, 100, 0, 101, 0, 3, 1);
    pixel(gpu, "overlap right first", 101, 0, 1);
    pixel(gpu, "overlap right middle", 102, 0, 2);
    pixel(gpu, "overlap right last", 103, 0, 3);
    copy(gpu, 101, 0, 100, 0, 3, 1);
    pixel(gpu, "overlap left first", 100, 0, 1);
    pixel(gpu, "overlap left last", 102, 0, 3);
    gpu->vram[200 + 10 * 1024] = 7;
    gpu->vram[200 + 11 * 1024] = 8;
    gpu->vram[200 + 12 * 1024] = 9;
    sync_vram(gpu);
    copy(gpu, 200, 10, 200, 11, 1, 2);
    pixel(gpu, "vertical copy first", 200, 11, 7);
    pixel(gpu, "vertical copy forward feedback", 200, 12, 7);
    gpu->vram[511 * 1024 + 1023] = 11; gpu->vram[511 * 1024] = 12;
    gpu->vram[1023] = 13; gpu->vram[0] = 14;
    sync_vram(gpu);
    copy(gpu, 1023, 511, 300, 200, 2, 2);
    pixel(gpu, "copy wrapped source 0", 300, 200, 11);
    pixel(gpu, "copy wrapped source 1", 301, 200, 12);
    pixel(gpu, "copy wrapped source 2", 300, 201, 13);
    pixel(gpu, "copy wrapped source 3", 301, 201, 14);
    copy(gpu, 300, 200, 1023, 511, 2, 2);
    pixel(gpu, "copy wrapped destination", 0, 0, 14);
    gpu->vram[400] = 0x001f; gpu->vram[401] = 0x83e0;
    gpu->vram[500] = 0x8001; gpu->vram[501] = 0x0002;
    sync_vram(gpu);
    gp0(gpu, 0xe6000003);
    copy(gpu, 400, 0, 500, 0, 2, 1);
    pixel(gpu, "copy mask check", 500, 0, 0x8001);
    pixel(gpu, "copy mask set", 501, 0, 0x83e0);
    gp0(gpu, 0xe6000000);
    copy(gpu, 400 + 1024, 512, 502 + 2048, 1024, 1025, 513);
    pixel(gpu, "copy coordinate and size masks", 502, 0, 0x001f);
    gpu->vram[1023 + 20 * 1024] = 0x31;
    gpu->vram[50 + 511 * 1024] = 0x42;
    sync_vram(gpu);
    copy(gpu, 0, 20, 0, 21, 0, 1);
    pixel(gpu, "copy zero width is 1024", 1023, 21, 0x31);
    copy(gpu, 50, 0, 51, 0, 1, 0);
    pixel(gpu, "copy zero height is 512", 51, 511, 0x42);
}

static void test_dither(psx_gpu_t* gpu) {
    poly_data_t poly = {0};
    poly.texp = 8 | (2 << 7);
    poly.v[0].x = 16; poly.v[0].y = 16;
    poly.v[1].x = 20; poly.v[1].y = 16;
    poly.v[2].x = 16; poly.v[2].y = 20;
    poly.v[0].c = poly.v[1].c = poly.v[2].c = 0x848484;
    for (unsigned int shaded = 0; shaded < 2; ++shaded) {
        memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
        gpu->vram[512] = 0x0421;
        sync_vram(gpu);
        poly.attrib = PA_TEXTURED | (shaded ? PA_SHADED : 0);
        gp0(gpu, 0xe1000200);
        if (gpu->backend) gpu->backend->draw_poly(gpu->backend, gpu, &poly);
        gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 0);
        pixel(gpu, "dither follows texture modulation", 16, 16, 0);
        pixel(gpu, "zero dither preserves level", 17, 16, 0x0421);
    }
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    gpu->vram[512] = 0x7fff;
    sync_vram(gpu);
    poly.v[0].c = poly.v[1].c = poly.v[2].c = 0xffffff;
    if (gpu->backend) gpu->backend->draw_poly(gpu->backend, gpu, &poly);
    gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 0);
    pixel(gpu, "dither before saturation", 16, 16, 0x7fff);
}

typedef struct {
    psx_gpu_backend_t base;
    uint16_t pixels[1024 * 512];
    unsigned int reads;
    unsigned int uploads;
} transfer_backend_t;

static void download(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, uint16_t* dst, uint32_t stride) {
    transfer_backend_t* backend = (transfer_backend_t*)be;
    ++backend->reads;
    expect("readback does not cross x boundary", x + w <= 1024, 1);
    expect("readback does not cross y boundary", y + h <= 512, 1);
    for (uint32_t row = 0; row < h; ++row)
        memcpy(dst + (y + row) * stride + x, backend->pixels + (y + row) * 1024 + x,
               w * sizeof(uint16_t));
}

static void upload(psx_gpu_backend_t* be, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, const uint16_t* src, uint32_t stride) {
    transfer_backend_t* backend = (transfer_backend_t*)be;
    ++backend->uploads;
    for (uint32_t row = 0; row < h; ++row)
        for (uint32_t col = 0; col < w; ++col) {
            const uint32_t px = (x + col) & 1023, py = (y + row) & 511;
            backend->pixels[py * 1024 + px] = src[py * stride + px];
        }
}

static void test_device_owned_transfer(void) {
    psx_gpu_t* gpu = psx_gpu_create();
    transfer_backend_t* backend = calloc(1, sizeof(*backend));
    if (!gpu || !backend) exit(2);
    psx_gpu_init(gpu, NULL);
    psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT);
    backend->base.download_vram = download;
    backend->base.upload_vram = upload;
    psx_gpu_set_backend(gpu, &backend->base);
    backend->pixels[511 * 1024 + 1023] = 0x1234;
    backend->pixels[511 * 1024] = 0x2345;
    backend->pixels[1023] = 0x3456;
    backend->pixels[0] = 0x4567;
    copy(gpu, 1023, 511, 100, 100, 2, 2);
    expect("device owned copy reads each wrapped part", backend->reads, 4);
    expect("device owned copy source preserved", backend->pixels[100 * 1024 + 100], 0x1234);
    expect("device owned wrapped copy source", backend->pixels[101 * 1024 + 101], 0x4567);
    expect("device owned copy uploads result", backend->uploads, 1);
    backend->pixels[200] = 0x8001;
    backend->pixels[201] = 0x0002;
    gp0(gpu, 0xe6000003);
    gp0(gpu, 0xa0000000); gp0(gpu, 200); gp0(gpu, xy(2, 1)); gp0(gpu, 0x001f03e0);
    expect("device owned upload reads destination mask", backend->pixels[200], 0x8001);
    expect("device owned upload consumes skipped texel", backend->pixels[201], 0x801f);
    backend->pixels[300] = 0x001f;
    backend->pixels[301] = 0x03e0;
    backend->pixels[400] = 0x8002;
    backend->pixels[401] = 0x0000;
    copy(gpu, 300, 0, 400, 0, 2, 1);
    expect("device owned copy reads destination mask", backend->pixels[400], 0x8002);
    expect("device owned copy sets mask", backend->pixels[401], 0x83e0);
    psx_gpu_set_backend(gpu, NULL);
    free(backend);
    psx_gpu_destroy(gpu);
}

int main(void) {
    for (int scale = 0; scale <= 2; ++scale) {
        psx_gpu_t* gpu = psx_gpu_create();
        if (!gpu) return 2;
        psx_gpu_init(gpu, NULL);
        gpu->draw_x2 = 1023; gpu->draw_y2 = 511;
        psx_gpu_set_accuracy_flags(gpu, PSX_GPU_ACCURACY_MASK_BIT |
            PSX_GPU_ACCURACY_DITHER_GATE | PSX_GPU_ACCURACY_TEX_MODULATE);
        psx_gpu_backend_t* backend = scale ? armsx_hw_rt_create(gpu, scale) : NULL;
        if (scale && !backend) return 2;
        psx_gpu_set_backend(gpu, backend);
        test_sampler(gpu);
        test_upload(gpu);
        test_copy(gpu);
        test_dither(gpu);
        psx_gpu_set_backend(gpu, NULL);
        armsx_hw_rt_destroy(backend);
        psx_gpu_destroy(gpu);
    }
    test_device_owned_transfer();
    printf("GPU_CONTRACT assertions=%d failures=%d\n", assertions, failures);
    return failures != 0;
}
