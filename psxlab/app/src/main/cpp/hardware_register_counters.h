#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>

/** CPU-thread-only, fixed storage; includes shadow-latch accesses that avoid JNI. */
struct HardwareRegisterCounters {
    struct Entry {uint64_t reads=0,writes=0;uint32_t widths=0,pc=0;};
    std::array<Entry,4096> registers{};
    uint64_t reads=0,writes=0,otherReads=0,otherWrites=0;
    void reset(){registers.fill(Entry{});reads=writes=otherReads=otherWrites=0;}
    void record(uint32_t address,int width,bool write,uint32_t pc){
        if(address<0x1f801000||address>=0x1f802000){if(write)otherWrites++;else otherReads++;return;}
        Entry& entry=registers[address-0x1f801000];entry.pc=pc;entry.widths|=uint32_t(width)<<(write?8:0);
        if(write){writes++;entry.writes++;}else{reads++;entry.reads++;}
    }
    std::vector<int64_t> snapshot(bool enabled)const{
        std::vector<unsigned> addresses;for(unsigned i=0;i<registers.size();i++)if(registers[i].reads||registers[i].writes)addresses.push_back(i);
        std::sort(addresses.begin(),addresses.end(),[&](unsigned a,unsigned b){uint64_t x=registers[a].reads+registers[a].writes,y=registers[b].reads+registers[b].writes;return x!=y?x>y:a<b;});
        if(addresses.size()>16)addresses.resize(16);
        std::vector<int64_t> out{enabled?1:0,int64_t(reads),int64_t(writes),int64_t(otherReads),int64_t(otherWrites)};
        for(unsigned i:addresses){const Entry& e=registers[i];out.insert(out.end(),{int64_t(0x1f801000+i),int64_t(e.reads),int64_t(e.writes),int64_t(e.widths),int64_t(e.pc)});}
        return out;
    }
};
