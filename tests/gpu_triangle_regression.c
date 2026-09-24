/* Run against the before/after core libraries. Full VRAM signatures must match;
   includes framebuffer feedback, masks, both windings and clipped triangles. */
#include "../psx/dev/gpu.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static unsigned seed = 0x37ab4129;
static unsigned random_word(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
int main(void) {
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
