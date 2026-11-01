// Original native implementation of PSX Lab's SPU. Timing/arithmetic mirror our Java engine.
#include <jni.h>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <new>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "spu_gaussian.h"
using i64=int64_t;
static int clamp16(int v){return std::clamp(v,-32768,32767);}
static int mul(int a,int b){return int((i64(a)*b)>>15);}
struct Envelope {
    int level=0,counter=0;
    void step(int shift,int rate,bool decreasing,bool exponential,bool negative,int frozen){
        if(frozen==31?shift==31:((shift<<2)|rate)==frozen)return;
        int delta=7-rate;if(decreasing!=negative)delta=~delta;delta=int(uint32_t(delta)<<std::max(0,11-shift));
        int increment=0x8000u>>std::max(0,shift-11);
        if(exponential&&!decreasing&&level>0x6000){if(shift<10)delta>>=2;else if(shift>=11)increment>>=2;else{delta>>=1;increment>>=1;}}
        else if(exponential&&decreasing)delta=mul(delta,level);
        counter+=std::max(1,increment);if(counter<0x8000)return;counter&=0x7fff;level+=delta;
        if(!decreasing)level=clamp16(level);else if(negative)level=std::clamp(level,-32768,0);else level=std::max(0,level);
    }
};
struct Volume {Envelope envelope;int level=0;void update(int setting){if(!(setting&0x8000)){level=int16_t(setting<<1);envelope.level=level;envelope.counter=0;}else{envelope.step((setting>>2)&31,setting&3,setting&0x2000,setting&0x4000,setting&0x1000,127);level=envelope.level;}}};
struct Voice {
    int samples[28]{},history[2]{},recent[4]{};Envelope envelope;Volume left,right;
    int address=0,index=0,phase=0,blockFlags=0,stage=3,delay=0;bool loaded=false,manualLoop=false;
    void start(int at){address=at&0x7fff0;index=phase=0;history[0]=history[1]=0;std::fill_n(recent,4,0);envelope.level=envelope.counter=0;stage=0;delay=4;loaded=manualLoop=false;}
    void release(){stage=3;envelope.counter=0;}
    int interpolate()const{int i=(phase>>4)&255;
#ifdef __aarch64__
        int weights[4]={GAUSSIAN[255-i],GAUSSIAN[511-i],GAUSSIAN[256+i],GAUSSIAN[i]};
        int32x4_t products=vshrq_n_s32(vmulq_s32(vld1q_s32(weights),vld1q_s32(recent)),15);
        return clamp16(vaddvq_s32(products));
#else
        return clamp16(mul(GAUSSIAN[255-i],recent[0])+mul(GAUSSIAN[511-i],recent[1])+mul(GAUSSIAN[256+i],recent[2])+mul(GAUSSIAN[i],recent[3]));
#endif
    }
};
struct ReverbFilter {
    int inputHistory[2][64]{},outputHistory[2][64]{},position=0;
    static constexpr int taps[20]={-1,2,-10,35,-103,266,-616,1332,-2960,10246,10246,-2960,1332,-616,266,-103,35,-10,2,-1};
    void input(int l,int r){inputHistory[0][position]=l;inputHistory[1][position]=r;}
    void output(int l,int r){outputHistory[0][position]=l;outputHistory[1][position]=r;}
    int filter(const int* h,int shift)const{i64 sum=i64(h[(position-19)&63])*16384;
#ifdef __aarch64__
        int64x2_t acc=vdupq_n_s64(0);
        for(int i=0;i<20;i+=4){int values[4]={h[(position-i*2)&63],h[(position-(i+1)*2)&63],h[(position-(i+2)*2)&63],h[(position-(i+3)*2)&63]};int32x4_t products=vmulq_s32(vld1q_s32(values),vld1q_s32(taps+i));acc=vaddq_s64(acc,vpaddlq_s32(products));}
        sum+=vaddvq_s64(acc);
#else
        for(int i=0;i<20;i++)sum+=i64(h[(position-i*2)&63])*taps[i];
#endif
        return clamp16(int(sum>>shift));
    }
    int down(int ch)const{return filter(inputHistory[ch],15);}int up(int ch)const{return filter(outputHistory[ch],14);}
    void advance(){position=(position+1)&63;}
};
struct SpuCore {
    // This prefix is read through a native-order Java LongBuffer, never copied per sample.
    i64 frames=0,nonzeroFrames=0,decodedBlocks=0,keyOns=0,manualLoopWrites=0,protectedLoopStarts=0,cdNonzeroFrames=0,cdMixedFrames=0;
    int registers[256]{};uint8_t ram[512*1024]{};Voice voices[24];Volume mainLeft,mainRight;ReverbFilter reverbFilter;
    int transferAddress=0,capture=0,ended=0,keyOn=0,keyOff=0,noise=0,noiseTimer=0,reverbAddress=0,reverbLeft=0,reverbRight=0;
    bool irq=false,raised=false;
    void checkIrq(int address){if(!irq && (registers[0xd5]&0x8040)==0x8040&&(address&0x7ffff)==registers[0xd2]*8){irq=raised=true;}}
    void checkLateIrq(){if(irq||(registers[0xd5]&0x8040)!=0x8040)return;checkIrq(transferAddress);for(const Voice& v:voices)if(v.loaded){checkIrq(v.address);checkIrq(v.address+8);}}
    int word(int address){address&=0x7fffe;checkIrq(address);return int16_t(ram[address]|(ram[address+1]<<8));}
    void word(int address,int value){address&=0x7fffe;checkIrq(address);ram[address]=uint8_t(value);ram[address+1]=uint8_t(uint32_t(value)>>8);}
    void writeTransfer(int value){word(transferAddress,value);transferAddress=(transferAddress+2)&0x7ffff;checkIrq(transferAddress);}
    int readTransfer(){int v=word(transferAddress)&65535;transferAddress=(transferAddress+2)&0x7ffff;checkIrq(transferAddress);return v;}
    int read(int address){int index=(address-0x1f801c00)/2;
        if(index>=256){int voice=(address-0x1f801e00)/4;return voice<24?((address&2)?voices[voice].right.level:voices[voice].left.level)&65535:65535;}
        if(address==0x1f801dae){int control=registers[0xd5],mode=(control>>4)&3;return(control&63)|(irq?64:0)|(mode>=2?128:0)|(mode==2?256:0)|(mode==3?512:0)|((registers[0xd6]&12)&&capture>=512?2048:0);}
        if(address==0x1f801d9c)return ended&65535;if(address==0x1f801d9e)return uint32_t(ended)>>16;
        if(address==0x1f801db8)return mainLeft.level&65535;if(address==0x1f801dba)return mainRight.level&65535;
        if(index<0xc0&&(index&7)==6)return voices[index/8].envelope.level&65535;return registers[index];
    }
    void write(int address,int value){value&=65535;int index=(address-0x1f801c00)/2;if(index>=256)return;
        if(address==0x1f801dae||address==0x1f801d9c||address==0x1f801d9e)return;registers[index]=value;
        if(index<0xc0){Voice& v=voices[index/8];if((index&7)==6)v.envelope.level=int16_t(value);if((index&7)==7){manualLoopWrites++;if(v.delay<=1)v.manualLoop=true;}return;}
        switch(address){case 0x1f801d88:keyOn|=value;break;case 0x1f801d8a:keyOn|=(value&255)<<16;break;case 0x1f801d8c:keyOff|=value;break;case 0x1f801d8e:keyOff|=(value&255)<<16;break;case 0x1f801da2:reverbAddress=value*8;break;case 0x1f801da4:checkLateIrq();break;case 0x1f801da6:transferAddress=value*8;checkIrq(transferAddress);break;case 0x1f801da8:writeTransfer(value);break;case 0x1f801daa:if(!(value&64))irq=false;else checkLateIrq();break;}
    }
    static void decodeBlock(const uint8_t* ram,int address,int* samples,int* history){static constexpr int f1[5]={0,60,115,98,122},f2[5]={0,0,-52,-55,-60};int header=ram[address&0x7ffff],shift=header&15,filter=header>>4;if(shift>12)shift=9;if(filter>4)filter=0;int first=history[0],second=history[1];
        for(int i=0;i<28;i++){int nibble=(ram[(address+2+i/2)&0x7ffff]>>((i&1)*4))&15;int sample=int16_t(nibble<<12)>>shift;sample=clamp16(sample+((first*f1[filter])>>6)+((second*f2[filter])>>6));samples[i]=sample;second=first;first=sample;}history[0]=first;history[1]=second;
    }
    void decode(int n){Voice& v=voices[n];int carried=v.index;checkIrq(v.address);checkIrq(v.address+8);decodeBlock(ram,v.address,v.samples,v.history);decodedBlocks++;v.blockFlags=ram[(v.address+1)&0x7ffff]&7;if(v.blockFlags&4){if(!v.manualLoop)registers[n*8+7]=v.address/8;else protectedLoopStarts++;}v.index=0;v.loaded=true;v.recent[3]=v.samples[0];for(int i=0;i<carried;i++)advance(n);}
    // Mark the next block unread until the following output frame. The game may
    // refill its samples/flags between frames; eager fetch also asserts IRQ early.
    void advance(int n){Voice& v=voices[n];if(!v.loaded){v.index++;return;}v.recent[0]=v.recent[1];v.recent[1]=v.recent[2];v.recent[2]=v.recent[3];v.index++;if(v.index>=28){if(v.blockFlags&1){ended|=1<<n;v.address=(registers[n*8+7]*8)&0x7fff0;if(!(v.blockFlags&2)){v.stage=3;v.envelope.level=0;}}else v.address=(v.address+16)&0x7ffff;v.index=0;v.loaded=false;}else v.recent[3]=v.samples[v.index];}
    void envelope(int n){Voice& v=voices[n];int a=registers[n*8+4],b=registers[n*8+5];switch(v.stage){
        case 0:v.envelope.step((a>>10)&31,(a>>8)&3,false,a&0x8000,false,127);if(v.envelope.level==32767){v.stage=1;v.envelope.counter=0;}break;
        case 1:v.envelope.step((a>>4)&15,0,true,true,false,31);if(v.envelope.level<=std::min(32767,((a&15)+1)*2048)){v.stage=2;v.envelope.counter=0;}break;
        case 2:v.envelope.step((b>>8)&31,(b>>6)&3,b&0x4000,b&0x8000,false,127);break;
        case 3:v.envelope.step(b&31,0,true,b&32,false,31);break;
    }}
    int revAddress(int offset)const{int base=registers[0xd1]*8,size=0x80000-base;int at=(reverbAddress-base+offset)%size;if(at<0)at+=size;return base+at;}
    int revRead(int offset){return word(revAddress(offset));}void revWrite(int offset,int value,bool enabled){if(enabled)word(revAddress(offset),clamp16(value));}
    int rv(int r)const{return int16_t(registers[0xe0+r]);}int ra(int r)const{return registers[0xe0+r]*8;}
    void reverb(int left,int right,bool enabled){int lin=mul(left,rv(30)),rin=mul(right,rv(31));for(int ch=0;ch<2;ch++){int input=ch==0?lin:rin;
        if(enabled){int old=revRead(ra(10+ch)-2),value=mul(input+mul(revRead(ra(16+ch)),rv(7))-old,rv(2))+old;revWrite(ra(10+ch),rv(2)==-32768?-value:value,true);old=revRead(ra(18+ch)-2);value=mul(input+mul(revRead(ra(24+(ch^1))),rv(7))-old,rv(2))+old;revWrite(ra(18+ch),rv(2)==-32768?-value:value,true);}
        int value=mul(revRead(ra(12+ch)),rv(3))+mul(revRead(ra(14+ch)),rv(4))+mul(revRead(ra(20+ch)),rv(5))+mul(revRead(ra(22+ch)),rv(6));
        for(int apf=0;apf<2;apf++){int address=ra(26+2*apf+ch),delayed=revRead(address-ra(apf));value=clamp16(value-mul(delayed,rv(8+apf)));revWrite(address,value,enabled);value=clamp16(mul(value,rv(8+apf))+delayed);}if(ch==0)reverbLeft=value;else reverbRight=value;
        }reverbAddress+=2;if(reverbAddress>=0x80000)reverbAddress=registers[0xd1]*8;
    }
    int flags(int n)const{return registers[n]|((registers[n+1]&255)<<16);}
    int sample(int cd){int control=registers[0xd5];noiseTimer-=4+((control>>8)&3);if(noiseTimer<0){noise=((noise<<1)|(((noise>>15)^(noise>>12)^(noise>>11)^(noise>>10)^1)&1))&65535;int reload=0x20000u>>((control>>10)&15);noiseTimer+=reload;if(noiseTimer<0)noiseTimer+=reload;}
        int left=0,right=0,echoLeft=0,echoRight=0,previous=0,modulation=flags(0xc8),noises=flags(0xca),echo=flags(0xcc);bool enabled=control&0x8000;
        for(int i=0;i<24;i++){Voice& v=voices[i];int r=i*8,bit=1<<i;if(keyOff&bit)v.release();if(keyOn&bit){v.start(registers[r+3]*8);ended&=~bit;keyOns++;}v.left.update(registers[r]);v.right.update(registers[r+1]);int out=0;
            if(enabled&&(!(v.stage==3&&v.envelope.level==0&&v.delay==0)||(control&64))){if(v.delay>0)v.delay--;else{if(!v.loaded)decode(i);int raw=(noises&bit)?int16_t(noise):v.interpolate();out=mul(raw,v.envelope.level);envelope(i);int pitch=registers[r+2];if(i>0&&(modulation&bit))pitch=((int16_t(pitch)*(previous+32768))>>15)&65535;v.phase+=std::min(0x4000,pitch);while(v.phase>=4096){v.phase-=4096;advance(i);}}}else if(!enabled){v.envelope.level=0;v.stage=3;v.delay=0;}
            previous=out;int l=mul(out,v.left.level),rr=mul(out,v.right.level);left+=l;right+=rr;if(echo&bit){echoLeft+=l;echoRight+=rr;}if((registers[0xd6]&12)&&(i==1||i==3))word((i==1?0x800:0xc00)+capture,out);
        }keyOn=keyOff=0;int cdLeft=int16_t(cd),cdRight=cd>>16;if(cd)cdNonzeroFrames++;if(!(control&0x4000))left=right=echoLeft=echoRight=0;
        if(control&1){int l=mul(cdLeft,int16_t(registers[0xd8])),r=mul(cdRight,int16_t(registers[0xd9]));if(l|r)cdMixedFrames++;left+=l;right+=r;if(control&4){echoLeft+=l;echoRight+=r;}}
        if(registers[0xd6]&12){word(capture,cdLeft);word(0x400+capture,cdRight);}capture=(capture+2)&1023;
        int wetLeft=0,wetRight=0;if(registers[0xd6]&12){reverbFilter.input(clamp16(echoLeft),clamp16(echoRight));bool half=(frames&1)==0;if(half)reverb(reverbFilter.down(0),reverbFilter.down(1),control&128);reverbFilter.output(half?reverbLeft:0,half?reverbRight:0);wetLeft=reverbFilter.up(0);wetRight=reverbFilter.up(1);reverbFilter.advance();}
        mainLeft.update(registers[0xc0]);mainRight.update(registers[0xc1]);left=mul(clamp16(left+mul(wetLeft,int16_t(registers[0xc2]))),mainLeft.level);right=mul(clamp16(right+mul(wetRight,int16_t(registers[0xc3]))),mainRight.level);
        int l=clamp16(left),r=clamp16(right);frames++;if(l||r)nonzeroFrames++;return(l&65535)|int32_t(uint32_t(r)<<16);
    }
};
static SpuCore* core(JNIEnv* env,jobject buffer){auto* p=static_cast<SpuCore*>(env->GetDirectBufferAddress(buffer));if(!p||env->GetDirectBufferCapacity(buffer)<jlong(sizeof(SpuCore))){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid native SPU memory");return nullptr;}return p;}
extern "C" JNIEXPORT jint JNICALL Java_dev_psxlab_core_NativeSpu_stateSize(JNIEnv*,jclass){return sizeof(SpuCore);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_NativeSpu_initialize(JNIEnv* env,jclass,jobject buffer){if(auto* p=core(env,buffer))new(p)SpuCore();}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_NativeSpu_operation(JNIEnv* env,jclass,jobject buffer,jint op,jint a,jint b){auto* p=core(env,buffer);if(!p)return 0;p->raised=false;int result=0;switch(op){case 0:result=p->read(a);break;case 1:p->write(a,b);break;case 2:result=p->readTransfer();break;case 3:p->writeTransfer(a);break;case 4:result=p->sample(a);break;default:env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid SPU operation");return 0;}return jlong(uint64_t(uint32_t(result))|(p->raised?(uint64_t(1)<<32):0));}
