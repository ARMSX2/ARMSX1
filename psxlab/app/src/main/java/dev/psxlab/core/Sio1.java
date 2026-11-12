package dev.psxlab.core;

/**
 * PlayStation SIO1 serial port. With no serial peripheral connected, transmission
 * completes without receiving data or asserting DSR/IRQ. Keep SIO1 independent
 * from the controller/memory-card SIO0 port.
 */
public final class Sio1 {
    private static final int TX_READY=1, TX_EMPTY=4;
    private int mode, control, baud;
    private int register(int offset) {
        switch (offset & ~1) {
            case 0x0: return 0xff; // Unconnected receive data bus.
            case 0x4: return TX_READY | TX_EMPTY;
            case 0x6: return 0;
            case 0x8: return mode;
            case 0xa: return control;
            case 0xe: return baud;
            default: return 0;
        }
    }
    public int read(int offset,int width) {
        int value=0;
        for(int i=0;i<width;i++){
            int pos=offset+i;
            int word=register(pos);
            value|=((word>>>((pos&1)*8))&255)<<(i*8);
        }
        return value;
    }
    public void write(int offset,int width,int value) {
        for(int i=0;i<width;i++){
            int pos=offset+i,byteValue=(value>>>(i*8))&255;
            if(pos==0)continue; // Disconnected serial TX.
            if(pos>=8 && pos<=9)mode=merge(mode,pos,byteValue);
            else if(pos>=10 && pos<=11){
                control=merge(control,pos,byteValue);
                if((control&0x40)!=0){control=0;mode=0;baud=0;}
                control&=~0x10; // ACK pulse self-clears.
            }
            else if(pos>=14 && pos<=15)baud=merge(baud,pos,byteValue);
        }
    }
    private static int merge(int old,int offset,int b){
        int shift=(offset&1)*8;
        return (old&~(255<<shift))|(b<<shift);
    }
}
