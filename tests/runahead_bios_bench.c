#include "../psx/psx.h"
#include "../psx/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static unsigned frames;
static void vblank(psx_gpu_t* gpu) { ++frames; psxe_gpu_vblank_timer_event_cb(gpu); }
static void frame(psx_t* p) {
    unsigned begin = frames, steps = 0;
    while (frames == begin && ++steps < 2000000) psx_update(p);
    assert(frames != begin);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    psx_t* p = psx_create(); assert(p && psx_init(p, argv[1], NULL) == 0);
    psx_gpu_set_udata(p->gpu, 1, p->timer);
    psx_gpu_set_event_callback(p->gpu, GPU_EVENT_VBLANK, vblank);
    psx_gpu_set_event_callback(p->gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
    psx_gpu_set_event_callback(p->gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
    psx_gpu_set_event_callback(p->gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
    psx_gpu_set_accuracy_flags(p->gpu, PSX_GPU_ACCURACY_MASK_BIT);
    void* data = NULL; size_t cap = 0, size = 0;
    double total = 0, worst = 0;
    for (int i=0; i<300; ++i) {
        clock_t start = clock();
        if (i) assert(psx_load_state_from_memory_ex(p, data, size, PSX_STATE_LOAD_KEEP_DECODE_CACHE) == 0);
        psx_spu_begin_frame(p->spu, 744);
        frame(p);
        uint32_t sample;
        while (psx_spu_pop_sample(p->spu, &sample)) {}
        psx_spu_begin_frame(p->spu, 0);
        assert(psx_save_state_to_memory_ex(p, &data, &cap, &size, PSX_STATE_SAVE_NO_THUMBNAIL) == 0);
        frame(p);
        double ms = 1000.0 * (clock()-start)/CLOCKS_PER_SEC;
        total += ms; if (ms > worst) worst = ms;
        if (i%60 == 59) { printf("BIOS runahead frames=%d mean=%.3f worst=%.3f ms\n",i+1,total/60,worst); total=worst=0; }
    }
    uint64_t hash=14695981039346656037ull;
    for (size_t i=0; i<size; ++i) hash=(hash ^ ((unsigned char*)data)[i])*1099511628211ull;
    printf("BIOS final snapshot=%016llx\n",(unsigned long long)hash);
    free(data); psx_destroy(p); return 0;
}
