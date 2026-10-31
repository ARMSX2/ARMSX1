extern "C" {
#include "../psx/psx.h"
#include "../psx/state.h"
}
#include "../frontend/runahead_prediction.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
static void MixPsxAudio(psx_t* p, uint8_t* bytes, int size) {
    memset(bytes,0,size);
    psx_cdrom_get_audio_samples(p->cdrom,bytes,size);
    psx_spu_update_cdda_buffer(p->spu,p->cdrom->cdda_buf);
    auto* out=(int16_t*)bytes;
    for(int i=0;i<size/4;++i) {
        uint32_t sample=0;
        if(!psx_spu_pop_sample(p->spu,&sample)) sample=psx_spu_get_sample(p->spu);
        out[2*i]=(int16_t)std::clamp((int)out[2*i]+(int)(int16_t)sample,-32768,32767);
        out[2*i+1]=(int16_t)std::clamp((int)out[2*i+1]+(int)(int16_t)(sample>>16),-32768,32767);
    }
}

struct Snapshot {
    void* data = nullptr;
    size_t capacity = 0, size = 0;
    psx_spu_t spu{};
    uint32_t gpustat=0, report_lba=0, report_anchor=0;
    int read_delay_pending=0, report_accum=0, cdda_counter=0;
    ~Snapshot() { free(data); }
    void save(psx_t* p) {
        assert(psx_save_state_to_memory_ex(p, &data, &capacity, &size,
                   PSX_STATE_SAVE_NO_THUMBNAIL) == PSX_STATE_OK);
    }
    void load(psx_t* p) const {
        assert(psx_load_state_from_memory_ex(p, data, size,
                   PSX_STATE_LOAD_KEEP_DECODE_CACHE) == PSX_STATE_OK);
    }
    bool equals(const Snapshot& other) const {
        return size == other.size && !memcmp(data, other.data, size);
    }
    void saveExact(psx_t* p) {
        save(p);
        spu=*p->spu;
        if(!spu.gen_count) memset(spu.gen_ring,0,sizeof(spu.gen_ring));
        gpustat=p->gpu->gpustat;
        report_lba=p->cdrom->report_lba;
        report_anchor=p->cdrom->report_anchor;
        report_accum=p->cdrom->report_accum;
        read_delay_pending=p->cdrom->read_delay_pending;
        cdda_counter=psx_spu_cdda_irq_phase();
    }
    void loadExact(psx_t* p) const {
        load(p);
        *p->spu=spu;
        p->gpu->gpustat=gpustat;
        p->cdrom->report_lba=report_lba;
        p->cdrom->report_anchor=report_anchor;
        p->cdrom->report_accum=report_accum;
        p->cdrom->read_delay_pending=read_delay_pending;
        psx_spu_restore_cdda_irq_phase(cdda_counter);
    }
    bool equalsExact(const Snapshot& b) const {
        return equals(b) && !memcmp(&spu,&b.spu,sizeof(spu)) && gpustat==b.gpustat &&
               report_lba==b.report_lba && report_anchor==b.report_anchor &&
               report_accum==b.report_accum && read_delay_pending==b.read_delay_pending &&
               cdda_counter==b.cdda_counter;
    }
};
static unsigned frames;
static void vblank(psx_gpu_t* gpu) { ++frames; psxe_gpu_vblank_timer_event_cb(gpu); }
static unsigned frame(psx_t* p) {
    unsigned begin = frames, steps = 0;
    while (frames == begin && ++steps < 2000000) psx_update(p);
    assert(frames != begin);
    return steps;
}
static void compare(const Snapshot& a, const Snapshot& b, int i) {
    if (a.equals(b)) return;
    fprintf(stdout, "MISMATCH frame=%d sizes=%zu,%zu\n", i, a.size, b.size);
    int count=0;
    for (size_t n=0; n<a.size && n<b.size && count<12; ++n) {
        auto av=((unsigned char*)a.data)[n], bv=((unsigned char*)b.data)[n];
        if (av!=bv) { printf("offset=%zu %02x != %02x\n",n,av,bv); ++count; }
    }
    exit(2);
}
int main(int argc, char** argv) {
    assert(argc >= 2);
    bool verify = argc > 2 && !strcmp(argv[2], "verify");
    bool baseline = argc > 2 && !strcmp(argv[2], "baseline");
    psx_t* p=psx_create(); assert(p && psx_init(p,argv[1],nullptr)==0);
    if(argc>3) assert(psx_cdrom_open(p->cdrom,argv[3])!=0);
    psx_gpu_set_udata(p->gpu,1,p->timer);
    psx_gpu_set_event_callback(p->gpu,GPU_EVENT_VBLANK,vblank);
    psx_gpu_set_event_callback(p->gpu,GPU_EVENT_HBLANK,psxe_gpu_hblank_event_cb);
    psx_gpu_set_event_callback(p->gpu,GPU_EVENT_VBLANK_END,psxe_gpu_vblank_end_event_cb);
    psx_gpu_set_event_callback(p->gpu,GPU_EVENT_HBLANK_END,psxe_gpu_hblank_end_event_cb);
    psx_gpu_set_accuracy_flags(p->gpu,PSX_GPU_ACCURACY_MASK_BIT);
    auto input=psx_input_create(); psx_input_init(input);
    auto sda=psxi_sda_create(); psxi_sda_init(sda,SDA_MODEL_DIGITAL);
    psxi_sda_set_analog_mode(sda,1); psxi_sda_init_input(sda,input);
    psx_pad_attach_joy(p->pad,0,input); p->pad->dest[0]=0;
    const bool cards=argc>5 && !strcmp(argv[5], "two-cards");
    if(cards) for(int slot=0;slot<2;++slot) {
        auto* card=psx_mcd_create();
        assert(card && psx_mcd_init(card,nullptr)==0);
        assert(!p->pad->mcd_slot[slot]);
        p->pad->mcd_slot[slot]=card; // In-memory fixtures; never read/write user saves.
        card->session_id=slot+1; // Stable test identity for independent-run hashes.
        for(size_t n=0;n<MCD_MEMORY_SIZE;++n) card->buf[n]=(uint8_t)(n*17+slot);
    }
    Snapshot real, probe, reference, reused;
    RunaheadPrediction prediction;
    unsigned hits=0, misses=0; double accumulator=0,total=0,worst=0;
    uint64_t pcm_hash=14695981039346656037ull;
    const int total_frames=argc>4 ? atoi(argv[4]) : 360;
    for(int i=0;i<total_frames;++i) {
        auto start=clock();
        if(i) real.load(p);
        if(i%31==0) {
            if((i/31)%2) psx_pad_button_press(p->pad,0,PSXI_SW_SDA_CROSS);
            else psx_pad_button_release(p->pad,0,PSXI_SW_SDA_CROSS);
        }
        if(i%47==0) psx_pad_analog_change(p->pad,0,PSXI_AX_SDA_LEFT_HORZ,(i*13)&255);
        if(i==210 || i==250) p->ram->buf[0x1ff000]^=1; // External state mutation must miss.
        if(i==90 || i==180) p->gpu->texture_filter^=1;
        if(i==135 || i==270) p->spu->reverb_disabled^=1;
        if(i==310) prediction.invalidate(); // Pause, reset or user state-load boundary.
        if(cards && (i==410 || i==450)) {
            auto* card=p->pad->mcd_slot[(i==410)?0:1];
            card->buf[0x10000]^=1;
            card->hash_valid=0;
            ++card->write_generation;
        }
        accumulator+=44100.0/59.292862;
        int samples=(int)accumulator; accumulator-=samples;
        psx_spu_begin_frame(p->spu,samples);
        probe.saveExact(p);
        bool hit=false;
        unsigned steps=0;
        if(verify) {
            frame(p); reference.saveExact(p);
            probe.loadExact(p);
        }
        if(!baseline) hit=prediction.reuse(p,steps);
        if(hit) ++hits; else ++misses;
        if(i==210 || i==250 || (i && i%31==0) || (i && i%47==0)) assert(!hit);
        if(i==90 || i==180 || i==135 || i==270 || i==310) assert(!hit);
        if(cards && (i==410 || i==450)) assert(!hit);
        if(!hit) frame(p);
        if(verify) {
            reused.saveExact(p); compare(reference,reused,i);
            assert(reference.equalsExact(reused));
        }
        std::vector<uint32_t> audio(samples);
        MixPsxAudio(p,(uint8_t*)audio.data(),samples*4);
        for(uint32_t sample:audio) {
            pcm_hash=(pcm_hash^sample)*1099511628211ull;
        }
        psx_spu_begin_frame(p->spu,0);
        real.save(p);
        real.load(p); // Match the canonical state restored before the next real frame.
        int next_samples=(int)(accumulator+44100.0/59.292862);
        psx_spu_begin_frame(p->spu,next_samples);
        assert(prediction.begin(p));
        steps=frame(p);
        prediction.finish(p,steps);
        double ms=1000.0*(clock()-start)/CLOCKS_PER_SEC;
        total+=ms; if(ms>worst) worst=ms;
        if(i%60==59) { printf("reuse=%s frames=%d hits=%u misses=%u mean=%.3f worst=%.3f ms\n",verify?"verify":baseline?"baseline":"fast",i+1,hits,misses,total/60,worst); fflush(stdout); total=worst=0; }
    }
    real.load(p); real.save(p);
    uint64_t hash=14695981039346656037ull;
    for(size_t n=0;n<real.size;++n) hash=(hash^((unsigned char*)real.data)[n])*1099511628211ull;
    printf("RESULT state=%016llx pcm=%016llx hits=%u misses=%u\n",(unsigned long long)hash,(unsigned long long)pcm_hash,hits,misses);
    psx_destroy(p);
}
