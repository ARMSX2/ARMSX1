#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <memory>
#include <unordered_map>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>

// Shared with NativeCpu.java. All state and memory belong to the core thread.
struct CpuState {
    int32_t r[32],cop0[32];
    uint32_t pc,next;int32_t hi,lo;uint32_t loadReg;int32_t loadValue;
    uint32_t branchPending,branchTaken,faultBranchTaken,faultBranchTarget;
    uint32_t lastException,faultPc,faultInstruction,cacheIsolated;
    uint64_t instructions,exceptions;
    int32_t gte[64];
};
static_assert(offsetof(CpuState,instructions)==312 && offsetof(CpuState,gte)==328 && sizeof(CpuState)==584);
struct Decoded {
    uint32_t word,at;int op,s,t,d,imm,u,fun,shift;
    Decoded(uint32_t w,uint32_t p):word(w),at(p),op(w>>26),s((w>>21)&31),t((w>>16)&31),d((w>>11)&31),imm(int16_t(w)),u(w&65535),fun(w&63),shift((w>>6)&31){}
    bool store()const{return op==40||op==41||op==42||op==43||op==46||op==58;}
    bool compilable()const {
        if(op==0)return fun==0||fun==2||fun==3||fun==4||fun==6||fun==7||fun==33||fun==35||fun==36||fun==37||fun==38||fun==39||fun==42||fun==43;
        return op==9||op==10||op==11||op==12||op==13||op==14||op==15||op==32||op==33||op==35||op==36||op==37||op==40||op==41||op==43;
    }
    bool memory()const{return op>=32;}
    bool branch()const{return (op>=1&&op<=7)||(op==0&&(fun==8||fun==9));}
};
struct CpuCodePage {
    inline static std::atomic<size_t> allocatedBytes{0};
    void* memory;size_t bytes,used=0;bool executable=false;
    explicit CpuCodePage(size_t n):memory(mmap(nullptr,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)),bytes(n){if(memory!=MAP_FAILED)allocatedBytes.fetch_add(bytes,std::memory_order_relaxed);}
    ~CpuCodePage(){if(memory!=MAP_FAILED){munmap(memory,bytes);allocatedBytes.fetch_sub(bytes,std::memory_order_relaxed);}}
};
struct CpuBlock {
    uint32_t pc;uint64_t version;int page;
    std::vector<Decoded> code;
    std::vector<int> ramReadClocks;
    void* executable=nullptr;size_t executableBytes=0;
    std::shared_ptr<CpuCodePage> codePage;
    using Function=int(*)(CpuState*,uint8_t*,uint64_t*,uint8_t*);
    Function function=nullptr;
    CpuBlock* links[2]={};
    explicit CpuBlock(uint32_t p):pc(p),version(0),page(-1){}
};
bool compileArm64(CpuBlock& block);
