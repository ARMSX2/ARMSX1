package dev.psxlab.core;

import java.util.Arrays;

/** Original 128 KiB card and sector-atomic serial protocol. */
public final class MemoryCard {
    public static final int SIZE=128*1024;
    private final byte[] data;
    private final byte[] incoming=new byte[128];
    private final Runnable changed;
    private int step,command,address,checksum,previous,result=0x47,flags=8;
    private boolean finished;
    public MemoryCard(byte[] image,Runnable changed){if(image.length!=SIZE)throw new IllegalArgumentException("Memory card must be 128 KiB");data=image.clone();this.changed=changed;}
    public static byte[] formatted(){
        byte[] image=new byte[SIZE];Arrays.fill(image,(byte)255);
        for(int sector=0;sector<36;sector++){int at=sector*128;Arrays.fill(image,at,at+128,(byte)0);if(sector==0){image[at]='M';image[at+1]='C';}else if(sector<16){image[at]=(byte)0xa0;image[at+8]=image[at+9]=(byte)255;}else Arrays.fill(image,at,at+4,(byte)255);int xor=0;for(int i=0;i<127;i++)xor^=image[at+i]&255;image[at+127]=(byte)xor;}
        System.arraycopy(image,0,image,63*128,128);return image;
    }
    public synchronized byte[] image(){return data.clone();}
    void deselect(){step=command=address=checksum=previous=0;finished=false;}
    SerialPad.Reply exchange(int value){
        value&=255;if(finished)return new SerialPad.Reply(255,false);
        int out=255;boolean last=false;int at=step++;
        if(at==0){if(value!=0x81){finished=true;return new SerialPad.Reply(255,false);}return new SerialPad.Reply(255,true);}
        if(at==1){command=value;if(command!=0x52 && command!=0x57 && command!=0x53)last=true;out=flags;}
        else if(at==2)out=0x5a;
        else if(at==3)out=0x5d;
        else if(command==0x53){int[] id={0x5c,0x5d,4,0,0,0x80};out=id[at-4];last=at==9;}
        else if(at==4){address=value<<8;checksum=value;out=0;}
        else if(at==5){address|=value;checksum^=value;out=previous;}
        else if(command==0x52){
            if(at==6)out=0x5c;else if(at==7)out=0x5d;
            else if(at==8)out=address<1024?address>>>8:255;
            else if(at==9){out=address<1024?address&255:255;last=address>=1024;}
            else if(at<138){synchronized(this){out=data[address*128+at-10]&255;}checksum^=out;}
            else if(at==138)out=checksum;
            else{out=0x47;last=true;}
        }else if(command==0x57){
            if(at<134){incoming[at-6]=(byte)value;checksum^=value;out=previous;}
            else if(at==134){result=address>=1024?255:checksum!=value?0x4e:0x47;out=previous;}
            else if(at==135)out=0x5c;
            else if(at==136)out=0x5d;
            else{out=result;last=true;if(result==0x47){synchronized(this){System.arraycopy(incoming,0,data,address*128,128);}flags&=~8;if(changed!=null)changed.run();}}
        }
        previous=value;if(last)finished=true;return new SerialPad.Reply(out,!last);
    }
}
