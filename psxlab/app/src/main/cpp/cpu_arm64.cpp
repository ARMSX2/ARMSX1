// Original ARM64 translator. Guards preserve event deadlines, delay slots and MMIO fallback.
#include "cpu_core.h"
#include "cpu_gte.h"
#include <algorithm>

#if defined(__aarch64__)
namespace {
struct Assembler {
    std::vector<uint32_t> words;
    void emit(uint32_t w){words.push_back(w);}
    void mov(int d,uint32_t v){emit(0x52800000|((v&65535)<<5)|d);if(v>>16)emit(0x72a00000|((v>>16)<<5)|d);}
    void pointer(int d,uintptr_t value){emit(0xd2800000|((value&65535)<<5)|d);for(int part=1;part<4;part++)if((value>>(part*16))&65535)emit(0xf2800000|(part<<21)|(((value>>(part*16))&65535)<<5)|d);}
    void load(int d,int offset){emit(0xb9400000|((offset/4)<<10)|d);}
    void store(int r,int offset){emit(0xb9000000|((offset/4)<<10)|r);}
    void binary(uint32_t op,int d,int a,int b){emit(op|(b<<16)|(a<<5)|d);}
    void immediate(int d,int a,int v){if(v>=0 && v<4096)emit(0x11000000|(v<<10)|(a<<5)|d);else{mov(12,uint32_t(v));binary(0x0b000000,d,a,12);}}
    void compare(int d,int a,int b,int condition){binary(0x6b000000,31,a,b);emit(0x1a9f07e0|((condition^1)<<12)|d);}
    size_t jump(uint32_t op){size_t p=words.size();emit(op);return p;}
    void patch(size_t p,size_t target,bool conditional){int64_t delta=int64_t(target)-int64_t(p);words[p]|=conditional?(uint32_t(delta)&0x7ffff)<<5:uint32_t(delta)&0x3ffffff;}
    void commit(int pending){
        if(pending<0){load(13,offsetof(CpuState,loadReg));load(14,offsetof(CpuState,loadValue));size_t skip=jump(0x34000000|13);emit(0x8b000000|(13<<16)|(2<<10)|15);emit(0xb9000000|(15<<5)|14);patch(skip,words.size(),true);}
        else if(pending>0)store(14,pending*4);
    }
    void finish(int count,int pending){
        if(pending>=0){mov(12,pending);store(12,offsetof(CpuState,loadReg));}
        if(pending>0)store(14,offsetof(CpuState,loadValue));
        load(12,offsetof(CpuState,pc));immediate(12,12,count*4);store(12,offsetof(CpuState,pc));store(12,offsetof(CpuState,faultBranchTarget));immediate(12,12,4);store(12,offsetof(CpuState,next));
        store(31,offsetof(CpuState,faultBranchTaken));mov(0,count);emit(0xd65f03c0);
    }
};
}
#endif

