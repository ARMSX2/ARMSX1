#pragma once
#include <jni.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

// Opt-in API/host timings, never hardware GPU execution timestamps.
struct NativeTiming {
    enum Path {CPU_COMPILE,CPU_CHECK,CPU_CLEAR,GPU_DRAW,GPU_TRANSFER,GPU_READBACK,
               GPU_SCANOUT,GLES_PRESENT,GL_PRODUCER,VK_FENCE,VK_ACQUIRE,VK_SUBMIT,
               VK_PRESENT,VK_ALLOCATION,CPU_BUILD,GPU_RESOLVE,GL_COMMAND_UPLOAD,GL_WINDOW,GL_SWAP,COUNT};
    struct Counter {
        std::atomic<uint64_t> calls{0},wall{0},cpu{0},maximum{0},maximumCpu{0},at{0},tid{0},context{0};
    };
    std::atomic<bool> enabled{false};std::array<Counter,COUNT> counters;
    static uint64_t now(clockid_t clock){timespec t{};clock_gettime(clock,&t);return uint64_t(t.tv_sec)*1000000000u+uint64_t(t.tv_nsec);}
    struct Scope {
        Counter* counter=nullptr;uint64_t start=0,cpuStart=0,context=0;
        Scope(NativeTiming& timing,Path path,uint64_t pc=0):context(pc){
            if(!timing.enabled.load(std::memory_order_relaxed))return;
            counter=&timing.counters[path];start=now(CLOCK_MONOTONIC);cpuStart=now(CLOCK_THREAD_CPUTIME_ID);
            if(!counter->tid.load(std::memory_order_relaxed))counter->tid.store(uint64_t(syscall(SYS_gettid)),std::memory_order_relaxed);
        }
        ~Scope(){if(!counter)return;uint64_t wall=now(CLOCK_MONOTONIC)-start,cpu=now(CLOCK_THREAD_CPUTIME_ID)-cpuStart;
            counter->calls.fetch_add(1,std::memory_order_relaxed);counter->wall.fetch_add(wall,std::memory_order_relaxed);counter->cpu.fetch_add(cpu,std::memory_order_relaxed);
            if(wall>counter->maximum.load(std::memory_order_relaxed)){counter->maximum=wall;counter->maximumCpu=cpu;counter->at=start;counter->context=context;}
        }
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    };
    jlongArray snapshot(JNIEnv* env)const{
        jlong values[COUNT*8];int i=0;for(const auto& c:counters){values[i++]=c.calls.load();values[i++]=c.wall.load();values[i++]=c.cpu.load();values[i++]=c.maximum.load();values[i++]=c.maximumCpu.load();values[i++]=c.at.load();values[i++]=c.tid.load();values[i++]=c.context.load();}
        jlongArray out=env->NewLongArray(COUNT*8);if(out)env->SetLongArrayRegion(out,0,COUNT*8,values);return out;
    }
};
