package dev.psxlab.core;

/** Original functional SPU. 44.1 kHz, one stereo frame per 768 base CPU clocks.
 * Register/RAM bus stalls and reverb arithmetic are not cycle/bit accurate. */
public final class Spu {
    public static final int SAMPLE_RATE=44100;
    public interface AudioSink { void samples(short[] interleaved,int count); }
    private final int[] registers=new int[256];
    private final byte[] ram=new byte[512*1024];
    private final Voice[] voices=new Voice[24];
    private final Volume mainLeft=new Volume(),mainRight=new Volume();
    private final ReverbResampler reverbFilter=new ReverbResampler();
    private final Bus bus;
    private final NativeSpu nativeCore;
    private final short[] output=new short[512];
    private AudioSink sink;
    private java.util.function.Consumer<String> traceSink;
    public void setTraceSink(java.util.function.Consumer<String> sink){traceSink=sink;}
    private int outputCount,clock,transferAddress,capture,ended,keyOn,keyOff;
    private int noise,noiseTimer,reverbAddress,reverbLeft,reverbRight;
    private boolean irq;
    public long frames,nonzeroFrames,decodedBlocks,keyOns,manualLoopWrites,protectedLoopStarts,cdNonzeroFrames,cdMixedFrames;
    Spu(Bus bus){this(bus,true);}
    Spu(Bus bus,boolean nativeExecution){this.bus=bus;nativeCore=nativeExecution && NativeCore.AVAILABLE?new NativeSpu(bus):null;for(int i=0;i<24;i++)voices[i]=new Voice();}
    public boolean nativeExecution(){return nativeCore!=null;}
    byte[] snapshotNative(){return nativeCore==null?new byte[0]:nativeCore.snapshot();}
    void restoreNative(byte[] bytes){if(nativeCore==null){if(bytes.length!=0)throw new IllegalArgumentException("State requires native SPU");}else nativeCore.restore(bytes);outputCount=0;nativeCounters();}
    private void nativeCounters(){if(nativeCore==null)return;frames=nativeCore.counter(0);nonzeroFrames=nativeCore.counter(1);decodedBlocks=nativeCore.counter(2);keyOns=nativeCore.counter(3);manualLoopWrites=nativeCore.counter(4);protectedLoopStarts=nativeCore.counter(5);cdNonzeroFrames=nativeCore.counter(6);cdMixedFrames=nativeCore.counter(7);}
    public void setAudioSink(AudioSink sink){this.sink=sink;outputCount=0;}
    public void flushAudio(){nativeCounters();if(outputCount>0 && sink!=null)sink.samples(output,outputCount);outputCount=0;}
    public String diagnostic(){nativeCounters();return "SPU: "+frames+" frames, "+nonzeroFrames+" nonzero, "+decodedBlocks+" ADPCM blocks, "+keyOns+" key-ons, "+manualLoopWrites+" loop writes, "+protectedLoopStarts+" protected loop starts, "+cdMixedFrames+" CD mixed frames";}
    /** Read-only observation, called on the emulation thread only while diagnostic logging is on.
     * An active envelope is not proof of audible dialogue; identify actual PCM activity separately. */
    public java.util.Map<String,Object> loggingSnapshot(){
        nativeCounters();java.util.List<java.util.Map<String,Object>> states=new java.util.ArrayList<>(24);
        for(int v=0;v<24;v++){
            int base=0x1f801c00+v*16,envelope=read(base+12),left=read(0x1f801e00+v*4),right=read(0x1f801e02+v*4);
            states.add(java.util.Map.of("voice",v,"envelope",envelope,"volume_left",(int)(short)left,"volume_right",(int)(short)right,
                    "pitch",read(base+4),"start_address",read(base+6)*8,"repeat_address",read(base+14)*8,
                    "adsr",java.util.List.of(read(base+8),read(base+10))));
        }
        java.util.Map<String,Object> result=new java.util.LinkedHashMap<>();
        result.put("frames",frames);result.put("nonzero_output_frames",nonzeroFrames);result.put("cd_input_nonzero_frames",cdNonzeroFrames);result.put("cd_mixed_nonzero_frames",cdMixedFrames);
        result.put("control",read(0x1f801daa));result.put("status",read(0x1f801dae));result.put("irq_address",read(0x1f801da4)*8);
        result.put("main_volume",java.util.List.of((int)(short)read(0x1f801db8),(int)(short)read(0x1f801dba)));
        result.put("cd_volume",java.util.List.of((int)(short)read(0x1f801db0),(int)(short)read(0x1f801db2)));
        result.put("voices",states);result.put("interpretation","Counters are cumulative; use deltas between snapshots. Voice envelopes/volumes do not identify speech or music.");return result;
    }
    public int read(int address){
        if(nativeCore!=null)return nativeCore.op(0,address,0);
        int index=(address-0x1f801c00)/2;
        if(index>=256){int voice=(address-0x1f801e00)/4;return voice<24?(((address&2)==0?voices[voice].left.level:voices[voice].right.level)&65535):65535;}
        if(address==0x1f801dae){int control=registers[0xd5],mode=(control>>>4)&3;return (control&63)|(irq?64:0)|(mode>=2?128:0)|(mode==2?256:0)|(mode==3?512:0)|((registers[0xd6]&12)!=0 && capture>=512?2048:0);}
        if(address==0x1f801d9c)return ended&65535;
        if(address==0x1f801d9e)return ended>>>16;
        if(address==0x1f801db8)return mainLeft.level&65535;
        if(address==0x1f801dba)return mainRight.level&65535;
        if(index<0xc0 && (index&7)==6)return voices[index/8].envelope.level&65535;
        return registers[index];
    }
    public void write(int address,int value){
        if(traceSink!=null && address>=0x1f801d88 && address<=0x1f801d8e && (value&65535)!=0){
            boolean on=address<0x1f801d8c;int mask=(value&65535)<<((address&2)==0?0:16);StringBuilder event=new StringBuilder(on?"SPU_KEYON":"SPU_KEYOFF");event.append(String.format(java.util.Locale.ROOT," mask=%06X",mask&0xffffff));
            if(on)for(int v=0;v<24;v++)if((mask&(1<<v))!=0){int base=0x1f801c00+v*16;event.append(String.format(java.util.Locale.ROOT," v%d(start=%05X pitch=%04X adsr=%04X/%04X volume=%04X/%04X)",v,read(base+6)*8,read(base+4),read(base+8),read(base+10),read(base),read(base+2)));}
            traceSink.accept(event.toString());
        }
        if(traceSink!=null && (address==0x1f801d80 || address==0x1f801d82 || address==0x1f801da4 || address==0x1f801daa || address==0x1f801db0 || address==0x1f801db2) && read(address)!=(value&65535))
            traceSink.accept(String.format(java.util.Locale.ROOT,"SPU_REGISTER address=%08X previous=%04X value=%04X",address,read(address),value&65535));
        if(nativeCore!=null){nativeCore.op(1,address,value);manualLoopWrites=nativeCore.counter(4);return;}
        value&=65535;int index=(address-0x1f801c00)/2;
        if(index>=256)return;
        if(address==0x1f801dae || address==0x1f801d9c || address==0x1f801d9e)return;
        registers[index]=value;
        if(index<0xc0){Voice v=voices[index/8];if((index&7)==6)v.envelope.level=(short)value;
            if((index&7)==7){manualLoopWrites++;if(v.delay<=1)v.manualLoop=true;}return;}
        switch(address){
            case 0x1f801d88:keyOn|=value;break;
            case 0x1f801d8a:keyOn|=(value&255)<<16;break;
            case 0x1f801d8c:keyOff|=value;break;
            case 0x1f801d8e:keyOff|=(value&255)<<16;break;
            case 0x1f801da2:reverbAddress=value*8;break;
            case 0x1f801da4:checkLateIrq();break;
            case 0x1f801da6:transferAddress=value*8;checkIrq(transferAddress);break;
            case 0x1f801da8:writeTransfer(value);break;
            case 0x1f801daa:if((value&64)==0)irq=false;else checkLateIrq();break;
        }
    }
    private void checkIrq(int address){if(!irq && (registers[0xd5]&0x8040)==0x8040 && (address&0x7ffff)==registers[0xd2]*8){irq=true;bus.raiseInterrupt(9);}}
    private void checkLateIrq(){
        if(irq || (registers[0xd5]&0x8040)!=0x8040)return;
        checkIrq(transferAddress);
        for(Voice voice:voices)if(voice.loaded){checkIrq(voice.address);checkIrq(voice.address+8);}
    }
    private int word(int address){address&=0x7fffe;checkIrq(address);return (short)((ram[address]&255)|(ram[address+1]&255)<<8);}
    private void word(int address,int value){address&=0x7fffe;checkIrq(address);ram[address]=(byte)value;ram[address+1]=(byte)(value>>>8);}
    public void writeTransfer(int value){if(nativeCore!=null){nativeCore.op(3,value,0);return;}word(transferAddress,value);transferAddress=(transferAddress+2)&0x7ffff;checkIrq(transferAddress);}
    public int readTransfer(){if(nativeCore!=null)return nativeCore.op(2,0,0);int value=word(transferAddress)&65535;transferAddress=(transferAddress+2)&0x7ffff;checkIrq(transferAddress);return value;}
    public void tick(int cycles){clock+=cycles;while(clock>=768){clock-=768;int cd=bus.cdrom.audioFrame();if(nativeCore!=null)sampleNative(cd);else sample(cd);}}
    int clocksToEvent(){return 768-clock;}
    private int sampleNative(int cd){int pcm=nativeCore.op(4,cd,0);frames++;if(pcm!=0)nonzeroFrames++;if(cd!=0)cdNonzeroFrames++;if(sink!=null){output[outputCount++]=(short)pcm;output[outputCount++]=(short)(pcm>>16);if(outputCount==output.length)flushAudio();}return pcm;}
    static int clamp(int value){return Math.max(-32768,Math.min(32767,value));}
    private static int mul(int a,int b){return (int)(((long)a*b)>>15);}
    private int flags(int index){return registers[index]|(registers[index+1]&255)<<16;}
    private int sample(int cd){
        int control=registers[0xd5];
        noiseTimer-=4+((control>>>8)&3);
        if(noiseTimer<0){noise=((noise<<1)|(((noise>>>15)^(noise>>>12)^(noise>>>11)^(noise>>>10)^1)&1))&65535;int reload=0x20000>>>((control>>>10)&15);noiseTimer+=reload;if(noiseTimer<0)noiseTimer+=reload;}
        int left=0,right=0,echoLeft=0,echoRight=0,previous=0;
        int modulation=flags(0xc8),noises=flags(0xca),echo=flags(0xcc);
        boolean enabled=(control&0x8000)!=0;
        for(int i=0;i<24;i++){
            Voice v=voices[i];int r=i*8,bit=1<<i;
            if((keyOff&bit)!=0)v.release();
            if((keyOn&bit)!=0){v.start(registers[r+3]*8);ended&=~bit;keyOns++;}
            v.left.update(registers[r]);v.right.update(registers[r+1]);
            int out=0;
            if(enabled && (!(v.stage==3 && v.envelope.level==0 && v.delay==0) || (control&64)!=0)){
                if(v.delay>0){v.delay--;}else{
                if(!v.loaded)v.decode(i);
                int raw=(noises&bit)!=0?(short)noise:v.interpolate();
                out=mul(raw,v.envelope.level);
                v.envelope(i);
                int pitch=registers[r+2];
                if(i>0 && (modulation&bit)!=0)pitch=(((short)pitch*(previous+32768))>>15)&65535;
                v.phase+=Math.min(0x4000,pitch);
                while(v.phase>=4096){v.phase-=4096;v.advance(i);}
                }
            }else if(!enabled){v.envelope.level=0;v.stage=3;v.delay=0;}
            previous=out;
            int l=mul(out,v.left.level),rr=mul(out,v.right.level);
            left+=l;right+=rr;if((echo&bit)!=0){echoLeft+=l;echoRight+=rr;}
            if((registers[0xd6]&12)!=0 && (i==1 || i==3))word((i==1?0x800:0xc00)+capture,out);
        }
        keyOn=keyOff=0;
        int cdLeft=(short)cd,cdRight=cd>>16;if(cd!=0)cdNonzeroFrames++;
        if((control&0x4000)==0){left=right=echoLeft=echoRight=0;}
        if((control&1)!=0){
            int l=mul(cdLeft,(short)registers[0xd8]),r=mul(cdRight,(short)registers[0xd9]);
            if((l|r)!=0)cdMixedFrames++;left+=l;right+=r;if((control&4)!=0){echoLeft+=l;echoRight+=r;}
        }
        if((registers[0xd6]&12)!=0){word(capture,cdLeft);word(0x400+capture,cdRight);}
        capture=(capture+2)&1023;
        int wetLeft=0,wetRight=0;
        if((registers[0xd6]&12)!=0){
            reverbFilter.input(clamp(echoLeft),clamp(echoRight));
            boolean halfRate=(frames&1)==0;
            if(halfRate)reverb(reverbFilter.down(0),reverbFilter.down(1),(control&128)!=0);
            // Zero insertion and FIR reconstruction, rather than holding each wet sample twice.
            reverbFilter.output(halfRate?reverbLeft:0,halfRate?reverbRight:0);
            wetLeft=reverbFilter.up(0);wetRight=reverbFilter.up(1);reverbFilter.advance();
        }
        mainLeft.update(registers[0xc0]);mainRight.update(registers[0xc1]);
        left=mul(clamp(left+mul(wetLeft,(short)registers[0xc2])),mainLeft.level);right=mul(clamp(right+mul(wetRight,(short)registers[0xc3])),mainRight.level);
        short l=(short)clamp(left),r=(short)clamp(right);frames++;if(l!=0 || r!=0)nonzeroFrames++;
        if(sink!=null){output[outputCount++]=l;output[outputCount++]=r;if(outputCount==output.length)flushAudio();}
        return (l&65535)|(r<<16);
    }
    public static String verifyNative(){
        if(!NativeCore.AVAILABLE)throw new AssertionError("Native SPU unavailable");
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus ba=new Bus(ga),bb=new Bus(gb);Spu a=new Spu(ba,false),b=new Spu(bb,true);java.util.Random random=new java.util.Random(0x535055);
            ba.write32(0x1f801074,512);bb.write32(0x1f801074,512);
            for(Spu s:new Spu[]{a,b})s.write(0x1f801da6,0);
            for(int i=0;i<32768;i++){int word=random.nextInt();a.writeTransfer(word);b.writeTransfer(word);}
            for(int n=0;n<24;n++)for(int r=0;r<8;r++){int value=switch(r){case 0,1->0x3fff;case 2->512+random.nextInt(0x4000);case 3,7->random.nextInt(4096);case 4->0x00ff;case 5->0x5fc0;default->0;};a.write(0x1f801c00+n*16+r*2,value);b.write(0x1f801c00+n*16+r*2,value);}
            int[][] setup={{0x1f801d80,0x3fff},{0x1f801d82,0x3fff},{0x1f801daa,0xc0c5},{0x1f801dac,4},{0x1f801d84,0x3fff},{0x1f801d86,0x3fff},{0x1f801db0,0x7fff},{0x1f801db2,0x7fff},{0x1f801d88,65535},{0x1f801d8a,255}};
            for(int[] p:setup){a.write(p[0],p[1]);b.write(p[0],p[1]);}
            for(int i=0;i<24000;i++){
                if(i%23==0){int address=0x1f801c00+random.nextInt(256)*2,value=random.nextInt(65536);a.write(address,value);b.write(address,value);}
                ba.write32(0x1f801070,~512);bb.write32(0x1f801070,~512);
                int cd=random.nextInt(),expected=a.sample(cd),actual=b.sampleNative(cd);
                if(expected!=actual || ba.interruptPending()!=bb.interruptPending())throw new AssertionError("Native SPU PCM/IRQ differs at frame "+i+" expected="+Integer.toHexString(expected)+" actual="+Integer.toHexString(actual));
                if(i%64==0)for(int address=0x1f801c00;address<0x1f801e60;address+=2)if(a.read(address)!=b.read(address))throw new AssertionError("Native SPU register differs at "+Integer.toHexString(address)+" frame="+i);
            }
            a.write(0x1f801daa,0);b.write(0x1f801daa,0);a.write(0x1f801da6,0);b.write(0x1f801da6,0);
            for(int i=0;i<262144;i++)if(a.readTransfer()!=b.readTransfer())throw new AssertionError("Native SPU RAM differs at halfword "+i);
            b.nativeCounters();if(a.frames!=b.frames || a.decodedBlocks!=b.decodedBlocks || a.keyOns!=b.keyOns || a.cdMixedFrames!=b.cdMixedFrames || a.protectedLoopStarts!=b.protectedLoopStarts)throw new AssertionError("Native SPU counters differ");
            return "Native SPU: 24,000 stereo PCM frames, live register mutations, IRQs, full sound RAM and counters match the original Java engine; no AudioTrack playback.\n";
        }
    }
    private int revAddress(int offset){int base=registers[0xd1]*8,size=0x80000-base;return base+Math.floorMod(reverbAddress-base+offset,size);}
    private int revRead(int offset){return word(revAddress(offset));}
    private void revWrite(int offset,int value,boolean enabled){if(enabled)word(revAddress(offset),clamp(value));}
    private int rv(int reg){return (short)registers[0xe0+reg];}
    private int ra(int reg){return registers[0xe0+reg]*8;}
    private void reverb(int left,int right,boolean enabled){
        int lin=mul(left,rv(30)),rin=mul(right,rv(31));
        for(int ch=0;ch<2;ch++){
            int input=ch==0?lin:rin;
            if(enabled){
                int old=revRead(ra(10+ch)-2),value=mul(input+mul(revRead(ra(16+ch)),rv(7))-old,rv(2))+old;
                revWrite(ra(10+ch),rv(2)==-32768?-value:value,true);
                old=revRead(ra(18+ch)-2);value=mul(input+mul(revRead(ra(24+(ch^1))),rv(7))-old,rv(2))+old;
                revWrite(ra(18+ch),rv(2)==-32768?-value:value,true);
            }
            int value=mul(revRead(ra(12+ch)),rv(3))+mul(revRead(ra(14+ch)),rv(4))+mul(revRead(ra(20+ch)),rv(5))+mul(revRead(ra(22+ch)),rv(6));
            for(int apf=0;apf<2;apf++){
                int address=ra(26+2*apf+ch),delayed=revRead(address-ra(apf));
                value=clamp(value-mul(delayed,rv(8+apf)));revWrite(address,value,enabled);
                value=clamp(mul(value,rv(8+apf))+delayed);
            }
            if(ch==0)reverbLeft=value;else reverbRight=value;
        }
        reverbAddress+=2;if(reverbAddress>=0x80000)reverbAddress=registers[0xd1]*8;
    }
    private static final int[] FILTER1={0,60,115,98,122},FILTER2={0,0,-52,-55,-60};
    /** Decode one block with persistent two-sample prediction history. */
    static void decodeBlock(byte[] ram,int address,int[] samples,int[] history){
        int header=ram[address&0x7ffff]&255,shift=header&15,filter=header>>>4;
        if(shift>12)shift=9;if(filter>4)filter=0;
        int first=history[0],second=history[1];
        for(int i=0;i<28;i++){
            int nibble=(ram[(address+2+i/2)&0x7ffff]>>>((i&1)*4))&15;
            int sample=((short)(nibble<<12))>>shift;
            sample=clamp(sample+((first*FILTER1[filter])>>6)+((second*FILTER2[filter])>>6));
            samples[i]=sample;second=first;first=sample;
        }
        history[0]=first;history[1]=second;
    }
    private final class Voice {
        final int[] samples=new int[28],history=new int[2],recent=new int[4];
        final Envelope envelope=new Envelope();final Volume left=new Volume(),right=new Volume();
        int address,index,phase,blockFlags,stage=3,delay;boolean loaded,manualLoop;
        void start(int start){address=start&0x7fff0;index=phase=0;history[0]=history[1]=0;java.util.Arrays.fill(recent,0);envelope.level=envelope.counter=0;stage=0;delay=4;loaded=false;manualLoop=false;}
        void release(){stage=3;envelope.counter=0;}
        void decode(int voice){
            int carried=index;
            checkIrq(address);checkIrq(address+8);decodeBlock(ram,address,samples,history);decodedBlocks++;
            blockFlags=ram[(address+1)&0x7ffff]&7;if((blockFlags&4)!=0){if(!manualLoop)registers[voice*8+7]=address/8;else protectedLoopStarts++;}
            index=0;loaded=true;recent[3]=samples[0];
            // Pitch can cross the boundary by up to three more samples. Rebuild
            // interpolation history now, after the new block has actually been read.
            for(int i=0;i<carried;i++)advance(voice);
        }
        void advance(int voice){
            if(!loaded){index++;return;}
            recent[0]=recent[1];recent[1]=recent[2];recent[2]=recent[3];index++;
            if(index>=28){
                if((blockFlags&1)!=0){ended|=1<<voice;address=(registers[voice*8+7]*8)&0x7fff0;if((blockFlags&2)==0){stage=3;envelope.level=0;}}
                else address=(address+16)&0x7ffff;
                // Fetch on the next output frame, leaving the CPU/DMA refill
                // window open and raising the read IRQ at the read, not early.
                index=0;loaded=false;
            }else recent[3]=samples[index];
        }
        int interpolate(){int i=(phase>>>4)&255;int[] g=SpuGaussian.TABLE;return clamp(mul(g[255-i],recent[0])+mul(g[511-i],recent[1])+mul(g[256+i],recent[2])+mul(g[i],recent[3]));}
        void envelope(int voice){
            int a=registers[voice*8+4],b=registers[voice*8+5];
            switch(stage){
                case 0:envelope.step((a>>>10)&31,(a>>>8)&3,false,(a&0x8000)!=0,false,127);if(envelope.level==32767){stage=1;envelope.counter=0;}break;
                case 1:envelope.step((a>>>4)&15,0,true,true,false,31);if(envelope.level<=Math.min(32767,((a&15)+1)*2048)){stage=2;envelope.counter=0;}break;
                case 2:envelope.step((b>>>8)&31,(b>>>6)&3,(b&0x4000)!=0,(b&0x8000)!=0,false,127);break;
                case 3:envelope.step(b&31,0,true,(b&32)!=0,false,31);break;
            }
        }
    }
    private static final class Envelope {
        int level,counter;
        void step(int shift,int step,boolean decreasing,boolean exponential,boolean negative,int frozenRate){
            if(frozenRate==31?shift==31:((shift<<2)|step)==frozenRate)return;
            int delta=7-step;if(decreasing^negative)delta=~delta;delta<<=Math.max(0,11-shift);
            int increment=0x8000>>>Math.max(0,shift-11);
            if(exponential && !decreasing && level>0x6000){if(shift<10)delta>>=2;else if(shift>=11)increment>>=2;else{delta>>=1;increment>>=1;}}
            else if(exponential && decreasing)delta=mul(delta,level);
            counter+=Math.max(1,increment);if(counter<0x8000)return;counter&=0x7fff;
            level+=delta;
            if(!decreasing)level=clamp(level);else if(negative)level=Math.max(-32768,Math.min(0,level));else level=Math.max(0,level);
        }
    }
    private static final class Volume {
        final Envelope envelope=new Envelope();int level;
        void update(int setting){
            if((setting&0x8000)==0){level=(short)(setting<<1);envelope.level=level;envelope.counter=0;}
            else{envelope.step((setting>>>2)&31,setting&3,(setting&0x2000)!=0,(setting&0x4000)!=0,(setting&0x1000)!=0,127);level=envelope.level;}
        }
    }
}
