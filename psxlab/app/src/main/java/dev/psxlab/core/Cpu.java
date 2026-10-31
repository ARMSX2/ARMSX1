package dev.psxlab.core;

/** Functional MIPS-I integer interpreter with COP2/GTE. Cycle/cache accuracy remains separate work. */
public final class Cpu {
    private NativeCpu nativeCpu;
    public boolean enableNative(boolean recompile,boolean events){
        return enableNative(recompile,events,false);
    }
    public boolean enableNative(boolean recompile,boolean events,boolean ramWaitStates){
        if(!NativeCore.AVAILABLE || !gte.nativeExecution())return false;
        if(nativeCpu!=null)return true;
        bus.setEventScheduling(events);nativeCpu=new NativeCpu(bus,recompile,ramWaitStates);nativeCpu.importState(this);return true;
    }
    public void closeNative(){if(nativeCpu!=null){nativeCpu.exportState(this);nativeCpu.close();nativeCpu=null;}}
    public String executionDiagnostic(){return nativeCpu==null?"CPU Java interpreter":nativeCpu.diagnostic()+" · device dispatches="+bus.deviceDispatches;}
    public String executionMode(){return nativeCpu==null?"Java interpreter":"Native blocks / ARM64";}
    public long[] nativeStatistics(){return nativeCpu==null?new long[8]:nativeCpu.statistics();}
    /** A snapshot boundary; tests and debugging can still single-step precisely. */
    public int run(int count){if(count<=0)return 0;if(nativeCpu!=null)return nativeCpu.run(this,count);for(int i=0;i<count;i++)step();return count;}
    public int runToFrame(int count){if(nativeCpu!=null)return nativeCpu.run(this,count,true);long frame=bus.gpu.vblanks;int i=0;for(;i<count;i++){if((i&63)==0&&bus.gpu.vblanks!=frame)break;step();}return i;}
    void synchronizeNativeState(){if(nativeCpu!=null)nativeCpu.importState(this);}
    void exportPipeline(java.nio.IntBuffer b){b.put(pc).put(next).put(hi).put(lo).put(loadReg).put(loadValue).put(branchPending?1:0).put(branchTaken?1:0).put(faultBranchTaken?1:0).put(faultBranchTarget).put(lastException).put(faultPc).put(faultInstruction).put(bus.cacheIsolated?1:0);}
    void importPipeline(java.nio.IntBuffer b){pc=b.get();next=b.get();hi=b.get();lo=b.get();loadReg=b.get();loadValue=b.get();branchPending=b.get()!=0;branchTaken=b.get()!=0;faultBranchTaken=b.get()!=0;faultBranchTarget=b.get();lastException=b.get();faultPc=b.get();faultInstruction=b.get();bus.cacheIsolated=b.get()!=0;}
    public final int[] r=new int[32];
    public final int[] cop0=new int[32];
    public final Gte gte;
    public int pc=0x80010000,hi,lo;
    private int next=pc+4,loadReg,loadValue;
    private boolean branchPending;
    private boolean branchTaken,faultBranchTaken;
    private int faultBranchTarget;
    /** Guest clock counter; the legacy interpreter charges one clock per instruction. */
    public long instructions;
    public long exceptions;
    public int lastException;
    public int faultPc,faultInstruction;
    public String diagnostic(){StringBuilder s=new StringBuilder(String.format(java.util.Locale.ROOT,"PC=%08X instruction=%08X next=%08X SR=%08X CAUSE=%08X EPC=%08X\n",faultPc,faultInstruction,pc,cop0[12],cop0[13],cop0[14]));for(int i=0;i<32;i++)s.append(String.format(java.util.Locale.ROOT,"r%d=%08X%s",i,r[i],i%4==3?"\n":" "));return s.toString();}
    private final Bus bus;
    public Cpu(Bus bus){this.bus=bus;gte=new Gte(bus.nativeCores);cop0[15]=2;}
    public void jump(int address){pc=address;next=address+4;loadReg=0;branchPending=branchTaken=false;}
    public void reset(){java.util.Arrays.fill(r,0);java.util.Arrays.fill(cop0,0);gte.reset();cop0[12]=0x10900000;cop0[15]=2;hi=lo=0;instructions=exceptions=0;bus.cacheIsolated=false;jump(0xbfc00000);}
    private void load(int reg,int value){loadReg=reg;loadValue=value;}
    private void commitLoad(){if(loadReg!=0)r[loadReg]=loadValue;loadReg=0;}
    private void trap(int code,int address,boolean delay,int badAddress){
        if(code==4 || code==5)cop0[8]=badAddress;
        cop0[13]=(cop0[13]&0x0000ff00)|(code<<2)|(delay?0x80000000:0)|(delay && faultBranchTaken?0x40000000:0);
        if(delay && faultBranchTaken)cop0[6]=faultBranchTarget;
        cop0[14]=delay?address-4:address;
        cop0[12]=(cop0[12]&~63)|((cop0[12]<<2)&63);
        pc=(cop0[12]&0x400000)!=0?0xbfc00180:0x80000080;
        next=pc+4;branchPending=branchTaken=false;lastException=code;exceptions++;r[0]=0;
    }
    private int add(int a,int b){return Math.addExact(a,b);}
    private void branch(boolean taken,int target){branchPending=true;branchTaken=taken;if(taken)next=target;}
    public void step(){
        if(nativeCpu!=null){nativeCpu.run(this,1);return;}
        // One base tick per instruction; access stalls and GTE timing are not modeled yet.
        bus.tick();
        int at=pc;boolean delay=branchPending;
        faultBranchTaken=branchTaken;faultBranchTarget=next;
        if(bus.interruptPending())cop0[13]|=0x400;else cop0[13]&=~0x400;
        if((cop0[12]&1)!=0 && (cop0[12]&cop0[13]&0xff00)!=0){
            commitLoad();
            // A GTE command issues even when the interrupt's EPC points to it.
            // The BIOS recognizes this case and resumes at EPC+4.
            if((cop0[12]&0x40000000)!=0){
                try{int command=bus.fetchInstruction(at);if((command&0xfe000000)==0x4a000000)gte.execute(command);}
                catch(Bus.AddressError ignored){} // IRQ takes priority over a bad next fetch.
            }
            trap(0,at,delay,0);instructions++;return;
        }
        int instruction;
        try{instruction=bus.fetchInstruction(at);}catch(Bus.AddressError e){commitLoad();trap(4,at,delay,e.address);instructions++;return;}
        if(instruction==0){commitLoad();pc=next;next+=4;branchPending=branchTaken=false;r[0]=0;instructions++;return;}
        int op=instruction>>>26,s=(instruction>>>21)&31,t=(instruction>>>16)&31,d=(instruction>>>11)&31;
        int a=r[s],b=r[t],imm=(short)instruction,u=instruction&65535;
        int merge=loadReg==t && t!=0?loadValue:b;
        commitLoad();pc=next;next+=4;branchPending=branchTaken=false;
        try{
            switch(op){
                case 0 -> {
                    switch(instruction&63){
                        case 0 -> r[d]=b<<((instruction>>>6)&31);
                        case 2 -> r[d]=b>>>((instruction>>>6)&31);
                        case 3 -> r[d]=b>>((instruction>>>6)&31);
                        case 4 -> r[d]=b<<(a&31);
                        case 6 -> r[d]=b>>>(a&31);
                        case 7 -> r[d]=b>>(a&31);
                        case 8 -> branch(true,a);
                        case 9 -> {r[d]=at+8;branch(true,a);}
                        case 12 -> trap(8,at,delay,0);
                        case 13 -> trap(9,at,delay,0);
                        case 16 -> r[d]=hi;
                        case 17 -> hi=a;
                        case 18 -> r[d]=lo;
                        case 19 -> lo=a;
                        case 24 -> {long product=(long)a*b;lo=(int)product;hi=(int)(product>>32);}
                        case 25 -> {long product=Integer.toUnsignedLong(a)*Integer.toUnsignedLong(b);lo=(int)product;hi=(int)(product>>>32);}
                        case 26 -> {if(b==0){lo=a>=0?-1:1;hi=a;}else{lo=a/b;hi=a%b;}}
                        case 27 -> {if(b==0){lo=-1;hi=a;}else{lo=Integer.divideUnsigned(a,b);hi=Integer.remainderUnsigned(a,b);}}
                        case 32 -> r[d]=add(a,b);
                        case 33 -> r[d]=a+b;
                        case 34 -> r[d]=Math.subtractExact(a,b);
                        case 35 -> r[d]=a-b;
                        case 36 -> r[d]=a&b;
                        case 37 -> r[d]=a|b;
                        case 38 -> r[d]=a^b;
                        case 39 -> r[d]=~(a|b);
                        case 42 -> r[d]=a<b?1:0;
                        case 43 -> r[d]=Integer.compareUnsigned(a,b)<0?1:0;
                        default -> trap(10,at,delay,0);
                    }
                }
                case 1 -> {
                    if(t==16 || t==17)r[31]=at+8;
                    branch((t&1)==0?a<0:a>=0,at+4+(imm<<2));
                }
                case 2,3 -> {if(op==3)r[31]=at+8;branch(true,((at+4)&0xf0000000)|((instruction&0x3ffffff)<<2));}
                case 4 -> branch(a==b,at+4+(imm<<2));
                case 5 -> branch(a!=b,at+4+(imm<<2));
                case 6 -> branch(a<=0,at+4+(imm<<2));
                case 7 -> branch(a>0,at+4+(imm<<2));
                case 8 -> r[t]=add(a,imm);
                case 9 -> r[t]=a+imm;
                case 10 -> r[t]=a<imm?1:0;
                case 11 -> r[t]=Integer.compareUnsigned(a,imm)<0?1:0;
                case 12 -> r[t]=a&u;
                case 13 -> r[t]=a|u;
                case 14 -> r[t]=a^u;
                case 15 -> r[t]=u<<16;
                case 16 -> {
                    if((cop0[12]&0x10000002)==2){trap(11,at,delay,0);break;}
                    if(s==0){if(d==0 || d==1 || d==2 || d==4 || d==10)trap(10,at,delay,0);else load(t,cop0[d]);}
                    else if(s==4){
                        if(d==13)cop0[13]=(cop0[13]&~0x300)|(b&0x300);
                        else if(d!=8 && d!=14 && d!=15)cop0[d]=b;
                        bus.cacheIsolated=(cop0[12]&0x10000)!=0;
                    }else if(s==16 && (instruction&63)==16)cop0[12]=(cop0[12]&~15)|((cop0[12]>>>2)&15);
                    else trap(10,at,delay,0);
                }
                case 17,19 -> {trap(11,at,delay,0);cop0[13]|=(op&3)<<28;}
                case 18 -> {
                    if((cop0[12]&0x40000000)==0){trap(11,at,delay,0);cop0[13]|=2<<28;break;}
                    if(s>=16)gte.execute(instruction);
                    else switch(s){
                        case 0 -> load(t,gte.readData(d));
                        case 2 -> load(t,gte.readControl(d));
                        case 4 -> gte.writeData(d,b);
                        case 6 -> gte.writeControl(d,b);
                        case 8 -> branch((t&1)==0,at+4+(imm<<2));
                        default -> trap(10,at,delay,0);
                    }
                }
                case 50,58 -> {
                    if((cop0[12]&0x40000000)==0){trap(11,at,delay,0);cop0[13]|=2<<28;break;}
                    if(op==50)gte.writeData(t,bus.read32(a+imm));else bus.write32(a+imm,gte.readData(t));
                }
                case 32 -> load(t,(byte)bus.read8(a+imm));
                case 33 -> load(t,(short)bus.read16(a+imm));
                case 34 -> {int address=a+imm,shift=(3-(address&3))*8;load(t,(merge&(0x00ffffff>>>((address&3)*8)))|(bus.read32(address&~3)<<shift));}
                case 35 -> load(t,bus.read32(a+imm));
                case 36 -> load(t,bus.read8(a+imm));
                case 37 -> load(t,bus.read16(a+imm));
                case 38 -> {int address=a+imm,shift=(address&3)*8;load(t,(merge&(0xffffff00<<((3-(address&3))*8)))|(bus.read32(address&~3)>>>shift));}
                case 40 -> bus.write8(a+imm,b);
                case 41 -> bus.write16(a+imm,b);
                case 42 -> {int address=a+imm;for(int i=0;i<=(address&3);i++)bus.write8((address&~3)+i,b>>>((3-(address&3)+i)*8));}
                case 43 -> bus.write32(a+imm,b);
                case 46 -> {int address=a+imm;for(int i=(address&3);i<4;i++)bus.write8((address&~3)+i,b>>>((i-(address&3))*8));}
                default -> trap(10,at,delay,0);
            }
        }catch(UnsupportedOperationException e){faultPc=at;faultInstruction=instruction;throw e;}
        catch(ArithmeticException e){trap(12,at,delay,0);}
        catch(Bus.AddressError e){trap(e.store?5:4,at,delay,e.address);}
        r[0]=0;instructions++;
    }
}
