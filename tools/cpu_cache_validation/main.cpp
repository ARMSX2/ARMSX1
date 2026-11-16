#include "cpu_cache_eviction.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <unordered_set>

struct Block {
    uint32_t pc;bool retiring=false;Block* links[2]{};
    explicit Block(uint32_t address):pc(address){}
};

int main(){
    std::unordered_map<uint32_t,std::unique_ptr<Block>> blocks;
    std::deque<uint32_t> order;
    for(uint32_t i=0;i<65536;i++){auto block=std::make_unique<Block>(i*4);blocks.emplace(i*4,std::move(block));order.push_back(i*4);}
    Block* protectedBlock=blocks.at(0).get();
    for(uint32_t i=0;i<65536;i++){
        blocks.at(i*4)->links[0]=blocks.at(((i+1)%65536)*4).get();
        blocks.at(i*4)->links[1]=protectedBlock;
    }
    const auto start=std::chrono::steady_clock::now();
    assert(evictCpuBlocks(blocks,order,128,protectedBlock)==128);
    assert(blocks.size()==65536-128);assert(blocks.at(0).get()==protectedBlock);
    for(uint32_t i=1;i<=128;i++)assert(blocks.count(i*4)==0);
    std::unordered_set<Block*> live;for(auto& entry:blocks)live.insert(entry.second.get());
    for(auto& entry:blocks)for(Block* link:entry.second->links)assert(!link||live.count(link));
    assert(protectedBlock->links[0]==nullptr);
    for(uint32_t i=1;i<=128;i++){blocks.emplace(i*4,std::make_unique<Block>(i*4));order.push_back(i*4);}
    assert(evictCpuBlocks(blocks,order,128,protectedBlock)==128);
    live.clear();for(auto& entry:blocks)live.insert(entry.second.get());
    for(auto& entry:blocks)for(Block* link:entry.second->links)assert(!link||live.count(link));
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("65,536-block capacity: bounded eviction, protected blocks, incoming links and reinsertion passed (%lld us including validation).\n",static_cast<long long>(elapsed));
}
