#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>

/** CPU-thread-only, fixed storage; includes shadow-latch accesses that avoid JNI. */
struct HardwareRegisterCounters {
    struct Entry {uint64_t reads=0,writes=0;uint32_t widths=0,pc=0;};
    std::array<Entry,4096> registers{};
    std::array<uint64_t,4096> markReads{},markWrites{}; // counts at the previous deltas() call
    std::array<std::pair<uint64_t,uint64_t>,4096> change{}; // deltas() scratch, kept off the stack
    uint64_t reads=0,writes=0,otherReads=0,otherWrites=0;
    void reset(){registers.fill(Entry{});markReads.fill(0);markWrites.fill(0);reads=writes=otherReads=otherWrites=0;}
    void record(uint32_t address,int width,bool write,uint32_t pc){
        if(address<0x1f801000||address>=0x1f802000){if(write)otherWrites++;else otherReads++;return;}
        Entry& entry=registers[address-0x1f801000];entry.pc=pc;entry.widths|=uint32_t(width)<<(write?8:0);
        if(write){writes++;entry.writes++;}else{reads++;entry.reads++;}
    }
    /** The busiest registers since the previous call, as {address, reads, writes, last pc} groups:
     * shows what a game polls right now (a wait loop), which cumulative totals hide. */
    std::vector<int64_t> deltas(size_t limit){
        std::vector<std::pair<uint64_t,unsigned>> busy;
        for(unsigned i=0;i<registers.size();i++){change[i]={registers[i].reads-markReads[i],registers[i].writes-markWrites[i]};uint64_t d=change[i].first+change[i].second;if(d)busy.push_back({d,i});markReads[i]=registers[i].reads;markWrites[i]=registers[i].writes;}
        std::sort(busy.begin(),busy.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first>b.first:a.second<b.second;});
        if(busy.size()>limit)busy.resize(limit);
        std::vector<int64_t> out;
        for(const auto& [d,i]:busy){const Entry& e=registers[i];out.insert(out.end(),{int64_t(0x1f801000+i),int64_t(change[i].first),int64_t(change[i].second),int64_t(e.pc)});}
        return out;
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
