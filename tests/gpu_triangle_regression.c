/* Run against the before/after core libraries. Full VRAM signatures must match;
   includes framebuffer feedback, masks, both windings and clipped triangles. */
#include "../psx/dev/gpu.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

static int64_t reference_edge(vertex_t a, vertex_t b, int x, int y) {
    return (int64_t)(b.x - a.x) * (y - a.y) - (int64_t)(b.y - a.y) * (x - a.x);
}

static int reference_inside(vertex_t a, vertex_t b, int x, int y) {
    const int64_t value = reference_edge(a, b, x, y);
    return value > 0 || (value == 0 && !(b.y > a.y || (b.y == a.y && b.x < a.x)));
}

static void check_coverage(void) {
    psx_gpu_t* gpu = psx_gpu_create();
    uint16_t* expected = malloc(PSX_GPU_VRAM_SIZE);
    assert(gpu && expected);
    psx_gpu_init(gpu, NULL);
    unsigned state = 0x817fdeu;
    for (int n = 0; n < 128; ++n) {
        poly_data_t p = {0};
        for (int v = 0; v < 3; ++v) {
            state = state * 1664525u + 1013904223u;
            p.v[v].x = (int)(state % 700) - 180;
            state = state * 1664525u + 1013904223u;
            p.v[v].y = (int)(state % 500) - 100;
            p.v[v].c = 0x08f8a0;
        }
        if (n % 16 == 0) p.v[2] = p.v[1];
        if (n % 16 == 1) p.v[1].y = p.v[0].y;
        if (n % 16 == 2) p.v[1].x = p.v[0].x;
        if (n % 16 == 3) { p.v[1].x = p.v[0].x + 1; p.v[2].x = p.v[0].x + 2; }
        if (n % 16 == 4) p.v[2].x = p.v[0].x + 1024;
        gpu->off_x = n % 23 - 11;
        gpu->off_y = n % 17 - 8;
        gpu->draw_x1 = n % 19;
        gpu->draw_y1 = n % 13;
        gpu->draw_x2 = 300 - n % 7;
        gpu->draw_y2 = 260 - n % 11;
        gpu->accuracy_flags = 15;
        gpu->gpustat = ((n & 1) ? 0x1000 : 0) | ((n & 2) ? 0x800 : 0);
        for (size_t i = 0; i < PSX_GPU_VRAM_SIZE / 2; ++i)
            expected[i] = gpu->vram[i] = (i & 1) ? 0x8421 : 0x0421;
        vertex_t a = p.v[0], b = p.v[1], c = p.v[2];
        if (reference_edge(a, b, c.x, c.y) < 0) { vertex_t swap = b; b = c; c = swap; }
        a.x += gpu->off_x; a.y += gpu->off_y;
        b.x += gpu->off_x; b.y += gpu->off_y;
        c.x += gpu->off_x; c.y += gpu->off_y;
        int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y;
        const vertex_t vertices[] = {b, c};
        for (int v = 0; v < 2; ++v) {
            if (vertices[v].x < minx) minx = vertices[v].x;
            if (vertices[v].x > maxx) maxx = vertices[v].x;
            if (vertices[v].y < miny) miny = vertices[v].y;
            if (vertices[v].y > maxy) maxy = vertices[v].y;
        }
        if (maxx - minx <= 1023 && maxy - miny <= 511 && reference_edge(a, b, c.x, c.y)) {
            for (int y = 0; y < 512; ++y) {
                for (int x = 0; x < 1024; ++x) {
                    const size_t index = (size_t)y * 1024 + x;
                    if (x < (int)gpu->draw_x1 || x > (int)gpu->draw_x2 ||
                        y < (int)gpu->draw_y1 || y > (int)gpu->draw_y2 ||
                        x < minx || x >= maxx || y < miny || y >= maxy ||
                        ((n & 1) && (expected[index] & 0x8000))) continue;
                    if (reference_inside(a, b, x, y) && reference_inside(b, c, x, y) &&
                        reference_inside(c, a, x, y))
                        expected[index] = 0x07f4 | ((n & 2) ? 0x8000 : 0);
                }
            }
        }
        gpu_render_triangle(gpu, p.v[0], p.v[1], p.v[2], p, 0);
        assert(memcmp(gpu->vram, expected, PSX_GPU_VRAM_SIZE) == 0);
    }
    free(expected);
    psx_gpu_destroy(gpu);
    puts("128 brute-force triangle coverage/mask cases passed");
}

