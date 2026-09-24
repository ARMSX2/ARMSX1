#define main parity_suite_main
#include "gpu_renderer_parity.c"
#undef main
int main(void) {
    psx_gpu_t* gpu = make_gpu();
    if (!gpu) return 2;
    memset(gpu->vram, 0, PSX_GPU_VRAM_SIZE);
    /* Captured licensing quad: 256x16 pixels, UV 0..255 / 68..83.
       Use an atlas with a distinct palette colour for each source row. */
    for (int i=1;i<16;i++) gpu->vram[480*1024+i]=(uint16_t)i;
    for (int y=0;y<256;y++) for(int x=640;x<704;x++) {
        unsigned index=y%15+1;
        gpu->vram[y*1024+x]=(uint16_t)(index*0x1111);
    }
    poly_data_t p={0};
    p.attrib=PA_TEXTURED|PA_RAW|PA_QUAD; p.texp=10; p.clut=480<<6;
    p.v[0]=(vertex_t){.x=118,.y=354,.tx=0,.ty=68,.c=0x808080};
    p.v[1]=(vertex_t){.x=374,.y=354,.tx=255,.ty=68,.c=0x808080};
    p.v[2]=(vertex_t){.x=118,.y=370,.tx=0,.ty=83,.c=0x808080};
    p.v[3]=(vertex_t){.x=374,.y=370,.tx=255,.ty=83,.c=0x808080};
    emit_poly(gpu, &p);
    int bad=0;
    for(int y=0;y<16;y++) {
        /* Independently computed 12-bit fixed-point half-texel rounding. */
        int row=((68<<12)+2048+((15<<12)/16)*y)>>12;
        unsigned expected=row%15+1;
        for(int x=1;x<255;x++) if(gpu->vram[(354+y)*1024+118+x]!=expected) bad++;
    }
    printf("BIOS atlas mismatched pixels: %d\n",bad);
    psx_gpu_destroy(gpu);
    return bad?1:0;
}

