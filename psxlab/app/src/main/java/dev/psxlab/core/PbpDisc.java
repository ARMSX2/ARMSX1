package dev.psxlab.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;

/** Unencrypted PS1 PSISOIMG PBP reader, with a bounded one-block decompression cache. */
public final class PbpDisc implements ByteSource,SectorSource {
    private static final int SECTOR=2352,BLOCK_SECTORS=16,BLOCK_BYTES=SECTOR*BLOCK_SECTORS,MAX_BLOCKS=32256;
    private final ByteSource file;
    private final Inflater inflater=new Inflater(true);
    private final byte[] cache=new byte[BLOCK_BYTES];
    private long[] offsets;private int[] sizes,starts,pregaps;private boolean[] audio;
    private int sectors,discCount,cachedBlock=-1;private boolean closed;
    private ByteSource raw;
    public PbpDisc(ByteSource file)throws IOException{this(file,0);}
    /** Disc index is zero-based; ordinary launches select the first embedded disc. */
    public PbpDisc(ByteSource file,int discIndex)throws IOException{
        this.file=file;try{open(discIndex);raw=CueDisc.rawBytes(this);}catch(IOException|RuntimeException e){try{close();}catch(IOException closing){e.addSuppressed(closing);}throw e;}
    }
    public static boolean hasMagic(byte[] bytes){return bytes.length>=4&&bytes[0]==0&&bytes[1]=='P'&&bytes[2]=='B'&&bytes[3]=='P';}
    private static long u32(byte[] b,int at){return Integer.toUnsignedLong((b[at]&255)|((b[at+1]&255)<<8)|((b[at+2]&255)<<16)|(b[at+3]<<24));}
    private static int u16(byte[] b,int at){return (b[at]&255)|((b[at+1]&255)<<8);}
    private static int bcd(byte b)throws IOException{int value=b&255;if((value&15)>9||(value>>>4)>9)throw new IOException("Invalid BCD in PBP track table");return (value>>>4)*10+(value&15);}
    private static int time(byte[] b,int at)throws IOException{int m=bcd(b[at]),s=bcd(b[at+1]),f=bcd(b[at+2]);if(s>=60||f>=75)throw new IOException("Invalid PBP track time");return m*4500+s*75+f;}
    private static boolean starts(byte[] bytes,String text){byte[] signature=text.getBytes(StandardCharsets.US_ASCII);if(bytes.length<signature.length)return false;for(int i=0;i<signature.length;i++)if(bytes[i]!=signature[i])return false;return true;}
    private byte[] readFile(long at,int size)throws IOException{long length=file.length();if(at<0||size<0||at>length-size)throw new EOFException("PBP section extends past the file");byte[] data=file.read(at,size);if(data.length!=size)throw new EOFException("Truncated PBP section");return data;}
    private void open(int discIndex)throws IOException{
        byte[] header=readFile(0,40);if(!hasMagic(header))throw new IOException("Not a PBP container");long psar=u32(header,36);if(psar<40)throw new IOException("Invalid PBP disc section offset");
        byte[] signature=readFile(psar,16);List<Long> discs=new ArrayList<>();
        if(starts(signature,"PSTITLEIMG000000")){
            byte[] table=readFile(psar+0x200,20);if(starts(table,"\0PGD"))throw new IOException("Encrypted PSN PBP images are not supported; use an unencrypted PS1 disc image");
            for(int i=0;i<5;i++){long offset=u32(table,i*4);if(offset==0)break;discs.add(psar+offset);}
            if(discs.isEmpty())throw new IOException("PBP contains no discs");
        }else if(starts(signature,"PSISOIMG0000"))discs.add(psar);
        else throw new IOException("PBP does not contain an unencrypted PS1 PSISOIMG disc; PSP applications are not PlayStation discs");
        discCount=discs.size();if(discIndex<0||discIndex>=discCount)throw new IOException("PBP disc index is out of range");long base=discs.get(discIndex);
        if(!starts(readFile(base,12),"PSISOIMG0000"))throw new IOException("Invalid PBP PS1 disc header");
        if(starts(readFile(base+0x400,4),"\0PGD"))throw new IOException("Encrypted PSN PBP images are not supported; use an unencrypted PS1 disc image");
        byte[] toc=readFile(base+0x800,1020);if((toc[2]&255)!=0xa0||(toc[12]&255)!=0xa1||(toc[22]&255)!=0xa2)throw new IOException("Invalid PBP track table");
        int first=bcd(toc[7]),last=bcd(toc[17]);if(first!=1||last<1||last>99)throw new IOException("Invalid PBP track range");
        starts=new int[last];pregaps=new int[last];audio=new boolean[last];int origin=time(toc,37),leadout=time(toc,27);
        sectors=leadout-origin;if(sectors<=0||sectors>MAX_BLOCKS*BLOCK_SECTORS)throw new IOException("Invalid PBP disc length");
        for(int i=0;i<last;i++){
            int at=(i+3)*10;if(bcd(toc[at+2])!=i+1)throw new IOException("Invalid PBP track numbering");
            starts[i]=time(toc,at+7)-origin;pregaps[i]=time(toc,at+3)-origin;audio[i]=(toc[at]&255)==1;
            if(starts[i]<0||starts[i]>=sectors||(i>0&&(pregaps[i]<starts[i-1]||starts[i]<pregaps[i])))throw new IOException("Invalid PBP track position");
        }
        long data=base+u32(readFile(base+0xbfc,4),0);int blocks=(sectors+BLOCK_SECTORS-1)/BLOCK_SECTORS;
        long tableEnd=base+0x4000+(long)blocks*32;if(data<tableEnd||data>file.length())throw new IOException("Invalid PBP compressed-data offset");
        byte[] table=readFile(base+0x4000,blocks*32);offsets=new long[blocks];sizes=new int[blocks];
        for(int i=0;i<blocks;i++){
            offsets[i]=data+u32(table,i*32);sizes[i]=u16(table,i*32+4);
            // Some conversions leave unused TOC-tail blocks absent. Keep the TOC
            // intact and reject a missing block only if the game actually reads it.
            if(sizes[i]==0&&i!=0)continue;
            if(sizes[i]==0||offsets[i]<data||offsets[i]>file.length()-sizes[i])throw new IOException("Invalid or missing PBP data block "+i);
        }
    }
    private void block(int index)throws IOException{
        if(cachedBlock==index)return;cachedBlock=-1;if(sizes[index]==0)throw new IOException("PBP has no stored data for block "+index+" (sectors "+index*BLOCK_SECTORS+"-"+(Math.min(sectors,(index+1)*BLOCK_SECTORS)-1)+")");byte[] compressed=readFile(offsets[index],sizes[index]);int expected=Math.min(BLOCK_SECTORS,sectors-index*BLOCK_SECTORS)*SECTOR;
        Arrays.fill(cache,(byte)0);
        if(compressed.length==BLOCK_BYTES){System.arraycopy(compressed,0,cache,0,compressed.length);}
        else{
            inflater.reset();inflater.setInput(compressed);int used=0;
            try{
                while(used<cache.length&&!inflater.finished()){
                    int n=inflater.inflate(cache,used,cache.length-used);used+=n;
                    if(n==0&&!inflater.finished())throw new IOException("Incomplete compressed PBP block "+index);
                }
                if(!inflater.finished()){byte[] extra=new byte[1];if(inflater.inflate(extra)>0||!inflater.finished())throw new IOException("Oversized compressed PBP block "+index);}
                if(used<expected)throw new IOException("Truncated decoded PBP block "+index);
            }catch(DataFormatException e){throw new IOException("Invalid compressed PBP block "+index,e);}
        }
        cachedBlock=index;
    }
    public int discCount(){return discCount;}
    @Override public int sectorCount(){return sectors;}
    @Override public int lastTrack(){return starts.length;}
    @Override public int trackStart(int track){if(track==0)return sectors;if(track<1||track>starts.length)throw new IllegalArgumentException("No such PBP track");return starts[track-1];}
    private int track(int lba){for(int i=starts.length-1;i>=0;i--)if(lba>=pregaps[i])return i;return 0;}
    @Override public int trackNumber(int lba){return track(lba)+1;}
    @Override public int trackIndex(int lba){return lba<starts[track(lba)]?0:1;}
    @Override public int trackRelative(int lba){return Math.abs(lba-starts[track(lba)]);}
    @Override public boolean audioTrack(int lba){return audio[track(lba)];}
    @Override public synchronized byte[] readSector(int lba)throws IOException{
        if(closed)throw new IOException("PBP disc is closed");if(lba<0||lba>=sectors)throw new EOFException("Sector outside PBP disc");block(lba/BLOCK_SECTORS);int offset=lba%BLOCK_SECTORS*SECTOR;return Arrays.copyOfRange(cache,offset,offset+SECTOR);
    }
    @Override public long length(){return (long)sectors*SECTOR;}
    @Override public byte[] read(long offset,int length)throws IOException{return raw.read(offset,length);}
    @Override public synchronized void close()throws IOException{if(!closed){closed=true;inflater.end();file.close();}}
}
