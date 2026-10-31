#pragma once

#include "runahead_prediction.h"
#include <vector>

// A bounded chain of consecutive predictions. Validate the entire new root
// before reusing its descendants; a changed input/state rebuilds the chain.
// Future audio advances emulated state into a private buffer, never the device.
class RunaheadSequence {
    struct Entry {
        RunaheadPrediction frame;
        double audio_remainder=0;
    } entries_[5];
    int depth_=0, head_=0, count_=0;
    double samples_per_frame_=0;
    void* canonical_=nullptr;
    size_t canonical_capacity_=0;
    std::vector<uint32_t> silent_audio_;

    Entry& entry(int offset) { return entries_[(head_+offset)%depth_]; }
    bool canonicalize(psx_t* p) {
        size_t size=0;
        return psx_save_state_to_memory_ex(p,&canonical_,&canonical_capacity_,&size,
                   PSX_STATE_SAVE_NO_THUMBNAIL)==PSX_STATE_OK &&
               psx_load_state_from_memory_ex(p,canonical_,size,
                   PSX_STATE_LOAD_KEEP_DECODE_CACHE)==PSX_STATE_OK;
    }
    bool fail() { invalidate(); return false; }

public:
    RunaheadSequence() = default;
    RunaheadSequence(const RunaheadSequence&) = delete;
    RunaheadSequence& operator=(const RunaheadSequence&) = delete;
    ~RunaheadSequence() { std::free(canonical_); }

    void invalidate() {
        for(auto& e:entries_) e.frame.invalidate();
        head_=count_=0;
    }
    void clear() {
        invalidate();
        for(auto& e:entries_) e.frame.clear();
        std::free(canonical_); canonical_=nullptr; canonical_capacity_=0;
        std::vector<uint32_t>().swap(silent_audio_);
        depth_=0; samples_per_frame_=0;
    }
    bool configure(int depth, double samples_per_frame) {
        if(depth<2 || depth>5 || !(samples_per_frame>0) ||
           samples_per_frame>PSX_SPU_SAMPLE_RING-1) { clear(); return false; }
        if(depth!=depth_ || samples_per_frame!=samples_per_frame_) {
            clear(); depth_=depth; samples_per_frame_=samples_per_frame;
        }
        return true;
    }
    bool reuseReal(psx_t* p, uint32_t& steps) {
        if(!count_) return false;
        if(!entry(0).frame.reuse(p,steps)) return fail();
        head_=(head_+1)%depth_;
        --count_;
        return true;
    }

    // Caller has restored the saved real frame. On failure the caller must
    // restore it again before using the ordinary runahead path.
    template<class Step, class Mix>
    bool predict(psx_t* p, double accumulator, int& reused, Step step, Mix mix) {
        reused=0;
        if(!depth_ || !RunaheadPrediction::supported(p)) return fail();
        const int pal_mode=psx_gpu_is_pal_mode(p->gpu);
        // Disk states omit this process-local divider. Speculative audio must
        // not leave it advanced when the caller restores the real timeline.
        struct RestoreAudioPhase {
            int phase=psx_spu_cdda_irq_phase();
            ~RestoreAudioPhase() { psx_spu_restore_cdda_irq_phase(phase); }
        } restore_audio_phase;
        const auto next_samples=[&]() {
            accumulator+=samples_per_frame_;
            int samples=static_cast<int>(accumulator);
            accumulator-=samples;
            return samples;
        };
        int samples=next_samples();
        psx_spu_begin_frame(p->spu,samples);

        if(count_) {
            auto& first=entry(0);
            auto& key=first.frame;
            // Matching the fractional audio phase and rate is necessary even
            // when this frame's integer sample budget happens to be unchanged.
            const bool matches=key.valid_ && key.machine_==p &&
                first.audio_remainder==accumulator && key.probe_.capture(p) &&
                key.probe_.matches(key.start_);
            if(matches) {
                // Each descendant was generated from this exact root with the
                // same held inputs and audio schedule; skip to the chain's end.
                if(!entry(count_-1).frame.end_.restore(p)) return fail();
                reused=count_;
                for(int i=1;i<count_;++i) samples=next_samples();
            } else {
                invalidate();
            }
        }

        while(count_<depth_) {
            if(count_) {
                silent_audio_.resize(samples);
                mix(p,reinterpret_cast<uint8_t*>(silent_audio_.data()),samples*4);
                psx_spu_begin_frame(p->spu,0);
                if(!canonicalize(p)) return fail();
                samples=next_samples();
            }
            psx_spu_begin_frame(p->spu,samples);
            auto& next=entry(count_);
            next.audio_remainder=accumulator;
            if(!next.frame.begin(p)) return fail();
            const uint32_t steps=step();
            // A game switching PAL/NTSC changes subsequent audio budgets.
            // Rebuild via the ordinary path for this transition frame.
            if(!steps || psx_gpu_is_pal_mode(p->gpu)!=pal_mode) return fail();
            next.frame.finish(p,steps);
            if(!next.frame.valid_) return fail();
            ++count_;
        }
        psx_spu_begin_frame(p->spu,0);
        return true;
    }
};
