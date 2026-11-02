#include "cpu_gte.h"
#include "hardware_register_counters.h"
#if defined(_WIN32)
#define API extern "C" __declspec(dllexport)
#else
#define API extern "C"
#endif
API int emit_block(const uint32_t* words,int count,uint32_t pc,uint8_t* output,int capacity){
    CpuBlock block(pc);for(int i=0;i<count;i++)block.code.emplace_back(words[i],pc+uint32_t(i*4));
    if(!compileArm64(block))return -1;
    if(int(block.executableBytes)>capacity)return -2;memcpy(output,block.executable,block.executableBytes);return int(block.executableBytes);
}
API uintptr_t gte_address(){return reinterpret_cast<uintptr_t>(&executeCompiledGte);}
API int prefix_limit(const uint32_t* words,int count,int clocks,int fetchWait,int ramWait){
    CpuBlock block(0x80010000);block.ramReadClocks.push_back(0);
    for(int i=0;i<count;i++){Decoded d(words[i],block.pc+i*4);block.code.push_back(d);block.ramReadClocks.push_back(block.ramReadClocks.back()+(d.compilable()&&d.memory()&&!d.store()?6:0));}
    return block.instructionLimit(clocks,fetchWait+1,ramWait!=0);
}
API void run_gte(CpuState* state,uint32_t word,int scale,int value){executeCompiledGte(state,word,scale,value);}
API int register_counter_check(){
    HardwareRegisterCounters c;c.record(0x1f801814,4,false,0x80010000);c.record(0x1f801814,4,false,0x80010004);c.record(0x1f801803,1,false,0x80010008);c.record(0x1f8010a8,4,true,0x8001000c);c.record(0xfffe0130,4,true,0x80010010);
    auto s=c.snapshot(true);if(s.size()!=20||s[1]!=3||s[2]!=1||s[4]!=1||s[5]!=0x1f801814||s[6]!=2||s[8]!=4||s[9]!=0x80010004)return 1;
    c.reset();if(c.snapshot(false)!=std::vector<int64_t>({0,0,0,0,0}))return 2;return 0;
}
