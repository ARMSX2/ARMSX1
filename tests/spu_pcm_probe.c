#include "psx/dev/spu.h"
#include "psx/perf.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#undef puts
void psx_ic_irq(psx_ic_t* ic, int irq) { (void)ic; (void)irq; }
static int calls;
static int16_t captured[3][2];
static void probe(void* context, int source, const int16_t* samples, size_t frames) {
    assert(context == &calls && frames == 1 && source >= 1 && source <= 2);
    ++calls;
    captured[source][0] = samples[0]; captured[source][1] = samples[1];
}
int main(void) {
    psx_spu_t* s = calloc(1, sizeof(*s)); assert(s);
    s->spucnt = 0x4000; s->mainlvol = s->mainrvol = 0x3fff;
    s->data[0].playing = 1; s->data[0].adsr_cycles = 10000;
    s->voice[0].envcvol = 0x7fff;
    s->voice[0].volumel = s->voice[0].volumer = 0x3fff;
    for (int i=0;i<28;++i) s->data[0].buf[i] = 1000;
    for (int i=0;i<4;++i) s->data[0].s[i] = 1000;
    const uint32_t baseline = psx_spu_get_sample(s);
    g_psx_audio_pcm_probe_context = &calls; g_psx_audio_pcm_probe = probe;
    assert(psx_spu_get_sample(s) == baseline);
    assert(calls == 2);
    assert(captured[1][0] == (int16_t)baseline);
    assert(captured[1][1] == (int16_t)(baseline >> 16));
    assert(captured[2][0] == 0 && captured[2][1] == 0);
    s->spucnt = 0;
    assert(psx_spu_get_sample(s) == 0 && calls == 4);
    assert(captured[1][0] == 0 && captured[2][0] == 0);
    g_psx_audio_pcm_probe = NULL;
    psx_spu_get_sample(s); assert(calls == 4);
    free(s); puts("SPU PCM probe tests passed");
}