bool compileArm64(CpuBlock& block){
#if defined(__aarch64__)
    if(block.code.empty() || (!block.code.front().compilable() && !(block.code.size()>=2&&block.code.front().branch())))return false;
    Assembler a;a.emit(0xd503245f);
    struct Exit {size_t branch;int count,pending;};std::vector<Exit> exits;
    int pending=-1,count=0;bool controlFinished=false;
    auto guard=[&](uint32_t op){exits.push_back({a.jump(op),count,pending});};
    auto instruction=[&](const Decoded& d){
        a.load(9,d.s*4);a.load(10,d.t*4);
        if(d.memory()){
            a.immediate(11,9,d.imm);a.emit(0x53007000|(11<<5)|11);
            a.mov(12,0x800000);a.binary(0x6b000000,31,11,12);guard(0x54000002);
            int width=(d.op==33||d.op==37||d.op==41)?2:(d.op==35||d.op==43)?4:1;
            if(width>1){a.emit(0x53000000|((width==4?1:0)<<10)|(11<<5)|12);guard(0x35000000|12);}
            a.emit(0x53005000|(11<<5)|11);
        }
        if(d.gte()){a.load(11,offsetof(CpuState,cop0)+12*4);a.mov(12,0x40000000);a.binary(0x6a000000,31,11,12);guard(0x54000000);}
        a.commit(pending);pending=0;
        if(d.gte()){
            // Save caller-owned arguments and LR around the shared, JNI-free geometry helper.
            a.emit(0xa9bc07e0);a.emit(0xa9010fe2);a.emit(0xa90217e4);a.emit(0xf9001bfe);
            a.mov(1,d.word);a.binary(0x2a000000,2,31,5);a.binary(0x2a000000,3,31,10);
            a.pointer(16,reinterpret_cast<uintptr_t>(&executeCompiledGte));a.emit(0xd63f0200);
            a.emit(0xf9401bfe);a.emit(0xa94217e4);a.emit(0xa9410fe2);a.emit(0xa8c407e0);
            if(d.s==0||d.s==2){a.load(14,offsetof(CpuState,loadValue));pending=d.t;}
            return;
        }
        if(d.op==0&&d.fun>=16&&d.fun<=27){
            if(d.fun==16||d.fun==18){a.load(11,d.fun==16?offsetof(CpuState,hi):offsetof(CpuState,lo));if(d.d)a.store(11,d.d*4);}
            else if(d.fun==17||d.fun==19)a.store(9,d.fun==17?offsetof(CpuState,hi):offsetof(CpuState,lo));
            else if(d.fun==24||d.fun==25){a.binary(d.fun==24?0x9b207c00:0x9ba07c00,11,9,10);a.store(11,offsetof(CpuState,lo));a.emit(0xd360fc00|(11<<5)|12);a.store(12,offsetof(CpuState,hi));}
            else{
                size_t nonzero=a.jump(0x35000000|10);a.mov(11,0xffffffff);
                if(d.fun==26){a.mov(12,1);a.binary(0x6b000000,31,9,31);a.emit(0x1a800000|(12<<16)|(10<<12)|(11<<5)|11);}
                a.store(11,offsetof(CpuState,lo));a.store(9,offsetof(CpuState,hi));size_t done=a.jump(0x14000000);a.patch(nonzero,a.words.size(),true);
                a.binary(d.fun==26?0x1ac00c00:0x1ac00800,11,9,10);a.emit(0x1b008000|(10<<16)|(9<<10)|(11<<5)|12);
                a.store(11,offsetof(CpuState,lo));a.store(12,offsetof(CpuState,hi));a.patch(done,a.words.size(),false);
            }
            return;
        }
        int dest=d.op==0?d.d:d.t;
        if(d.memory()){
            a.binary(0x8b000000,15,1,11);
            if(d.store()){
                // The store ends the block and pending loads are committed, so W13/W14 are free here.
                // Read the old bytes first: rewriting code with identical bytes keeps its page valid.
                a.emit((d.op==40?0x39400000:d.op==41?0x79400000:0xb9400000)|(15<<5)|13); // LDRB/LDRH/LDR W13,[X15].
                uint32_t op=d.op==40?0x39000000:d.op==41?0x79000000:0xb9000000;a.emit(op|(15<<5)|10);
                int stored=10;if(d.op!=43){a.emit((d.op==40?0x53001c00:0x53003c00)|(10<<5)|14);stored=14;} // UXTB/UXTH W14,W10.
                a.binary(0x6b000000,31,13,stored);size_t unchanged=a.jump(0x54000000); // CMP; B.EQ clean.
                // Data sharing an executable page must not invalidate unrelated code.
                a.emit(0x53057c00|(11<<5)|12);a.binary(0x8b000000,15,3,12);a.emit(0x39400000|(15<<5)|13);
                a.emit(0x53021000|(11<<5)|12);a.mov(16,1);a.binary(0x1ac02000,16,16,12);a.binary(0x6a000000,31,13,16);size_t clean=a.jump(0x54000000);
                // A store overlapping decoded code ends and invalidates this block.
                a.emit(0x530c7c00|(11<<5)|12); // LSR W12,W11,#12.
                a.emit(0x8b000000|(12<<16)|(3<<10)|(2<<5)|15);
                a.emit(0xf9400000|(15<<5)|16);a.emit(0x91000610);a.emit(0xf9000000|(15<<5)|16);
                a.patch(clean,a.words.size(),true);a.patch(unchanged,a.words.size(),true);
            }else{
                uint32_t op=d.op==32?0x39c00000:d.op==33?0x79c00000:d.op==35?0xb9400000:d.op==36?0x39400000:0x79400000;
                a.emit(op|(15<<5)|14);pending=d.t;
                // Preserve the last load value even for a load targeting r0.
                a.store(14,offsetof(CpuState,loadValue));
            }
        }else{
            if(d.op==0){switch(d.fun){
                case 0:a.emit(0x53000000|(((32-d.shift)&31)<<16)|((31-d.shift)<<10)|(10<<5)|11);break;
                case 2:a.emit(0x53007c00|(d.shift<<16)|(10<<5)|11);break;
                case 3:a.emit(0x13007c00|(d.shift<<16)|(10<<5)|11);break;
                case 4:a.binary(0x1ac02000,11,10,9);break;
                case 6:a.binary(0x1ac02400,11,10,9);break;
                case 7:a.binary(0x1ac02800,11,10,9);break;
                case 33:a.binary(0x0b000000,11,9,10);break;
                case 35:a.binary(0x4b000000,11,9,10);break;
                case 36:a.binary(0x0a000000,11,9,10);break;
                case 37:a.binary(0x2a000000,11,9,10);break;
                case 38:a.binary(0x4a000000,11,9,10);break;
                case 39:a.binary(0x2a000000,11,9,10);a.binary(0x2a200000,11,31,11);break;
                case 42:a.compare(11,9,10,11);break;
                case 43:a.compare(11,9,10,3);break;
            }}else if(d.op==9)a.immediate(11,9,d.imm);
            else if(d.op==15)a.mov(11,uint32_t(d.u)<<16);
            else{a.mov(12,uint32_t(d.op==10||d.op==11?d.imm:d.u));if(d.op==10||d.op==11)a.compare(11,9,12,d.op==10?11:3);else a.binary(d.op==12?0x0a000000:d.op==13?0x2a000000:0x4a000000,11,9,12);}
            if(dest)a.store(11,dest*4);
        }
    };
    for(size_t index=0;index<block.code.size();index++){
        const Decoded& d=block.code[index];int needed=d.branch()?2:1;
        if(!d.branch()&&!d.compilable())return false;
        // A longer block can execute a prefix without crossing a device event or run budget.
        a.emit(0x71000000|(needed<<10)|(4<<5)|4);guard(0x54000003);
        if(!d.branch()){instruction(d);count++;continue;}
        if(index+1>=block.code.size()||!block.code[index+1].compilable()||block.code[index+1].memory()||block.code[index+1].gte())return false;
        a.load(9,d.s*4);a.load(10,d.t*4);a.commit(pending);pending=0;
        if(d.op==0){a.binary(0x2a000000,16,31,9);a.mov(17,1);}
        else if(d.op==2||d.op==3){a.mov(16,((d.at+4)&0xf0000000)|((d.word&0x3ffffff)<<2));a.mov(17,1);}
        else{a.mov(16,d.at+4+uint32_t(d.imm*4));if(d.op==4||d.op==5)a.compare(17,9,10,d.op==4?0:1);else a.compare(17,9,31,d.op==6?13:d.op==7?12:(d.t&1)?10:11);}
        int link=d.op==3?31:d.op==0&&d.fun==9?d.d:d.op==1&&(d.t==16||d.t==17)?31:0;
        if(link){a.mov(11,d.at+8);a.store(11,link*4);}
        a.mov(11,d.at+8);a.binary(0x6b000000,31,17,31);a.emit(0x1a800000|(11<<16)|(1<<12)|(16<<5)|12);
        a.store(12,offsetof(CpuState,faultBranchTarget));a.store(17,offsetof(CpuState,faultBranchTaken));
        instruction(block.code[index+1]);count+=2;
        a.load(12,offsetof(CpuState,faultBranchTarget));a.store(12,offsetof(CpuState,pc));a.immediate(12,12,4);a.store(12,offsetof(CpuState,next));
        a.store(31,offsetof(CpuState,loadReg));a.store(31,offsetof(CpuState,branchPending));a.store(31,offsetof(CpuState,branchTaken));
        a.mov(0,count);a.emit(0xd65f03c0);controlFinished=true;break;
    }
    if(!controlFinished)a.finish(count,pending);
    for(const Exit& e:exits){a.patch(e.branch,a.words.size(),true);a.finish(e.count,e.pending);}
    size_t page=size_t(sysconf(_SC_PAGESIZE)),bytes=(a.words.size()*4+15)&~size_t(15);
    // Only this core thread executes its generated code. Compilation happens
    // after generated functions return, so a page can safely switch W -> X.
    static thread_local std::shared_ptr<CpuCodePage> current;
    if(!current||current->bytes-current->used<bytes)current=std::make_shared<CpuCodePage>(std::max(size_t(16384),(bytes+page-1)&~(page-1)));
    if(current->memory==MAP_FAILED||mprotect(current->memory,current->bytes,PROT_READ|PROT_WRITE)!=0)return false;
    current->executable=false;
    void* memory=static_cast<uint8_t*>(current->memory)+current->used;
    memcpy(memory,a.words.data(),a.words.size()*4);__builtin___clear_cache(static_cast<char*>(memory),static_cast<char*>(memory)+a.words.size()*4);
    if(mprotect(current->memory,current->bytes,PROT_READ|PROT_EXEC)!=0)return false;
    current->executable=true;
    current->used+=bytes;block.codePage=current;block.executable=memory;block.executableBytes=bytes;block.function=reinterpret_cast<CpuBlock::Function>(memory);return true;
#else
    (void)block;return false;
#endif
}
