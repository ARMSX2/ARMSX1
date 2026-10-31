package dev.psxlab.core;

/** Root counters; external video clocks are supplied explicitly. */
public final class Timers {
    private final Bus bus;
    private final int[] count=new int[3],mode={0x400,0x400,0x400},target=new int[3],prescale=new int[3];
    private final boolean[] fired=new boolean[3],blank=new boolean[3],released=new boolean[3];
    Timers(Bus bus){this.bus=bus;}
    public int read(int address){int i=(address-0x1f801100)/16,reg=(address&15)/4;if(i<0||i>=3)return -1;return switch(reg){case 0->count[i];case 1->{int m=mode[i];mode[i]&=~0x1800;yield m;}case 2->target[i];default->-1;};}
    public void write(int address,int value){int i=(address-0x1f801100)/16,reg=(address&15)/4;if(i<0||i>=3)return;switch(reg){case 0->count[i]=value&65535;case 1->{mode[i]=(value&0x3ff)|0x400;count[i]=prescale[i]=0;fired[i]=released[i]=false;}case 2->target[i]=value&65535;default->{}}}
    private boolean running(int i){
        if((mode[i]&1)==0)return true;int sync=(mode[i]>>>1)&3;
        if(i==2)return sync==1 || sync==2;
        return switch(sync){case 0->!blank[i];case 1->true;case 2->blank[i];default->released[i];};
    }
    private void pulse(int i,int ticks){
        if(!running(i))return;
        while(ticks>0){
            int toTarget=(target[i]-count[i])&65535;if(toTarget==0)toTarget=65536;
            int toOverflow=65535-count[i];if(toOverflow==0)toOverflow=65536;
            int step=Math.min(ticks,Math.min(toTarget,toOverflow));
            count[i]=(count[i]+step)&65535;ticks-=step;
            boolean match=step==toTarget,overflow=step==toOverflow;
            if(match)mode[i]|=0x800;if(overflow)mode[i]|=0x1000;
            boolean irq=match && (mode[i]&16)!=0 || overflow && (mode[i]&32)!=0;
            if(irq && (!fired[i] || (mode[i]&64)!=0)){
                if((mode[i]&128)!=0)mode[i]^=0x400;else mode[i]&=~0x400;
                if((mode[i]&0x400)==0)bus.raiseInterrupt(4+i);
                if((mode[i]&128)==0)mode[i]|=0x400;fired[i]=true;
            }
            if(match && (mode[i]&8)!=0)count[i]=0;
        }
    }
    public void cpuCycles(int cycles){
        if((mode[0]&0x100)==0)pulse(0,cycles);
        if((mode[1]&0x100)==0)pulse(1,cycles);
        if((mode[2]&0x200)==0)pulse(2,cycles);else{prescale[2]+=cycles;pulse(2,prescale[2]/8);prescale[2]%=8;}
    }
    private int ticksToEvent(int i){int a=(target[i]-count[i])&65535,b=65535-count[i];return Math.min(a==0?65536:a,b==0?65536:b);}
    int clocksToEvent(){
        int next=65536*8;
        for(int i=0;i<3;i++)if(running(i) && (i==2 || (mode[i]&0x100)==0)){
            int ticks=ticksToEvent(i);next=Math.min(next,i==2&&(mode[2]&0x200)!=0?ticks*8-prescale[2]:ticks);
        }
        return Math.max(1,next);
    }
    int dotClocksToEvent(){return (mode[0]&0x100)!=0 && running(0)?ticksToEvent(0):Integer.MAX_VALUE;}
    public void dotClocks(int clocks){if((mode[0]&0x100)!=0)pulse(0,clocks);}
    public void hblank(boolean active){boolean rising=active && !blank[0];setBlank(0,active);if(rising && (mode[1]&0x100)!=0)pulse(1,1);}
    public void vblank(boolean active){setBlank(1,active);}
    private void setBlank(int i,boolean active){
        boolean rising=active && !blank[i];blank[i]=active;
        if(rising && (mode[i]&1)!=0){int sync=(mode[i]>>>1)&3;if(sync==1 || sync==2)count[i]=0;if(sync==3)released[i]=true;}
    }
}
