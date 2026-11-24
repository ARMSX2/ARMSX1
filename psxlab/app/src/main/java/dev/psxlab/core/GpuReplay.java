package dev.psxlab.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Bounded graphics-only captures. No BIOS, disc or executable is needed to replay one. */
public final class GpuReplay {
    public static final String FILE_NAME="graphics-replay.armgfx";
    public static final int VRAM_WORDS=1024*512,MAX_WORDS=1024*1024,MAX_OPERATIONS=65536;
    public static final int BATCH=1,FILL=2,COPY=3,UPLOAD=4,FRAME=5;
    /** Guest main RAM and data scratchpad, stored whole or not at all (version 2). */
    public static final int RAM_BYTES=2*1024*1024,SCRATCHPAD_BYTES=1024;
    /** Sound RAM and the raw SPU register file, stored whole or not at all (version 3). */
    public static final int SPU_RAM_BYTES=512*1024,SPU_REGISTERS=256;
    private static final long MAGIC=0x41524d5358475055L;
    private static final int VERSION=3;
    public record Operation(int kind,int[] words){}
    /** Arrays belong to this immutable capture; callers must not change them. */
    public record Capture(int[] before,List<Operation> operations,int[] after,int[] display,int[] environment,
                          long firstFrame,long lastFrame,long startedNanos,long capturedNanos){}
    /** ram and scratchpad are null for version-1 captures or when memory was not recorded. */
    public record Document(String metadata,Capture capture,byte[] ram,byte[] scratchpad,byte[] spuRam,int[] spuRegisters){
        public Document(String metadata,Capture capture){this(metadata,capture,null,null,null,null);}
        public Document(String metadata,Capture capture,byte[] ram,byte[] scratchpad){this(metadata,capture,ram,scratchpad,null,null);}
    }
    public record Picture(int width,int height,int[] pixels){}

