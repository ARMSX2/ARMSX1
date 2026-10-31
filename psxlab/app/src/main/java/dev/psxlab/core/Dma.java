package dev.psxlab.core;

/** Functional device DMA with scheduled burst chopping.
 * CPU bus stalls, channel arbitration and non-chopped transfer timing remain approximate.
 */
public final class Dma {
    private final Bus bus;
    private final int[][] channels=new int[7][3];
    private final boolean[] running=new boolean[7];
    private final Chopped[] chopped=new Chopped[7];
    private int choppedMask;
    private OutputTransfer mdecOutput;
    // Keep cyclic transfers live so the CPU can repair or cancel the list.
    // Ordinary lists retain their existing atomic transfer behaviour.
    private static final int LINKED_RETRY_CLOCKS=1024;
    private int linkedDelay;
    private long linkedYields;
    private int linkedLastAddress,linkedLastHeader;
    private EngineLog journal;
    public void setJournal(EngineLog value){journal=value;}
    private int priority=0x07654321,interrupt;
    public long choppedTransfers,choppedWindows;
    public boolean performanceEnabled;
    public final long[] performanceWords=new long[7];
    private static final class OutputTransfer {
        final int address,words,control;
        int delay;
        OutputTransfer(int address,int words,int control){this.address=address;this.words=words;this.control=control;delay=words;}
    }
    private static final class Chopped {
        int address,remaining,window,cpuCycles,wordCycles,delay;
        final int control,step;
        boolean cpuWindow;
        Chopped(int address,int words,int control,int wordCycles){
            this.address=address;remaining=words;this.control=control;step=(control&2)!=0?-4:4;
            window=1<<((control>>>16)&7);cpuCycles=1<<((control>>>20)&7);this.wordCycles=wordCycles;
            delay=Math.min(window,remaining)*wordCycles;
        }
    }
    Dma(Bus bus){this.bus=bus;}
    public void resumeCdrom(){start(3);}
    public void resumeMdec(){start(0);start(1);}
    public int read(int address){
        int offset=address-0x1f801080;
        if(offset<0x70){int reg=(offset&15)/4;return reg<3?channels[offset/16][reg]:0;}
        return (offset&~3)==0x70?priority:(offset&~3)==0x74?interrupt:0;
    }
    public void write(int address,int value,int mask){
        int offset=address-0x1f801080;
        if(offset<0x70){int ch=offset/16,reg=(offset&15)/4;if(reg<3){channels[ch][reg]=(channels[ch][reg]&~mask)|(value&mask);if(reg==0)channels[ch][reg]&=0xffffff;if(reg==2){chopped[ch]=null;choppedMask&=~(1<<ch);if(ch==1)mdecOutput=null;if(ch==2)linkedDelay=0;start(ch);}}}
        else if((offset&~3)==0x70){priority=(priority&~mask)|(value&mask);for(int ch=0;ch<7;ch++)start(ch);}
        else if((offset&~3)==0x74){int controls=mask&0xffffff;interrupt=(interrupt&~controls)|(value&controls);interrupt&=~(value&mask&0x7f000000);updateInterrupt();}
    }
    private void updateInterrupt(){
        boolean previous=interrupt<0;
        boolean active=(interrupt&0x8000)!=0 || (interrupt&0x800000)!=0 && (interrupt&0x7f000000)!=0;
        if(active){interrupt|=0x80000000;if(!previous)bus.raiseInterrupt(3);}else interrupt&=0x7fffffff;
    }
    private void start(int ch){
        if(running[ch])return;
        running[ch]=true;
        try{transfer(ch);}finally{running[ch]=false;}
    }
    private void transfer(int ch){
        if(chopped[ch]!=null || ch==1 && mdecOutput!=null || ch==2 && linkedDelay>0)return;
        int control=channels[ch][2],mode=(control>>>9)&3;
        if((priority&(8<<(ch*4)))==0 || (control&0x1000000)==0 || mode==0 && (control&0x10000000)==0)return;
        if(ch==5)throw new UnsupportedOperationException("DMA channel "+ch+" is not implemented");
        boolean chopping=(control&0x100)!=0;
        if(chopping && mode!=0)throw new UnsupportedOperationException("DMA chopping is only supported in burst mode");
        int address=channels[ch][0]&0x1ffffc,step=(control&2)!=0?-4:4;
        if(mode==2){
            if(ch!=2 || (control&1)==0)throw new UnsupportedOperationException("Invalid GPU linked-list DMA direction");
            java.util.HashSet<Integer> visited=new java.util.HashSet<>();
            while(true){
                if(!visited.add(address)){
                    // Re-read RAM after yielding. A cycle is neither a host
                    // exception nor a successful DMA completion.
                    linkedDelay=LINKED_RETRY_CLOCKS;linkedYields++;
                    if(journal!=null && journal.enabled() && (linkedYields<=4 || (linkedYields&(linkedYields-1))==0))
                        journal.event(EngineLog.WARN,"dma.gpu.linked.yield",java.util.Map.of("yields",linkedYields,"last_address",linkedLastAddress,"last_header",Integer.toUnsignedLong(linkedLastHeader),"next_address",channels[2][0],"control",channels[2][2],"resume_clocks",linkedDelay));
                    return;
                }
                int header=bus.read32(address),words=header>>>24;
                linkedLastAddress=address;linkedLastHeader=header;
                if(performanceEnabled)performanceWords[2]+=words;
                for(int i=0;i<words;i++){address=(address+4)&0x1ffffc;bus.gpu.write(bus.read32(address));}
                // Test the end bit before masking the RAM mirror/address bits.
                if((header&0x800000)!=0){channels[ch][0]=0xffffff;complete(ch);return;}
                channels[ch][0]=header&0xfffffc;address=header&0x1ffffc;
            }
        }else{
            if(mode>1)throw new UnsupportedOperationException("Reserved DMA synchronization mode");
            long words=channels[ch][1]&65535;if(words==0)words=65536;
            if(mode==1){long blocks=(channels[ch][1]>>>16)&65535;words*=blocks==0?65536:blocks;}
            if(words>1048576)throw new UnsupportedOperationException("DMA transfer exceeds diagnostic limit");
            if(ch==6 && (mode!=0 || (control&1)!=0))throw new UnsupportedOperationException("Unsupported OTC DMA mode");
            if(ch==0){
                if((control&1)==0)throw new UnsupportedOperationException("MDEC input DMA must read RAM");
                if(!bus.mdec.inputRequest())return;
            }
            if(ch==1){
                if((control&1)!=0 || step!=4)throw new UnsupportedOperationException("Unsupported MDEC output DMA direction");
                if(!bus.mdec.outputRequest() || bus.mdec.availableWords()<words)return;
                if(!chopping){
                    // Output DMA must not finish inside the CHCR write. Movie callbacks
                    // publish their in-flight state after starting the transfer.
                    // One clock/word models RAM bus time; decoder stalls/arbitration
                    // remain approximate, as does the synchronous input decoder.
                    mdecOutput=new OutputTransfer(address,(int)words,control);return;
                }
            }
            if(ch==3){
                if(mode!=0 || (control&1)!=0)throw new UnsupportedOperationException("Unsupported CD-ROM DMA mode");
                if(words>585)throw new UnsupportedOperationException("CD-ROM DMA larger than one sector is not implemented");
                if(bus.cdrom.availableBytes()<words*4)return;
            }
            if(chopping){
                // Nominal transfer budgets; programmable memory wait states remain approximate.
                int wordCycles=ch==3?40:ch==4?4:1;
                chopped[ch]=new Chopped(address,(int)words,control,wordCycles);choppedMask|=1<<ch;
                channels[ch][2]&=~0x10000000;choppedTransfers++;return;
            }
            for(long i=0;i<words;i++){
                transferWord(ch,address,control,i==words-1);
                address=(address+step)&0x1ffffc;
            }
            if(mode==1)channels[ch][0]=address;
        }
        complete(ch);
    }
    private void transferWord(int ch,int address,int control,boolean last){
        if(performanceEnabled)performanceWords[ch]++;
        if(ch==0)bus.mdec.write(bus.read32(address));
        else if(ch==1){int destination=(address+bus.mdec.dmaAddressOffset())&0x1ffffc;bus.write32(destination,bus.mdec.read());}
        else if(ch==6)bus.write32(address,last?0xffffff:(address-4)&0x1fffff);
        else if(ch==3)bus.write32(address,bus.cdrom.readDmaWord());
        else if(ch==4){
            if((control&1)!=0){int word=bus.read32(address);bus.spu.writeTransfer(word&65535);bus.spu.writeTransfer(word>>>16);}
            else bus.write32(address,bus.spu.readTransfer()|(bus.spu.readTransfer()<<16));
        }
        else if((control&1)!=0)bus.gpu.write(bus.read32(address));else bus.write32(address,bus.gpu.read());
    }
    /** Advance DMA/CPU windows without skipping devices or completing the DMA early. */
    public void tick(int cycles){
        int linkedElapsed=cycles;
        while(linkedDelay>0 && (priority&0x800)!=0){
            if(linkedElapsed<linkedDelay){linkedDelay-=linkedElapsed;break;}
            linkedElapsed-=linkedDelay;linkedDelay=0;start(2);
        }
        if(mdecOutput!=null && (priority&0x80)!=0){
            OutputTransfer state=mdecOutput;
            state.delay=Math.max(0,state.delay-cycles);
            if(state.delay==0 && bus.mdec.outputRequest() && bus.mdec.availableWords()>=state.words){
                int address=state.address;
                for(int i=0;i<state.words;i++){transferWord(1,address,state.control,i==state.words-1);address=(address+4)&0x1ffffc;}
                if(((state.control>>>9)&3)==1)channels[1][0]=address;
                mdecOutput=null;complete(1);
            }
        }
        if(choppedMask==0)return;
        for(int ch=0;ch<7;ch++){
            Chopped state=chopped[ch];
            if(state==null || (priority&(8<<(ch*4)))==0)continue;
            int elapsed=cycles;
            while(state!=null && elapsed>=state.delay){
                elapsed-=state.delay;
                if(state.cpuWindow){
                    state.cpuWindow=false;state.delay=Math.min(state.window,state.remaining)*state.wordCycles;
                }else{
                    int count=Math.min(state.window,state.remaining);
                    if(ch==3 && bus.cdrom.availableBytes()<count*4 || ch==0 && !bus.mdec.inputRequest() || ch==1 && (!bus.mdec.outputRequest() || bus.mdec.availableWords()<count)){
                        state.delay=0;elapsed=0;break;
                    }
                    for(int i=0;i<count;i++){
                        transferWord(ch,state.address,state.control,state.remaining==1);
                        state.address=(state.address+state.step)&0x1ffffc;state.remaining--;
                    }
                    channels[ch][0]=state.address;
                    channels[ch][1]=(channels[ch][1]&0xffff0000)|(state.remaining&65535);
                    choppedWindows++;
                    if(state.remaining==0){chopped[ch]=null;choppedMask&=~(1<<ch);complete(ch);state=null;}
                    else{state.cpuWindow=true;state.delay=state.cpuCycles;}
                }
            }
            if(state!=null)state.delay-=elapsed;
        }
    }
    private void complete(int ch){
        channels[ch][2]&=~0x11000000;
        if((interrupt&0x800000)!=0 && (interrupt&(1<<(16+ch)))!=0)interrupt|=1<<(24+ch);
        updateInterrupt();
    }
    int clocksToEvent(){int n=mdecOutput!=null&&(priority&0x80)!=0?Math.max(1,mdecOutput.delay):Integer.MAX_VALUE;if(linkedDelay>0&&(priority&0x800)!=0)n=Math.min(n,linkedDelay);for(int ch=0;ch<7;ch++)if(chopped[ch]!=null&&(priority&(8<<(ch*4)))!=0)n=Math.min(n,Math.max(1,chopped[ch].delay));return n;}
    public String diagnostic(){return "DMA chopped="+choppedTransfers+" windows="+choppedWindows+String.format(java.util.Locale.ROOT," active=%02X CD-MADR=%06X BCR=%08X CHCR=%08X; GPU-MADR=%06X CHCR=%08X linked-yields=%d resume=%d last=%06X header=%08X",choppedMask,channels[3][0],channels[3][1],channels[3][2],channels[2][0],channels[2][2],linkedYields,linkedDelay,linkedLastAddress,linkedLastHeader);}
}
