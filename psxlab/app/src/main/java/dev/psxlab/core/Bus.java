package dev.psxlab.core;

public final class Bus {
    static final int BIOS_OFFSET=0x200000,SCRATCH_OFFSET=0x280000,GEN_OFFSET=0x280400,CODE_MASK_OFFSET=0x281400,CD_STATUS_OFFSET=0x291400,MEMORY_BYTES=0x291410;
    private java.nio.ByteBuffer nativeMemory;
    private boolean eventScheduling;
    private boolean deadlineDirty;
    private int clockDeadline=32;
    private long nativeFrame;
    void beginNativeFrame(){nativeFrame=gpu.vblanks;}
    public long deviceDispatches;
    java.nio.ByteBuffer enableNativeMemory(){
        if(nativeMemory==null){nativeMemory=java.nio.ByteBuffer.allocateDirect(MEMORY_BYTES).order(java.nio.ByteOrder.LITTLE_ENDIAN);nativeMemory.put(ram);nativeMemory.position(BIOS_OFFSET);nativeMemory.put(bios);nativeMemory.position(SCRATCH_OFFSET);nativeMemory.put(scratch);nativeMemory.clear();}
        return nativeMemory;
    }
    public byte[] snapshotRam(){if(nativeMemory==null)return ram.clone();byte[] copy=new byte[ram.length];nativeMemory.duplicate().get(copy);return copy;}
    void prepareSnapshot(){flushClocks();if(nativeMemory!=null){nativeMemory.duplicate().get(ram);java.nio.ByteBuffer bytes=nativeMemory.duplicate();bytes.position(SCRATCH_OFFSET);bytes.get(scratch);}}
    void restoreSnapshotMemory(){if(nativeMemory!=null){nativeMemory.duplicate().put(ram);java.nio.ByteBuffer bytes=nativeMemory.duplicate();bytes.position(SCRATCH_OFFSET);bytes.put(scratch);}invalidateInstructionFetch();updateDeadline();}
    private int memory8(byte[] data,int offset,int base){return nativeMemory==null?data[offset]&255:nativeMemory.get(base+offset)&255;}
    private int memory16(byte[] data,int offset,int base){return nativeMemory==null?half(data,offset):nativeMemory.getShort(base+offset)&65535;}
    private int memory32(byte[] data,int offset,int base){return nativeMemory==null?word(data,offset):nativeMemory.getInt(base+offset);}
    private void storeRam(int offset,int value,int bytes){
        instructionValid[offset>>>2]=false;
        if(nativeMemory==null){for(int i=0;i<bytes;i++)ram[offset+i]=(byte)(value>>>(i*8));}
        else{if(bytes==4)nativeMemory.putInt(offset,value);else if(bytes==2)nativeMemory.putShort(offset,(short)value);else nativeMemory.put(offset,(byte)value);if((nativeMemory.get(CODE_MASK_OFFSET+(offset>>>5))&(1<<((offset>>>2)&7)))!=0){int at=GEN_OFFSET+(offset>>>12)*8;nativeMemory.putLong(at,nativeMemory.getLong(at)+1);}}
    }
    // Physical RAM patches bypass guest cache isolation, retain code-cache invalidation,
    // and skip unchanged values. Unaligned 16-bit cheat writes cross words safely.
    void patchRam(int address,int value,int width){
        if((width!=1&&width!=2)||address<0||address>ram.length-width)throw new IllegalArgumentException("Invalid cheat RAM patch");
        if(width==2&&(address&1)==0){if(memory16(ram,address,0)!=(value&65535))storeRam(address,value,2);}
        else for(int i=0;i<width;i++){int next=(value>>>(i*8))&255;if(memory8(ram,address+i,0)!=next)storeRam(address+i,next,1);}
    }
    public final byte[] ram=new byte[2*1024*1024];
    private final int[] instructionWords=new int[512*1024];
    private final boolean[] instructionValid=new boolean[512*1024];
    public boolean instructionCaching=true;
    /** Call after any external direct RAM-array edits; bus stores invalidate automatically. */
    public void invalidateInstructionFetch(){java.util.Arrays.fill(instructionValid,false);if(nativeMemory!=null)for(int p=0;p<512;p++){int at=GEN_OFFSET+p*8;nativeMemory.putLong(at,nativeMemory.getLong(at)+1);}}
    public final byte[] bios=new byte[512*1024];
    private final byte[] scratch=new byte[1024];
    private final int[] memoryControl={0x1f000000,0x1f802000,0x0013243f,0x00003022,0x0013243f,0x200931e1,0x00020843,0x00070777,0x00031125};
    int nativeBiosReadWaits(){int device=memoryControl[4],common=memoryControl[8];return MemoryTiming.readWait(device,common,1)|(MemoryTiming.readWait(device,common,2)<<8)|(MemoryTiming.readWait(device,common,4)<<16);}
    int nativeCdReadWaits(){int device=memoryControl[6],common=memoryControl[8];return MemoryTiming.readWait(device,common,1)|(MemoryTiming.readWait(device,common,2)<<8)|(MemoryTiming.readWait(device,common,4)<<16);}
    private int ramSize=0xb88,cacheControl,irqStatus,irqMask;
    public boolean cacheIsolated;
    public int biosPostCode;
    public final Gpu gpu;
    public final Dma dma;
    public final Mdec mdec;
    public final Spu spu;
    public final Timers timers;
    public final CdRom cdrom;
    public final Sio0 sio0;
    public final boolean nativeCores;
    public Bus(Gpu gpu){this(gpu,new PadState(),true);}
    public Bus(Gpu gpu,PadState pad){this(gpu,pad,true);}
    public Bus(Gpu gpu,PadState pad,boolean nativeCores){this.nativeCores=nativeCores;this.gpu=gpu;dma=new Dma(this);mdec=new Mdec(this);spu=new Spu(this,nativeCores);timers=new Timers(this);cdrom=new CdRom(this);sio0=new Sio0(this,pad);}
    public static final class AddressError extends RuntimeException {
        public final int address;public final boolean store;
        AddressError(int address,boolean store){this.address=address;this.store=store;}
    }
    private int pendingClocks;
    /** Functional clock batching: MMIO flushes pending time; IRQ observation is at most 31 clocks late. */
    public void tick(){if(deadlineDirty)updateDeadline();if(++pendingClocks>=clockDeadline)flushClocks();}
    public void setEventScheduling(boolean enabled){flushClocks();eventScheduling=enabled;updateDeadline();}
    private void updateDeadline(){
        deadlineDirty=false;
        if(!eventScheduling){clockDeadline=32;return;}
        int next=Math.min(spu.clocksToEvent(),Math.min(gpu.clocksToEvent(timers),timers.clocksToEvent()));
        next=Math.min(next,Math.min(cdrom.clocksToEvent(),Math.min(sio0.clocksToEvent(),dma.clocksToEvent())));
        clockDeadline=Math.max(32,((next+31)/32)*32);
    }
    private long nativeStatus(int value){if(deadlineDirty)updateDeadline();if(nativeMemory!=null){nativeMemory.put(CD_STATUS_OFFSET,(byte)cdrom.statusRegister());nativeMemory.put(CD_STATUS_OFFSET+3,(byte)cdrom.interruptRegister());}return Integer.toUnsignedLong(value)|((long)Math.max(1,clockDeadline-pendingClocks)<<32)|(gpu.vblanks!=nativeFrame?1L<<61:0)|(interruptPending()?1L<<62:0)|(cacheIsolated?1L<<63:0);}
    long nativeClock(int elapsed,int isolated){cacheIsolated=isolated!=0;pendingClocks+=elapsed;if(pendingClocks>=clockDeadline)flushClocks();return nativeStatus(0);}
    long nativeFinish(int elapsed,int isolated){cacheIsolated=isolated!=0;pendingClocks+=elapsed;return nativeStatus(0);}
    long nativeRead(int address,int width,int elapsed,int isolated){cacheIsolated=isolated!=0;pendingClocks+=elapsed;
        // CD status/IRQ polling does not consume data or change device timing.
        // Observe it at the normal event boundary, without dispatching every
        // device again for each iteration of a guest busy-wait loop.
        int a=address&0x1fffffff;
        if(width==1 && (a==0x1f801800 || a==0x1f801803)){if(pendingClocks>=clockDeadline)flushClocks();return nativeStatus(cdrom.read(a&3));}
        int value=width==1?read8(address):width==2?read16(address):read32(address);return nativeStatus(value);}
    long nativeWrite(int address,int width,int value,int elapsed,int isolated){cacheIsolated=isolated!=0;pendingClocks+=elapsed;
        // Bank selection is a latch; it needs no device-clock synchronization.
        if(width==1 && (address&0x1fffffff)==0x1f801800){if(pendingClocks>=clockDeadline)flushClocks();cdrom.write(0,value);return nativeStatus(0);}
        if(width==1)write8(address,value);else if(width==2)write16(address,value);else write32(address,value);return nativeStatus(0);}
    public void flushClocks(){
        int clocks=pendingClocks;if(clocks==0)return;pendingClocks=0;deviceDispatches++;
        dma.tick(clocks);
        timers.cpuCycles(clocks);gpu.tickVideo(this,clocks);if(eventScheduling)cdrom.tickScheduled(clocks);else cdrom.tick(clocks);sio0.tick(clocks);spu.tick(clocks);
        updateDeadline();
    }
    private static int half(byte[] data,int at){return (data[at]&255)|(data[at+1]&255)<<8;}
    private static int word(byte[] data,int at){return half(data,at)|half(data,at+2)<<16;}
    public boolean interruptPending(){return (irqStatus&irqMask)!=0;}
    boolean interruptRaised(int bit){return (irqStatus&(1<<bit))!=0;}
    public void raiseInterrupt(int bit){irqStatus|=1<<bit;}
    public void loadBios(byte[] data){if(data.length!=bios.length)throw new IllegalArgumentException("BIOS must be exactly 512 KiB");System.arraycopy(data,0,bios,0,data.length);}
    public int read8(int address){
        int a=address&0x1fffffff;
        if(a<0x800000)return memory8(ram,a&0x1fffff,0);
        if(a>=0x1fc00000 && a<0x1fc80000)return memory8(bios,a-0x1fc00000,BIOS_OFFSET);
        if(a>=0x1f800000 && a<0x1f800400)return memory8(scratch,a-0x1f800000,SCRATCH_OFFSET);
        if(a>=0x1f000000 && a<0x1f800000)return 255;
        if(a>=0x1f802000 && a<0x1f804000)return 255;
        if(a>=0x1f801000 && a<0x1f802000){flushClocks();deadlineDirty=eventScheduling;}
        if(a>=0x1f801080 && a<0x1f801100)return dma.read(a&~3)>>>((a&3)*8)&255;
        if(a>=0x1f801c00 && a<0x1f802000)return spu.read(a&~1)>>>((a&1)*8)&255;
        if(a>=0x1f801100 && a<0x1f801140)return timers.read(a&~3)>>>((a&3)*8)&255;
        if(a>=0x1f801800 && a<0x1f801804)return cdrom.read(a&3);
        if(a>=0x1f801040 && a<0x1f801050)return sio0.read(a-0x1f801040,1);
        if(isControl(a,address))return controlRead(a,address)>>>((a&3)*8)&255;
        throw missing("Read",address);
    }
    private boolean isControl(int a,int address){return a>=0x1f801000 && a<=0x1f801023 || a>=0x1f801060 && a<=0x1f801063 || a>=0x1f801070 && a<=0x1f801077 || Integer.compareUnsigned(address,0xfffe0130)>=0 && Integer.compareUnsigned(address,0xfffe0133)<=0;}
    private int controlRead(int a,int address){
        if(Integer.compareUnsigned(address,0xfffe0130)>=0 && Integer.compareUnsigned(address,0xfffe0133)<=0)return cacheControl;
        if(a>=0x1f801000 && a<=0x1f801023)return memoryControl[(a-0x1f801000)/4];
        if((a&~3)==0x1f801060)return ramSize;
        return (a&~3)==0x1f801070?irqStatus:irqMask;
    }
    public int read16(int address){if((address&1)!=0)throw new AddressError(address,false);int a=address&0x1fffffff;if(a<0x800000)return memory16(ram,a&0x1fffff,0);if(a>=0x1fc00000 && a<0x1fc80000)return memory16(bios,a-0x1fc00000,BIOS_OFFSET);if(a>=0x1f801000 && a<0x1f802000){flushClocks();deadlineDirty=eventScheduling;}if(a>=0x1f801100 && a<0x1f801140)return (timers.read(a&~3)>>>((a&2)*8))&65535;if(a>=0x1f801c00 && a<0x1f802000)return spu.read(a);if(a>=0x1f801800 && a<0x1f801804)return cdrom.read(a&3)|cdrom.read(a&3)<<8;if(a>=0x1f801040 && a<0x1f801050)return sio0.read(a-0x1f801040,2);return read8(address)|read8(address+1)<<8;}
    /** Hot instruction-word lookup, coherent with RAM writes and physical aliases. */
    public int fetchInstruction(int address){
        int a=address&0x1fffffff;
        if(nativeMemory!=null && (a&3)==0 && a<0x800000)return nativeMemory.getInt(a&0x1fffff);
        if((a&3)==0 && a<0x800000){
            int offset=a&0x1fffff,index=offset>>>2;
            if(!instructionCaching)return word(ram,offset);
            if(instructionValid[index])return instructionWords[index];
            int value=word(ram,offset);instructionWords[index]=value;instructionValid[index]=true;return value;
        }
        return read32(address);
    }
    public int read32(int address){
        if((address&3)!=0)throw new AddressError(address,false);
        int a=address&0x1fffffff;
        if(a<0x800000)return memory32(ram,a&0x1fffff,0);
        if(a>=0x1fc00000 && a<0x1fc80000)return memory32(bios,a-0x1fc00000,BIOS_OFFSET);
        if(a>=0x1f801000 && a<0x1f802000){flushClocks();deadlineDirty=eventScheduling;}
        if(a>=0x1f801100 && a<0x1f801140)return timers.read(a);
        if(a>=0x1f801800 && a<0x1f801804)return cdrom.read(a&3)|cdrom.read(a&3)<<8|cdrom.read(a&3)<<16|cdrom.read(a&3)<<24;
        if(a>=0x1f801040 && a<0x1f801050)return sio0.read(a-0x1f801040,4);
        if(a==0x1f801810)return gpu.read();if(a==0x1f801814)return gpu.status();
        if(a==0x1f801820)return mdec.read();if(a==0x1f801824)return mdec.status();
        return read8(address)|read8(address+1)<<8|read8(address+2)<<16|read8(address+3)<<24;
    }
    public void write8(int address,int value){
        int a=address&0x1fffffff;
        if(a<0x800000){if(!cacheIsolated){storeRam(a&0x1fffff,value,1);}}
        else if(a>=0x1f800000 && a<0x1f800400){if(nativeMemory==null)scratch[a-0x1f800000]=(byte)value;else nativeMemory.put(SCRATCH_OFFSET+a-0x1f800000,(byte)value);}
        else if(a>=0x1f801000 && a<0x1f802000){flushClocks();writeMmioByte(a,value);deadlineDirty=eventScheduling;}
        else if(a>=0x1f802000 && a<0x1f804000){if(a==0x1f802041)biosPostCode=value&255;}
        else if(isControl(a,address)){int shift=(a&3)*8,mask=255<<shift;controlWrite(a&~3,address&~3,(controlRead(a,address)&~mask)|((value&255)<<shift),mask);}
        else if(a>=0x1fc00000 && a<0x1fc80000){}
        else throw missing("Write",address);
    }
    private void writeMmioByte(int a,int value){
        if(a>=0x1f801080 && a<0x1f801100)dma.write(a&~3,(value&255)<<((a&3)*8),255<<((a&3)*8));
        else if(a>=0x1f801c00 && a<0x1f802000){if((a&1)==0)spu.write(a,value);}
        else if(a>=0x1f801100 && a<0x1f801140){int shift=(a&3)*8;timers.write(a&~3,(timers.read(a&~3)&~(255<<shift))|((value&255)<<shift));}
        else if(a>=0x1f801800 && a<0x1f801804)cdrom.write(a&3,value);
        else if(a>=0x1f801040 && a<0x1f801050)sio0.write(a-0x1f801040,1,value);
        else if(a>=0x1f801130 && a<0x1f801140){} // Reserved fourth root-counter slot: writes are ignored.
        else if(isControl(a,a)){
            int shift=(a&3)*8,old=controlRead(a,a),word=(old&~(255<<shift))|((value&255)<<shift);
            controlWrite(a&~3,a&~3,word,255<<shift);
        }else if(a>=0x1fc00000 && a<0x1fc80000){}
        else throw missing("Write",a);
    }
    private void controlWrite(int a,int address,int value,int mask){
        if(address==0xfffe0130)cacheControl=value;
        else if(a>=0x1f801000 && a<=0x1f801020)memoryControl[(a-0x1f801000)/4]=value;
        else if(a==0x1f801060)ramSize=value;
        else if(a==0x1f801070)irqStatus&=(value|~mask);
        else if(a==0x1f801074)irqMask=value&0x7ff;
    }
    public void write16(int address,int value){
        if((address&1)!=0)throw new AddressError(address,true);
        int a=address&0x1fffffff;
        if(a<0x800000){if(!cacheIsolated){storeRam(a&0x1fffff,value,2);}return;}
        if(a>=0x1f801000 && a<0x1f802000){flushClocks();deadlineDirty=eventScheduling;}
        if(a>=0x1f801080 && a<0x1f801100){dma.write(a&~3,(value&65535)<<((a&2)*8),65535<<((a&2)*8));return;}
        if(a>=0x1f801c00 && a<0x1f802000){spu.write(a,value);return;}
        if(a>=0x1f801100 && a<0x1f801140){timers.write(a&~3,(a&2)==0?value:0);return;}
        if(a>=0x1f801040 && a<0x1f801050){sio0.write(a-0x1f801040,2,value);return;}
        if(isControl(a,address)){int shift=(a&2)*8,mask=65535<<shift;controlWrite(a&~3,address&~3,(controlRead(a,address)&~mask)|((value&65535)<<shift),mask);return;}
        write8(address,value);write8(address+1,value>>>8);
    }
    public void write32(int address,int value){
        if((address&3)!=0)throw new AddressError(address,true);
        int a=address&0x1fffffff;
        if(a<0x800000){if(!cacheIsolated){storeRam(a&0x1fffff,value,4);}return;}
        if(a>=0x1f801000 && a<0x1f802000){flushClocks();deadlineDirty=eventScheduling;}
        if(a>=0x1f801080 && a<0x1f801100){dma.write(a,value,-1);return;}
        if(a>=0x1f801c00 && a<0x1f802000){spu.write(a,value&65535);spu.write(a+2,value>>>16);return;}
        if(a>=0x1f801100 && a<0x1f801140){timers.write(a,value);return;}
        if(a>=0x1f801040 && a<0x1f801050){sio0.write(a-0x1f801040,4,value);return;}
        if(a==0x1f801810){gpu.write(value);return;}if(a==0x1f801814){gpu.control(value);return;}
        if(a==0x1f801820){mdec.write(value);return;}if(a==0x1f801824){mdec.control(value);return;}
        if(isControl(a,address)){controlWrite(a,address,value,-1);return;}
        for(int i=0;i<4;i++)write8(address+i,value>>>(8*i));
    }
    private UnsupportedOperationException missing(String operation,int address){return new UnsupportedOperationException(String.format("%s at %08X requires unimplemented hardware",operation,address));}
}