    private static void validate(Operation operation)throws IOException{
        int[] p=operation.words();int n=p.length;
        switch(operation.kind()){
            case BATCH -> {
                for(int at=0;at<n;){int count=p[at++];if(count<2||count>12||at+count+7>n)throw new IOException("Invalid draw packet");
                    int op=p[at]>>>24,expected=op>=0x20&&op<0x40?((op&16)!=0?((op&8)!=0?4:3)*(2+((op>>>2)&1)):1+((op&8)!=0?4:3)*(1+((op>>>2)&1))):op>=0x40&&op<0x60?((op&16)!=0?4:3):op>=0x60&&op<0x80?2+((op>>>2)&1)+((op&24)==0?1:0):0;
                    if(expected!=count)throw new IOException("Invalid draw opcode/length");at+=count+7;
                }
            }
            case FILL -> {if(n!=3)throw new IOException("Invalid fill");}
            case COPY -> {if(n!=4)throw new IOException("Invalid copy");}
            case UPLOAD -> {if(n<6||p[0]<0||p[3]<1||p[3]>1024||p[4]<p[0]||p[4]>VRAM_WORDS||n-6>(p[4]-p[0]+1)/2)throw new IOException("Invalid upload");}
            case FRAME -> {if(n!=8)throw new IOException("Invalid display state");}
            default -> throw new IOException("Unknown graphics operation");
        }
    }
    public static void write(File file,String metadata,Capture capture)throws IOException{write(file,metadata,capture,null,null);}
    /** ram/scratchpad: whole guest memory at the capture point, or both null to omit it. */
    public static void write(File file,String metadata,Capture capture,byte[] ram,byte[] scratchpad)throws IOException{write(file,metadata,capture,ram,scratchpad,null,null);}
    /** spuRam/spuRegisters: sound RAM and SPU registers at the capture point, or both null to omit them. */
    public static void write(File file,String metadata,Capture capture,byte[] ram,byte[] scratchpad,byte[] spuRam,int[] spuRegisters)throws IOException{
        if((spuRam==null)!=(spuRegisters==null)||spuRam!=null&&(spuRam.length!=SPU_RAM_BYTES||spuRegisters.length!=SPU_REGISTERS))throw new IOException("Sound memory snapshot has the wrong size");
        if((ram==null)!=(scratchpad==null)||ram!=null&&(ram.length!=RAM_BYTES||scratchpad.length!=SCRATCHPAD_BYTES))throw new IOException("Guest memory snapshot has the wrong size");
        if(metadata.length()>16000||capture.before().length!=VRAM_WORDS||capture.after().length!=VRAM_WORDS||capture.display().length!=8||capture.environment().length!=7||capture.operations().size()>MAX_OPERATIONS)throw new IOException("Graphics capture exceeds limits");
        int words=0;for(Operation operation:capture.operations()){validate(operation);words+=operation.words().length;if(words>MAX_WORDS)throw new IOException("Graphics commands exceed limit");}
        File parent=file.getParentFile();if(parent!=null&&!parent.isDirectory()&&!parent.mkdirs())throw new IOException("Cannot create graphics capture folder");
        Path pending=file.toPath().resolveSibling(file.getName()+".tmp");
        try{
            try(DataOutputStream out=new DataOutputStream(new BufferedOutputStream(new GZIPOutputStream(Files.newOutputStream(pending))))){
                out.writeLong(MAGIC);out.writeInt(VERSION);out.writeUTF(metadata);out.writeLong(capture.firstFrame());out.writeLong(capture.lastFrame());out.writeLong(capture.startedNanos());out.writeLong(capture.capturedNanos());
                for(int value:capture.display())out.writeInt(value);for(int value:capture.environment())out.writeInt(value);
                for(int value:capture.before())out.writeShort(value);
                out.writeInt(capture.operations().size());
                for(Operation operation:capture.operations()){out.writeInt(operation.kind());out.writeInt(operation.words().length);for(int value:operation.words())out.writeInt(value);}
                for(int value:capture.after())out.writeShort(value);
                out.writeInt(ram==null?0:RAM_BYTES);if(ram!=null){out.write(ram);out.writeInt(SCRATCHPAD_BYTES);out.write(scratchpad);}
                out.writeInt(spuRam==null?0:SPU_RAM_BYTES);if(spuRam!=null){out.write(spuRam);for(int value:spuRegisters)out.writeShort(value);}
            }
            Files.move(pending,file.toPath(),StandardCopyOption.REPLACE_EXISTING);
        }finally{Files.deleteIfExists(pending);}
    }
    public static Document read(File file)throws IOException{
        try(DataInputStream in=new DataInputStream(new BufferedInputStream(new GZIPInputStream(new FileInputStream(file))))){
            if(in.readLong()!=MAGIC)throw new IOException("Not an ArmSX1 graphics capture");
            int version=in.readInt();if(version<1||version>VERSION)throw new IOException("Unsupported graphics capture version "+version);
            String metadata=in.readUTF();if(metadata.length()>16000)throw new IOException("Oversized metadata");
            long first=in.readLong(),last=in.readLong(),start=in.readLong(),end=in.readLong();int[] display=new int[8],environment=new int[7],before=new int[VRAM_WORDS],after=new int[VRAM_WORDS];
            for(int i=0;i<8;i++)display[i]=in.readInt();for(int i=0;i<7;i++)environment[i]=in.readInt();for(int i=0;i<VRAM_WORDS;i++)before[i]=in.readUnsignedShort();
            int count=in.readInt(),words=0;if(count<0||count>MAX_OPERATIONS)throw new IOException("Oversized operation count");
            List<Operation> operations=new ArrayList<>(count);
            for(int i=0;i<count;i++){int kind=in.readInt(),length=in.readInt();if(length<0||length>MAX_WORDS-words)throw new IOException("Oversized command data");words+=length;int[] data=new int[length];for(int j=0;j<length;j++)data[j]=in.readInt();Operation operation=new Operation(kind,data);validate(operation);operations.add(operation);}
            for(int i=0;i<VRAM_WORDS;i++)after[i]=in.readUnsignedShort();
            byte[] ram=null,scratchpad=null;
            if(version>=2){
                int ramLength=in.readInt();
                if(ramLength==RAM_BYTES){ram=new byte[RAM_BYTES];in.readFully(ram);if(in.readInt()!=SCRATCHPAD_BYTES)throw new IOException("Invalid scratchpad snapshot");scratchpad=new byte[SCRATCHPAD_BYTES];in.readFully(scratchpad);}
                else if(ramLength!=0)throw new IOException("Invalid guest memory snapshot");
            }
            byte[] spuRam=null;int[] spuRegisters=null;
            if(version>=3){
                int spuLength=in.readInt();
                if(spuLength==SPU_RAM_BYTES){spuRam=new byte[SPU_RAM_BYTES];in.readFully(spuRam);spuRegisters=new int[SPU_REGISTERS];for(int i=0;i<SPU_REGISTERS;i++)spuRegisters[i]=in.readUnsignedShort();}
                else if(spuLength!=0)throw new IOException("Invalid sound memory snapshot");
            }
            if(in.read()!=-1)throw new IOException("Trailing graphics data");
            return new Document(metadata,new Capture(before,List.copyOf(operations),after,display,environment,first,last,start,end),ram,scratchpad,spuRam,spuRegisters);
        }
    }
    /** Independent Java raster replay. Live texture feedback can differ from the hardware snapshot contract. */
    public static int[] replay(Capture capture){
        int[] vram=capture.before().clone();SoftwareRasterizer raster=new SoftwareRasterizer(vram,false);
        for(Operation operation:capture.operations()){
            int[] p=operation.words();
            switch(operation.kind()){
                case BATCH -> {int count=0;for(int at=0;at<p.length;count++)at+=p[at]+8;raster.drawBatch(p,count);}
                case FILL -> {int x=p[1]&0x3f0,y=(p[1]>>>16)&511,w=((p[2]&1023)+15)&~15,h=(p[2]>>>16)&511,color=((p[0]&255)>>>3)|(((p[0]>>>8)&255)>>>3)<<5|(((p[0]>>>16)&255)>>>3)<<10;for(int row=0;row<h;row++)for(int col=0;col<w;col++)vram[((y+row)&511)*1024+((x+col)&1023)]=color;}
                case COPY -> {int sx=p[0]&1023,sy=(p[0]>>>16)&511,x=p[1]&1023,y=(p[1]>>>16)&511,w=((p[2]-1)&1023)+1,h=(((p[2]>>>16)-1)&511)+1;for(int row=0;row<h;row++)for(int col=0;col<w;col++)store(vram,((y+row)&511)*1024+((x+col)&1023),vram[((sy+row)&511)*1024+((sx+col)&1023)],p[3]);}
                case UPLOAD -> {int samples=Math.min((p.length-6)*2,p[4]-p[0]);for(int i=0;i<samples;i++){int index=p[0]+i;store(vram,((p[2]+index/p[3])&511)*1024+((p[1]+index%p[3])&1023),(p[6+i/2]>>>((i&1)*16))&65535,p[5]);}}
                case FRAME -> {} // Drawing already contains its exact environment; scanout state is retained separately.
                default -> throw new IllegalArgumentException("Unknown graphics operation");
            }
        }
        return vram;
    }
    private static void store(int[] vram,int at,int value,int mask){if((mask&2)==0||(vram[at]&32768)==0)vram[at]=value|((mask&1)<<15);}
    public static Picture picture(int[] vram,int[] display){
        int status=display[0],divider=(status&0x10000)!=0?7:switch((status>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};
        int width=Math.max(1,Math.min(1024,(display[4]-display[3])/divider)),height=Math.max(1,Math.min(512,(display[6]-display[5])*((status&0x480000)==0x480000?2:1)));int[] pixels=new int[width*height];Arrays.fill(pixels,0xff000000);
        if((status&0x800000)==0)for(int y=0;y<height;y++)for(int x=0;x<width;x++){
            int row=((display[2]+y)&511)*1024,color;
            if((status&0x200000)==0){int value=vram[row+((display[1]+x)&1023)],r=value&31,g=(value>>>5)&31,b=(value>>>10)&31;color=((r<<3)|(r>>>2))<<16|((g<<3)|(g>>>2))<<8|(b<<3)|(b>>>2);}
            else{int at=display[1]*2+x*3;color=readByte(vram,row,at)<<16|readByte(vram,row,at+1)<<8|readByte(vram,row,at+2);}
            pixels[y*width+x]|=color;
        }
        return new Picture(width,height,pixels);
    }
    private static int readByte(int[] vram,int row,int at){return(vram[row+((at>>>1)&1023)]>>>((at&1)*8))&255;}
    private GpuReplay(){}
}