static unsigned seed = 0x37ab4129;
static unsigned random_word(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
int main(void) {
    check_coverage();
    /* Recorded from the pre-optimisation strict-FP software renderer. */
    static const uint64_t expected[] = {
        0xd89eeba45dcbf2eeull, 0x2f6f56e008fcfa69ull,
        0x4bc2c90f3b4534ebull, 0xce2926c72a6732bbull,
        0x5839550b8118301full, 0x4ceb82a96985e5e1ull,
        0x78b0e892dff7b6bcull, 0x21617e5b15915b4aull
    };
    psx_gpu_t* g = psx_gpu_create(); assert(g);
    psx_gpu_init(g, NULL);
    uint64_t digest = 14695981039346656037ull;
    double render_ms = 0;
    for (int i=0; i<512; ++i) {
        for (unsigned j=0; j<PSX_GPU_VRAM_SIZE/2; ++j)
            g->vram[j] = (uint16_t)random_word();
        g->accuracy_flags = (i & 1) ? 15 : 0;
        g->gpustat = ((i >> 1) & 3) << 5 | ((i >> 3) & 7) << 9;
        g->off_x = i % 33 - 16; g->off_y = i % 17 - 8;
        g->draw_x1 = i % 23; g->draw_y1 = i % 19;
        g->draw_x2 = 511 - i % 29; g->draw_y2 = 383 - i % 31;
        g->texw_mx = (i & 32) ? 0x38 : 0;
        g->texw_my = (i & 64) ? 0x18 : 0;
        g->texw_ox = 24; g->texw_oy = 8;
        psx_gpu_set_texture_filter(g, (i & 128) ? 1 : 0);
        poly_data_t p = {0};
        p.attrib = ((i & 1) ? PA_TEXTURED : 0) |
            ((i & 2) ? PA_SHADED : 0) | ((i & 4) ? PA_TRANSP : 0) |
            ((i & 8) ? PA_RAW : 0);
        p.texp = (i%8) | ((i%3)<<7) | (((i>>4)&3)<<5);
        p.clut = (500<<6) | 16;
        for (int v=0;v<3;v++) {
            p.v[v].x = (int)(random_word()%700)-100;
            p.v[v].y = (int)(random_word()%500)-70;
            p.v[v].tx = random_word()&255; p.v[v].ty = random_word()&255;
            p.v[v].c = random_word()&0xffffff;
        }
        /* Include degenerate/shared-edge and thin-triangle cases explicitly. */
        if (i%16==0) p.v[2]=p.v[1];
        if (i%16==1) p.v[1].y=p.v[0].y;
        if (i%16==2) p.v[1].x=p.v[0].x;
        if (i%16==3) { p.v[1].x=p.v[0].x+1; p.v[2].x=p.v[0].x+2; }
        clock_t start=clock();
        for (int repeat=0;repeat<8;repeat++)
            gpu_render_triangle(g,p.v[0],p.v[1],p.v[2],p,i&1);
        render_ms+=1000.0*(clock()-start)/CLOCKS_PER_SEC;
        const unsigned char* bytes=(const unsigned char*)g->vram;
        for (unsigned j=0;j<PSX_GPU_VRAM_SIZE;j++)
            digest=(digest^bytes[j])*1099511628211ull;
        if (i%64==63) {
            printf("triangles=%d vram=%016llx\n",i+1,(unsigned long long)digest);
            assert(digest==expected[i/64]);
        }
    }
    printf("4096 triangle draws: %.3f ms\n",render_ms);
    psx_gpu_destroy(g);
    return 0;
}
