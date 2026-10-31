#pragma once

#include <cstdlib>
#include <cstring>
#include <cstdint>
extern "C" {
#include "../psx/psx.h"
#include "../psx/state.h"
#include "../psx/pgxp.h"
}

// Same-process, single-frame memoisation. A prediction is reusable only if the
// entire next starting state (including newly applied input) is byte-identical.
// This removes duplicated execution, not input sampling or emulated frames.
// The on-disk save-state format and user save files are unchanged.
class RunaheadPrediction {
    friend class RunaheadSequence;
    struct Runtime {
        uint32_t samples[PSX_SPU_SAMPLE_RING];
        int head, tail, count, budget;
        uint32_t cycles;
        int loop_address[24];
        uint32_t gpu_status, report_lba, report_anchor;
        int report_accum, read_delay_pending, cdda_irq_phase;
        uint32_t accuracy, read_speed, seek_speed;
        int texture_filter, reverb_disabled, widescreen, cpu_engine;

        void capture(psx_t* p) {
            // Deterministic padding and unused ring entries for the exact key.
            std::memset(this, 0, sizeof(*this));
            auto* s = p->spu;
            head=s->gen_head; tail=s->gen_tail; count=s->gen_count;
            budget=s->gen_budget; cycles=s->gen_cycles;
            for (int n=0; n<count; ++n) {
                const int i=(tail+n)&(PSX_SPU_SAMPLE_RING-1);
                samples[i]=s->gen_ring[i];
            }
            for (int i=0; i<24; ++i) loop_address[i]=s->data[i].ignore_loop_addr;
            gpu_status=p->gpu->gpustat;
            report_lba=p->cdrom->report_lba;
            report_anchor=p->cdrom->report_anchor;
            report_accum=p->cdrom->report_accum;
            read_delay_pending=p->cdrom->read_delay_pending;
            cdda_irq_phase=psx_spu_cdda_irq_phase();
            accuracy=p->gpu->accuracy_flags;
            texture_filter=p->gpu->texture_filter;
            reverb_disabled=s->reverb_disabled;
            widescreen=psx_widescreen_active();
            cpu_engine=psx_cpu_get_execution_mode(p->cpu);
            read_speed=psx_cdrom_get_read_speedup();
            seek_speed=psx_cdrom_get_seek_speedup();
        }

        void restore(psx_t* p) const {
            // Disk state loads intentionally flush queued audio and normalise
            // derived registers. Reusing a computed frame must retain them.
            auto* s=p->spu;
            std::memcpy(s->gen_ring,samples,sizeof(samples));
            s->gen_head=head; s->gen_tail=tail; s->gen_count=count;
            s->gen_budget=budget; s->gen_cycles=cycles;
            for (int i=0; i<24; ++i) s->data[i].ignore_loop_addr=loop_address[i];
            p->gpu->gpustat=gpu_status;
            p->cdrom->report_lba=report_lba;
            p->cdrom->report_anchor=report_anchor;
            p->cdrom->report_accum=report_accum;
            p->cdrom->read_delay_pending=read_delay_pending;
            psx_spu_restore_cdda_irq_phase(cdda_irq_phase);
            // Host options above participate in the key, never in restoration.
        }
    };

    struct Snapshot {
        void* data=nullptr;
        size_t capacity=0, size=0;
        Runtime runtime{};
        ~Snapshot() { std::free(data); }
        bool capture(psx_t* p) {
            size=0;
            if (psx_save_state_to_memory_ex(p,&data,&capacity,&size,
                    PSX_STATE_SAVE_NO_THUMBNAIL)!=PSX_STATE_OK) return false;
            runtime.capture(p);
            return true;
        }
        bool restore(psx_t* p) const {
            if (psx_load_state_from_memory_ex(p,data,size,
                    PSX_STATE_LOAD_KEEP_DECODE_CACHE)!=PSX_STATE_OK) return false;
            runtime.restore(p);
            return true;
        }
        bool matches(const Snapshot& other) const {
            return size && size==other.size &&
                !std::memcmp(&runtime,&other.runtime,sizeof(runtime)) &&
                !std::memcmp(data,other.data,size);
        }
        void clear() {
            std::free(data); data=nullptr; capacity=size=0;
        }
    } start_, end_, probe_;
    psx_t* machine_=nullptr;
    bool started_=false, valid_=false;
    uint32_t steps_=0;

public:
    RunaheadPrediction() = default;
    RunaheadPrediction(const RunaheadPrediction&) = delete;
    RunaheadPrediction& operator=(const RunaheadPrediction&) = delete;

    static bool supported(psx_t* p) {
        if (!p || psx_pgxp_enabled() || psx_get_cpu_overclock()!=100 ||
            p->gpu->texrep || p->gpu->dbg_file) return false;
#ifdef USE_HARDWARE
        if (p->gpu->backend) return false;
#endif
        return true;
    }
    void invalidate() { valid_=started_=false; }
    void clear() {
        invalidate(); machine_=nullptr;
        start_.clear(); end_.clear(); probe_.clear();
    }
    bool reuse(psx_t* p, uint32_t& steps) {
        const bool candidate=valid_ && machine_==p && supported(p);
        invalidate(); // Only the immediately following real frame can reuse it.
        if (!candidate || !probe_.capture(p) || !probe_.matches(start_)) return false;
        if (!end_.restore(p)) {
            probe_.restore(p);
            return false;
        }
        steps=steps_;
        return true;
    }
    bool begin(psx_t* p) {
        invalidate(); machine_=p;
        started_=supported(p) && start_.capture(p);
        return started_;
    }
    void finish(psx_t* p, uint32_t steps) {
        valid_=started_ && machine_==p && supported(p) && end_.capture(p);
        started_=false; steps_=steps;
    }
};
