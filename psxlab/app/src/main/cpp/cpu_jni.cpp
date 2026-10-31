// Original PSX Lab native MIPS-I core and coherent block cache.
#include <jni.h>
#include "cpu_core.h"
#include "gte_core.h"
#include <stdexcept>
#include <limits>

namespace {
constexpr int Bios=0x200000,Scratch=0x280000,Generations=0x280400,CodeMask=0x281400,CdStatus=0x291400;
struct AddressFault {uint32_t address;bool store;};
struct Stop {};
struct CpuCore {
    JNIEnv* env=nullptr;jobject bus; jmethodID clockMethod,finishMethod,readMethod,writeMethod,biosWaitMethod,cdWaitMethod;
    CpuState* s;uint8_t* memory;uint64_t* generations;
    int elapsed=0,deadline=32,biosReadWaits=0,cdReadWaits=0;bool irq=false,recompile,frameBoundary=false,ramWaitStates;
    std::unordered_map<uint32_t,std::unique_ptr<CpuBlock>> blocks;
    CpuBlock* previous=nullptr;
    uint64_t cacheHits=0,compiled=0,jitInstructions=0,interpreted=0,invalidations=0,linked=0,bridges=0,compileFailures=0;
    uint32_t currentAt=0,currentWord=0;
    CpuCore(JNIEnv* e,jobject b,CpuState* state,uint8_t* mem,int options):bus(e->NewGlobalRef(b)),s(state),memory(mem),generations(reinterpret_cast<uint64_t*>(mem+Generations)),recompile((options&1)!=0),ramWaitStates((options&2)!=0){
        jclass cls=e->GetObjectClass(b);clockMethod=e->GetMethodID(cls,"nativeClock","(II)J");finishMethod=e->GetMethodID(cls,"nativeFinish","(II)J");readMethod=e->GetMethodID(cls,"nativeRead","(IIII)J");writeMethod=e->GetMethodID(cls,"nativeWrite","(IIIII)J");biosWaitMethod=e->GetMethodID(cls,"nativeBiosReadWaits","()I");cdWaitMethod=e->GetMethodID(cls,"nativeCdReadWaits","()I");biosReadWaits=e->CallIntMethod(b,biosWaitMethod);cdReadWaits=e->CallIntMethod(b,cdWaitMethod);e->DeleteLocalRef(cls);
    }
    void status(jlong packed){deadline=int((uint64_t(packed)>>32)&0x1fffffff);irq=(uint64_t(packed)>>62)&1;frameBoundary=(uint64_t(packed)>>61)&1;elapsed=0;bridges++;if(env->ExceptionCheck()){s->faultPc=currentAt;s->faultInstruction=currentWord;throw Stop{};}}
    jlong clockCall(jmethodID method){jvalue args[2]{};args[0].i=elapsed;args[1].i=int(s->cacheIsolated);return env->CallLongMethodA(bus,method,args);}
    void tick(){if(++elapsed>=deadline)status(clockCall(clockMethod));}
    void wait(int clocks){elapsed+=clocks;s->instructions+=clocks;if(elapsed>=deadline)status(clockCall(clockMethod));}
    int biosWait(int width)const{return (biosReadWaits>>(width==4?16:width==2?8:0))&255;}
    int fetchWait(uint32_t pc)const{return ramWaitStates && pc>=0xbfc00000 && pc<0xbfc80000?biosWait(4):0;}
    uint32_t read(uint32_t address,int width,bool instruction=false){
        if(width>1 && (address&uint32_t(width-1)))throw AddressFault{address,false};uint32_t a=address&0x1fffffff,offset;
        // Data RAM reads consume six bus clocks in addition to the instruction.
        // Cached instruction fetches and scratchpad accesses do not use this wait.
        if(a<0x800000){offset=a&0x1fffff;if(ramWaitStates&&!instruction){elapsed+=6;s->instructions+=6;if(elapsed>=deadline)status(clockCall(clockMethod));}}
        else if(a>=0x1fc00000&&a<0x1fc80000){offset=Bios+a-0x1fc00000;if(ramWaitStates&&!instruction)wait(biosWait(width));}
        else if(a>=0x1f800000&&a<0x1f800400)offset=Scratch+a-0x1f800000;
        else{
            if(ramWaitStates&&a>=0x1f801800&&a<0x1f801804){
                wait((cdReadWaits>>(width==4?16:width==2?8:0))&255);
                // Read-only status latches are refreshed at every device/MMIO
                // boundary. Result/data ports still take the synchronized path.
                if(width==1&&(a==0x1f801800||a==0x1f801803))return memory[CdStatus+(a&3)];
            }
            jvalue args[4]{};args[0].i=jint(address);args[1].i=width;args[2].i=elapsed;args[3].i=int(s->cacheIsolated);jlong result=env->CallLongMethodA(bus,readMethod,args);status(result);return uint32_t(result);
        }
        uint32_t value=0;memcpy(&value,memory+offset,size_t(width));return value;
    }
    void write(uint32_t address,uint32_t value,int width){
        if(width>1 && (address&uint32_t(width-1)))throw AddressFault{address,true};uint32_t a=address&0x1fffffff;
        if(a<0x800000){if(!s->cacheIsolated){uint32_t offset=a&0x1fffff;memcpy(memory+offset,&value,size_t(width));if(memory[CodeMask+(offset>>5)]&(1<<((offset>>2)&7)))generations[offset>>12]++;}return;}
        if(a>=0x1f800000&&a<0x1f800400){memcpy(memory+Scratch+a-0x1f800000,&value,size_t(width));return;}
        if(a>=0x1fc00000&&a<0x1fc80000)return;
        if(ramWaitStates&&width==1&&a==0x1f801800&&(value&3)==(memory[CdStatus]&3))return;
        jvalue args[5]{};args[0].i=jint(address);args[1].i=width;args[2].i=jint(value);args[3].i=elapsed;args[4].i=int(s->cacheIsolated);status(env->CallLongMethodA(bus,writeMethod,args));
        if(a>=0x1f801000&&a<=0x1f801023){biosReadWaits=env->CallIntMethod(bus,biosWaitMethod);cdReadWaits=env->CallIntMethod(bus,cdWaitMethod);}
    }
    GteCore geometry(){return GteCore{s->gte,s->gte+32,0,false};}
    int gteRead(int r){int32_t* d=s->gte;switch(r){case 1:case 3:case 5:case 8:case 9:case 10:case 11:return int16_t(d[r]);case 7:case 16:case 17:case 18:case 19:return d[r]&65535;case 15:return d[14];case 28:case 29:{int v=0;for(int i=0;i<3;i++)v|=std::max(0,std::min(31,int(int16_t(d[9+i]))>>7))<<(i*5);return v;}default:return d[r];}}
    void gteWrite(int r,int v){int32_t* d=s->gte;switch(r){case 1:case 3:case 5:case 8:case 9:case 10:case 11:d[r]=int16_t(v);break;case 7:case 16:case 17:case 18:case 19:d[r]=v&65535;break;case 15:d[12]=d[13];d[13]=d[14];d[14]=v;break;case 28:d[9]=(v&31)<<7;d[10]=((uint32_t(v)>>5)&31)<<7;d[11]=((uint32_t(v)>>10)&31)<<7;break;case 29:case 31:break;case 30:d[30]=v;d[31]=v==0||v==-1?32:__builtin_clz(v<0?uint32_t(~v):uint32_t(v));break;default:d[r]=v;}}
    void gteControl(int r,int v){switch(r){case 4:case 12:case 20:case 26:case 27:case 29:case 30:v=int16_t(v);break;case 31:v&=0x7ffff000;if(v&0x7f87e000)v|=int32_t(0x80000000u);break;default:break;}s->gte[32+r]=v;}
    void gteExecute(uint32_t word){auto g=geometry();if(!g.execute(int(word))){env->ThrowNew(env->FindClass("java/lang/UnsupportedOperationException"),"Unsupported native CPU GTE command");throw Stop{};}}
    void commit(){if(s->loadReg)s->r[s->loadReg]=s->loadValue;s->loadReg=0;}
    void load(int r,int v){s->loadReg=r;s->loadValue=v;}
    void branch(bool taken,uint32_t target){s->branchPending=1;s->branchTaken=taken;if(taken)s->next=target;}
    void trap(int code,uint32_t at,bool delay,uint32_t bad=0){
        if(code==4||code==5)s->cop0[8]=int(bad);
        s->cop0[13]=(s->cop0[13]&0xff00)|(code<<2)|(delay?int32_t(0x80000000u):0)|(delay&&s->faultBranchTaken?0x40000000:0);
        if(delay&&s->faultBranchTaken)s->cop0[6]=int(s->faultBranchTarget);
        s->cop0[14]=int(delay?at-4:at);s->cop0[12]=(s->cop0[12]&~63)|((uint32_t(s->cop0[12])<<2)&63);
        s->pc=(s->cop0[12]&0x400000)?0xbfc00180:0x80000080;s->next=s->pc+4;s->branchPending=s->branchTaken=0;s->lastException=code;s->exceptions++;s->r[0]=0;
    }
    int add(int a,int b,bool subtract=false){int64_t value=subtract?int64_t(a)-b:int64_t(a)+b;if(value>INT32_MAX||value<INT32_MIN)throw 12;return int(value);}
    void step(const Decoded* cached=nullptr){
        currentAt=s->pc;currentWord=0;tick();uint32_t at=s->pc;bool delay=s->branchPending;
        s->faultBranchTaken=s->branchTaken;s->faultBranchTarget=s->next;
        if(irq)s->cop0[13]|=0x400;else s->cop0[13]&=~0x400;
        if((s->cop0[12]&1)&&(s->cop0[12]&s->cop0[13]&0xff00)){
            commit();if(s->cop0[12]&0x40000000){try{uint32_t w=read(at,4,true);if((w&0xfe000000)==0x4a000000)gteExecute(w);}catch(AddressFault&){} }
            trap(0,at,delay);s->instructions++;interpreted++;return;
        }
        uint32_t word;
        // Decoded/JIT host caches do not turn KSEG1 ROM into guest cached memory.
        wait(fetchWait(at));
        try{word=cached?cached->word:read(at,4,true);}catch(AddressFault& e){commit();trap(4,at,delay,e.address);s->instructions++;interpreted++;return;}
        currentWord=word;Decoded d=cached?*cached:Decoded(word,at);int a=s->r[d.s],b=s->r[d.t];uint32_t merge=s->loadReg==uint32_t(d.t)&&d.t?s->loadValue:b;
        commit();s->pc=s->next;s->next+=4;s->branchPending=s->branchTaken=0;
        try{
            switch(d.op){
                case 0:switch(d.fun){
                    case 0:s->r[d.d]=uint32_t(b)<<d.shift;break;case 2:s->r[d.d]=uint32_t(b)>>d.shift;break;case 3:s->r[d.d]=b>>d.shift;break;
                    case 4:s->r[d.d]=uint32_t(b)<<(a&31);break;case 6:s->r[d.d]=uint32_t(b)>>(a&31);break;case 7:s->r[d.d]=b>>(a&31);break;
                    case 8:branch(true,uint32_t(a));break;case 9:s->r[d.d]=int(at+8);branch(true,uint32_t(a));break;
                    case 12:trap(8,at,delay);break;case 13:trap(9,at,delay);break;
                    case 16:s->r[d.d]=s->hi;break;case 17:s->hi=a;break;case 18:s->r[d.d]=s->lo;break;case 19:s->lo=a;break;
                    case 24:{int64_t p=int64_t(a)*b;s->lo=int(p);s->hi=int(p>>32);break;}
                    case 25:{uint64_t p=uint64_t(uint32_t(a))*uint32_t(b);s->lo=int(p);s->hi=int(p>>32);break;}
                    case 26:if(!b){s->lo=a>=0?-1:1;s->hi=a;}else if(a==INT32_MIN&&b==-1){s->lo=INT32_MIN;s->hi=0;}else{s->lo=a/b;s->hi=a%b;}break;
                    case 27:if(!b){s->lo=-1;s->hi=a;}else{s->lo=int(uint32_t(a)/uint32_t(b));s->hi=int(uint32_t(a)%uint32_t(b));}break;
                    case 32:s->r[d.d]=add(a,b);break;case 33:s->r[d.d]=uint32_t(a)+uint32_t(b);break;case 34:s->r[d.d]=add(a,b,true);break;case 35:s->r[d.d]=uint32_t(a)-uint32_t(b);break;
                    case 36:s->r[d.d]=a&b;break;case 37:s->r[d.d]=a|b;break;case 38:s->r[d.d]=a^b;break;case 39:s->r[d.d]=~(a|b);break;case 42:s->r[d.d]=a<b;break;case 43:s->r[d.d]=uint32_t(a)<uint32_t(b);break;
                    default:trap(10,at,delay);break;
                }break;
                case 1:if(d.t==16||d.t==17)s->r[31]=int(at+8);branch((d.t&1)?a>=0:a<0,at+4+uint32_t(d.imm*4));break;
                case 2:case 3:if(d.op==3)s->r[31]=int(at+8);branch(true,((at+4)&0xf0000000)|((word&0x3ffffff)<<2));break;
                case 4:branch(a==b,at+4+uint32_t(d.imm*4));break;case 5:branch(a!=b,at+4+uint32_t(d.imm*4));break;case 6:branch(a<=0,at+4+uint32_t(d.imm*4));break;case 7:branch(a>0,at+4+uint32_t(d.imm*4));break;
                case 8:s->r[d.t]=add(a,d.imm);break;case 9:s->r[d.t]=uint32_t(a)+uint32_t(d.imm);break;case 10:s->r[d.t]=a<d.imm;break;case 11:s->r[d.t]=uint32_t(a)<uint32_t(d.imm);break;
                case 12:s->r[d.t]=a&d.u;break;case 13:s->r[d.t]=a|d.u;break;case 14:s->r[d.t]=a^d.u;break;case 15:s->r[d.t]=uint32_t(d.u)<<16;break;
                case 16:
                    if((s->cop0[12]&0x10000002)==2){trap(11,at,delay);break;}
                    if(d.s==0){if(d.d==0||d.d==1||d.d==2||d.d==4||d.d==10)trap(10,at,delay);else load(d.t,s->cop0[d.d]);}
                    else if(d.s==4){if(d.d==13)s->cop0[13]=(s->cop0[13]&~0x300)|(b&0x300);else if(d.d!=8&&d.d!=14&&d.d!=15)s->cop0[d.d]=b;s->cacheIsolated=(s->cop0[12]&0x10000)!=0;}
                    else if(d.s==16&&d.fun==16)s->cop0[12]=(s->cop0[12]&~15)|((uint32_t(s->cop0[12])>>2)&15);else trap(10,at,delay);break;
                case 17:case 19:trap(11,at,delay);s->cop0[13]|=(d.op&3)<<28;break;
                case 18:
                    if(!(s->cop0[12]&0x40000000)){trap(11,at,delay);s->cop0[13]|=2<<28;break;}
                    if(d.s>=16)gteExecute(word);else switch(d.s){case 0:load(d.t,gteRead(d.d));break;case 2:load(d.t,s->gte[32+d.d]);break;case 4:gteWrite(d.d,b);break;case 6:gteControl(d.d,b);break;case 8:branch((d.t&1)==0,at+4+uint32_t(d.imm*4));break;default:trap(10,at,delay);break;}break;
                case 50:case 58:
                    if(!(s->cop0[12]&0x40000000)){trap(11,at,delay);s->cop0[13]|=2<<28;break;}
                    if(d.op==50)gteWrite(d.t,int(read(uint32_t(a)+d.imm,4)));else write(uint32_t(a)+d.imm,uint32_t(gteRead(d.t)),4);break;
                case 32:load(d.t,int8_t(read(uint32_t(a)+d.imm,1)));break;case 33:load(d.t,int16_t(read(uint32_t(a)+d.imm,2)));break;
                case 34:{uint32_t address=uint32_t(a)+d.imm;load(d.t,int((merge&(0xffffffu>>((address&3)*8)))|(read(address&~3u,4)<<((3-(address&3))*8))));break;}
                case 35:load(d.t,int(read(uint32_t(a)+d.imm,4)));break;case 36:load(d.t,int(read(uint32_t(a)+d.imm,1)));break;case 37:load(d.t,int(read(uint32_t(a)+d.imm,2)));break;
                case 38:{uint32_t address=uint32_t(a)+d.imm;load(d.t,int((merge&(0xffffff00u<<((3-(address&3))*8)))|(read(address&~3u,4)>>((address&3)*8))));break;}
                case 40:write(uint32_t(a)+d.imm,uint32_t(b),1);break;case 41:write(uint32_t(a)+d.imm,uint32_t(b),2);break;
                case 42:{uint32_t address=uint32_t(a)+d.imm;for(unsigned i=0;i<=(address&3);i++)write((address&~3u)+i,uint32_t(b)>>((3-(address&3)+i)*8),1);break;}
                case 43:write(uint32_t(a)+d.imm,uint32_t(b),4);break;
                case 46:{uint32_t address=uint32_t(a)+d.imm;for(unsigned i=address&3;i<4;i++)write((address&~3u)+i,uint32_t(b)>>((i-(address&3))*8),1);break;}
                default:trap(10,at,delay);break;
            }
        }catch(int code){trap(code,at,delay);}catch(AddressFault& fault){trap(fault.store?5:4,at,delay,fault.address);}
        s->r[0]=0;s->instructions++;interpreted++;
    }
    bool valid(CpuBlock* block)const{return block && block->pc==s->pc && (block->page<0||block->version==generations[block->page]);}
    CpuBlock* findBlock(){
        if(previous)for(CpuBlock* link:previous->links)if(valid(link)){linked++;return link;}
        auto it=blocks.find(s->pc);CpuBlock* block=it==blocks.end()?nullptr:it->second.get();
        if(block&&!valid(block)){invalidations++;block->function=nullptr;block->codePage.reset();block->executable=nullptr;block->code.clear();}
        if(block&&!block->code.empty()){cacheHits++;}
        else{
            if(CpuCodePage::allocatedBytes.load(std::memory_order_relaxed)>32*1024*1024){blocks.clear();previous=nullptr;block=nullptr;memset(memory+CodeMask,0,65536);}
            uint32_t p=s->pc&0x1fffffff;if((s->pc&3)||!(p<0x800000 || (p>=0x1fc00000&&p<0x1fc80000)))return nullptr;
            if(!block){if(blocks.size()>=65536){blocks.clear();previous=nullptr;memset(memory+CodeMask,0,65536);}auto owner=std::make_unique<CpuBlock>(s->pc);block=owner.get();blocks.emplace(s->pc,std::move(owner));}
            block->page=p<0x800000?int((p&0x1fffff)>>12):-1;block->version=block->page<0?0:generations[block->page];
            for(int i=0;i<32;i++){
                uint32_t at=s->pc+uint32_t(i*4),physical=at&0x1fffffff;if((physical>>12)!=(p>>12))break;
                uint32_t word=0;uint32_t offset=p<0x800000?(physical&0x1fffff):Bios+physical-0x1fc00000;memcpy(&word,memory+offset,4);Decoded d(word,at);
                if(p<0x800000)memory[CodeMask+(offset>>5)]|=uint8_t(1<<((offset>>2)&7));
                if(!d.compilable()){
                    if(d.branch()&&i<31&&((physical+4)>>12)==(p>>12)){
                        uint32_t slot;memcpy(&slot,memory+offset+4,4);
                        if(slot==0){if(p<0x800000)memory[CodeMask+((offset+4)>>5)]|=uint8_t(1<<(((offset+4)>>2)&7));block->code.push_back(d);block->code.emplace_back(0,at+4);break;}
                    }
                    if(block->code.empty())block->code.push_back(d);break;
                }
                block->code.push_back(d);if(d.store())break;
            }
            // Generated memory operations only complete for RAM. Prefix costs
            // exclude an instruction which exits to the interpreter for MMIO,
            // scratchpad or an address fault.
            block->ramReadClocks.assign(1,0);for(const Decoded& d:block->code)block->ramReadClocks.push_back(block->ramReadClocks.back()+((d.memory()&&!d.store()&&d.compilable())?6:0));
            if(recompile&&(block->code.front().compilable()||(block->code.size()==2&&block->code.front().branch()))){if(compileArm64(*block))compiled++;else compileFailures++;}
        }
        if(previous){if(!previous->links[0]||previous->links[0]->pc==block->pc)previous->links[0]=block;else previous->links[1]=block;}
        return block;
    }
    int run(JNIEnv* e,int budget,bool stopFrame){
        env=e;currentAt=s->pc;currentWord=0;uint64_t start=s->instructions;
        try{
            status(env->CallLongMethod(bus,clockMethod,0,int(s->cacheIsolated)));
            while(s->instructions-start<uint64_t(budget)){
                if(stopFrame&&frameBoundary)break;
                CpuBlock* block=findBlock();int remaining=budget-int(s->instructions-start);
                int cause=(s->cop0[13]&~0x400)|(irq?0x400:0);
                bool pendingIrq=(s->cop0[12]&1)&&(s->cop0[12]&cause&0xff00);
                int instructionClocks=1+fetchWait(s->pc);
                int blockClocks=block?int(block->code.size())*instructionClocks+(ramWaitStates?block->ramReadClocks.back():0):0;
                if(block&&block->function&&block->codePage->executable&&!s->branchPending&&!s->cacheIsolated&&!pendingIrq&&remaining>=blockClocks&&deadline-elapsed>blockClocks){
                    if(irq)s->cop0[13]|=0x400;else s->cop0[13]&=~0x400;
                    int count=block->function(s,memory,generations,memory+CodeMask);int clocks=count*instructionClocks+(ramWaitStates?block->ramReadClocks[size_t(count)]:0);s->instructions+=clocks;elapsed+=clocks;jitInstructions+=count;
                    if(count<int(block->code.size()))step(&block->code[size_t(count)]);
                }else if(block){
                    for(const Decoded& d:block->code){if(s->pc!=d.at||s->instructions-start>=uint64_t(budget))break;
                        // A device callback can DMA new code immediately before this instruction.
                        if(elapsed+1>=deadline)step();else step(&d);
                        if((stopFrame&&frameBoundary)||d.store() || (block->page>=0&&block->version!=generations[block->page]))break;
                    }
                }else step();
                previous=block;
            }
            status(clockCall(finishMethod));
        }catch(Stop&){}
        return int(s->instructions-start);
    }
};
}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_NativeCpu_create(JNIEnv* env,jclass,jobject bus,jobject state,jobject memory,jint options){
    auto* s=static_cast<CpuState*>(env->GetDirectBufferAddress(state));auto* mem=static_cast<uint8_t*>(env->GetDirectBufferAddress(memory));
    if(!s||!mem||env->GetDirectBufferCapacity(state)<jlong(sizeof(CpuState))||env->GetDirectBufferCapacity(memory)<0x291410){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid native CPU shared buffers");return 0;}
    return reinterpret_cast<jlong>(new CpuCore(env,bus,s,mem,options));
}
extern "C" JNIEXPORT jint JNICALL Java_dev_psxlab_core_NativeCpu_runNative(JNIEnv* env,jclass,jlong handle,jint budget,jboolean stopFrame){return reinterpret_cast<CpuCore*>(handle)->run(env,budget,stopFrame);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_NativeCpu_destroy(JNIEnv* env,jclass,jlong handle){auto* core=reinterpret_cast<CpuCore*>(handle);if(core){env->DeleteGlobalRef(core->bus);delete core;}}
extern "C" JNIEXPORT jlongArray JNICALL Java_dev_psxlab_core_NativeCpu_statisticsNative(JNIEnv* env,jclass,jlong handle){auto* c=reinterpret_cast<CpuCore*>(handle);jlong values[]={jlong(c->compiled),jlong(c->jitInstructions),jlong(c->interpreted),jlong(c->cacheHits),jlong(c->linked),jlong(c->invalidations),jlong(c->bridges),jlong(c->compileFailures)};jlongArray result=env->NewLongArray(8);env->SetLongArrayRegion(result,0,8,values);return result;}
