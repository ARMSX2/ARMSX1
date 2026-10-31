package dev.psxlab.core;

/** Controller/card serial host port, with scheduled byte transfers and ACK pulses. */
public final class Sio0 {
    private static final int ACK_DELAY=160,ACK_WIDTH=96;
    private final Bus bus;
    private SerialPad pad,secondPad;
    public void setPad(int slot,PadState state){if(slot==0)pad=new SerialPad(state);else if(slot==1)secondPad=new SerialPad(state);else throw new IllegalArgumentException("Controller slot");deselect();}
    void restored(){pad.restored();secondPad.restored();}
    private final MemoryCard[] cards=new MemoryCard[2];
    private int selectedDevice;
    public void insertCard(int slot,MemoryCard card){if(slot<0 || slot>1)throw new IllegalArgumentException("Card slot");cards[slot]=card;deselect();}
    private void deselect(){pad.deselect();secondPad.deselect();for(MemoryCard card:cards)if(card!=null)card.deselect();selectedDevice=0;}
    private int mode,control,baud,holding=-1,transmitting=-1,transferTicks,ackDelay,ackWidth;
    private boolean holdingEnabled,irq,dsr;
    private final int[] rx=new int[8];
    private int head,count,lastReceived=255;
    public long bytesTransferred,ackPulses;
    Sio0(Bus bus,PadState state){this.bus=bus;pad=new SerialPad(state);PadState absent=new PadState();absent.controllerType=3;secondPad=new SerialPad(absent);}
    private int status(){return (holding<0?1:0)|(count>0?2:0)|(transmitting<0 && holding<0?4:0)|(dsr?128:0)|(irq?512:0);}
    private int peek(int offset){return offset<count?rx[(head+offset)&7]:lastReceived;}
    private int pop(){if(count==0)return lastReceived;int value=rx[head];head=(head+1)&7;count--;return value;}
    private void receive(int value){lastReceived=value;if(count==8)rx[(head+7)&7]=value;else{rx[(head+count)&7]=value;count++;}}
    public int read(int offset,int width){
        if(width==4 && offset>=8)return read(offset,2)|(read(offset+2,2)<<16);
        if(offset<4){
            if(offset!=0)return peek(offset);
            if(width==1)return pop();
            if(width==2){int value=peek(0)|(peek(1)<<8);pop();return value;}
            int value=0;for(int i=0;i<4;i++)value|=pop()<<(i*8);return value;
        }
        int value=offset<8?status():offset<10?mode:offset<12?control:offset<14?0:baud;
        int shift=offset<8?(offset-4)*8:(offset&1)*8;
        return (value>>>shift)&(width==1?255:width==2?65535:-1);
    }
    public void write(int offset,int width,int value){
        if(width==4 && offset>=8){write(offset,2,value&65535);write(offset+2,2,value>>>16);return;}
        if(offset<4){if(offset==0){holding=value&255;holdingEnabled=(control&1)!=0;start();}return;}
        if(offset<8)return;
        int aligned=offset&~1,mask=width==1?255<<((offset&1)*8):65535;
        int old=aligned==8?mode:aligned==10?control:aligned==14?baud:0;
        int merged=(old&~mask)|((value<<((width==1?(offset&1):0)*8))&mask);
        if(aligned==8)mode=merged&0x13f;
        else if(aligned==14)baud=merged&65535;
        else if(aligned==10){
            if((merged&64)!=0){mode=control=baud=0;holding=transmitting=-1;transferTicks=ackDelay=ackWidth=0;irq=dsr=holdingEnabled=false;head=count=0;lastReceived=255;deselect();return;}
            if((merged&16)!=0)irq=false;
            if(((control^merged)&0x2002)!=0){deselect();ackDelay=ackWidth=0;dsr=false;}
            control=merged&0x3f07;start();checkInterrupt();
        }
    }
    private void start(){
        if(transmitting>=0 || holding<0 || (control&1)==0 && !holdingEnabled)return;
        transmitting=holding;holding=-1;holdingEnabled=false;
        int factor=switch(mode&3){case 2->16;case 3->64;default->1;};
        int bitTicks=Math.max((baud*factor)&~1,1),bits=5+((mode>>>2)&3);
        transferTicks=bitTicks*bits;
    }
    private void completeByte(){
        int incoming=255;boolean ack=false;
        if((control&2)!=0){
            int slot=(control>>>13)&1;if(selectedDevice==0)selectedDevice=transmitting==0x81?2:transmitting==1?1:3;
            SerialPad.Reply reply=selectedDevice==2 && cards[slot]!=null?cards[slot].exchange(transmitting):selectedDevice==1?(slot==0?pad:secondPad).exchange(transmitting):new SerialPad.Reply(255,false);
            incoming=reply.value();ack=reply.acknowledge();
        }
        if((control&6)!=0){receive(incoming);control&=~4;}
        transmitting=-1;bytesTransferred++;
        if(ack)ackDelay=ACK_DELAY;
        checkInterrupt();start();
    }
    private void checkInterrupt(){
        int threshold=1<<((control>>>8)&3);
        boolean condition=(control&0x1000)!=0 && dsr || (control&0x800)!=0 && count>=threshold || (control&0x400)!=0 && (status()&5)!=0;
        if(condition && !irq){irq=true;bus.raiseInterrupt(7);}
    }
    public void tick(int cycles){
        start();checkInterrupt();
        while(cycles>0){
            int next=Integer.MAX_VALUE;if(transferTicks>0)next=Math.min(next,transferTicks);if(ackDelay>0)next=Math.min(next,ackDelay);if(ackWidth>0)next=Math.min(next,ackWidth);if(next==Integer.MAX_VALUE)break;
            int amount=Math.min(cycles,next);cycles-=amount;
            boolean tx=transferTicks>0,delay=ackDelay>0,pulse=ackWidth>0;
            if(tx)transferTicks-=amount;if(delay)ackDelay-=amount;if(pulse)ackWidth-=amount;
            if(pulse && ackWidth==0)dsr=false;
            if(delay && ackDelay==0){dsr=true;ackWidth=ACK_WIDTH;ackPulses++;checkInterrupt();}
            if(tx && transferTicks==0)completeByte();
        }
    }
    int clocksToEvent(){int n=Integer.MAX_VALUE;if(transferTicks>0)n=Math.min(n,transferTicks);if(ackDelay>0)n=Math.min(n,ackDelay);if(ackWidth>0)n=Math.min(n,ackWidth);return n;}
    public String diagnostic(){return String.format(java.util.Locale.ROOT,"SIO0: %d bytes, %d ACKs, CTRL=%04X MODE=%04X BAUD=%04X RX=%d",bytesTransferred,ackPulses,control,mode,baud,count);}
}
