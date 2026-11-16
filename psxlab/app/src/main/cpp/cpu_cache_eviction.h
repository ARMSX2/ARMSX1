#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

// Retire a bounded number of blocks without leaving raw successor links dangling.
// Victims remain alive until every incoming link has been detached.
template<class Block>
size_t evictCpuBlocks(std::unordered_map<uint32_t,std::unique_ptr<Block>>& blocks,
                     std::deque<uint32_t>& order,size_t count,Block* protectedBlock=nullptr){
    std::vector<Block*> victims;victims.reserve(count);
    size_t remaining=order.size();
    while(remaining--&&!order.empty()&&victims.size()<count){
        uint32_t pc=order.front();order.pop_front();auto it=blocks.find(pc);
        if(it==blocks.end())continue;
        Block* block=it->second.get();
        if(block==protectedBlock){order.push_back(pc);continue;}
        block->retiring=true;victims.push_back(block);
    }
    if(victims.empty())return 0;
    for(auto& entry:blocks)for(Block*& link:entry.second->links)
        if(link&&link->retiring)link=nullptr;
    for(Block* block:victims)blocks.erase(block->pc);
    return victims.size();
}
