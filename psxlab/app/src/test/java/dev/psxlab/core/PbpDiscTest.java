package dev.psxlab.core;

import org.junit.Test;
import java.io.*;
import java.util.*;
import java.util.zip.Deflater;
import static org.junit.Assert.*;

public class PbpDiscTest {
    private static ByteSource memory(byte[] b){return new ByteSource(){public long length(){return b.length;}public byte[] read(long o,int n)throws IOException{if(o<0||o+n>b.length)throw new EOFException();return Arrays.copyOfRange(b,(int)o,(int)o+n);}public void close(){}};}
    private static void word(byte[] b,int p,int n){BootTest.word(b,p,n);}
    private static void msf(byte[] b,int p,int frames){b[p]=(byte)CdRom.bcd(frames/4500);b[p+1]=(byte)CdRom.bcd(frames/75%60);b[p+2]=(byte)CdRom.bcd(frames%75);}
    private static byte[] raw(int marker,boolean audio){
        int count=audio?34:32;byte[] result=new byte[count*2352],iso=BootTest.iso();word(iso,23*2048,0x24010000|marker);
        for(int i=0;i<32;i++){int at=i*2352;Arrays.fill(result,at+1,at+11,(byte)255);result[at+15]=2;System.arraycopy(iso,i*2048,result,at+24,2048);}
        if(audio)Arrays.fill(result,32*2352,result.length,(byte)0x34);return result;
    }
    private static byte[] compressed(byte[] block){Deflater d=new Deflater(6,true);try{d.setInput(block);d.finish();byte[] scratch=new byte[40000];int n=d.deflate(scratch);assertTrue(d.finished());return n<block.length?Arrays.copyOf(scratch,n):block;}finally{d.end();}}
    private static byte[] body(byte[] raw,boolean zip,boolean audio)throws Exception{
        int blocks=(raw.length+37631)/37632;byte[][] data=new byte[blocks][];int payload=0;
        for(int i=0;i<blocks;i++){byte[] b=new byte[37632];System.arraycopy(raw,i*37632,b,0,Math.min(b.length,raw.length-i*37632));data[i]=zip?compressed(b):b;payload+=data[i].length;}
        byte[] result=new byte[0x100000+payload];System.arraycopy("PSISOIMG0000".getBytes("US-ASCII"),0,result,0,12);word(result,0xbfc,0x100000);
        int toc=0x800;result[toc+2]=(byte)0xa0;result[toc+7]=1;result[toc+12]=(byte)0xa1;result[toc+17]=(byte)(audio?2:1);result[toc+22]=(byte)0xa2;msf(result,toc+27,raw.length/2352+150);
        result[toc+30]=0x41;result[toc+32]=1;msf(result,toc+33,0);msf(result,toc+37,150);
        if(audio){result[toc+40]=1;result[toc+42]=2;msf(result,toc+43,180);msf(result,toc+47,182);}
        int cursor=0;for(int i=0;i<blocks;i++){word(result,0x4000+i*32,cursor);result[0x4000+i*32+4]=(byte)data[i].length;result[0x4000+i*32+5]=(byte)(data[i].length>>>8);System.arraycopy(data[i],0,result,0x100000+cursor,data[i].length);cursor+=data[i].length;}
        return result;
    }
    static byte[] fixture(boolean zip,boolean audio)throws Exception{return container(body(raw(42,audio),zip,audio));}
    private static byte[] container(byte[] body){byte[] result=new byte[0x100+body.length];result[1]='P';result[2]='B';result[3]='P';word(result,4,0x10000);for(int i=0;i<8;i++)word(result,8+i*4,i==7?0x100:40);System.arraycopy(body,0,result,0x100,body.length);return result;}
    @Test public void compressedAndUncompressedPbpExtractAndExecuteTheOriginalFixture()throws Exception{
        for(boolean compressed:new boolean[]{false,true})try(PbpDisc pbp=new PbpDisc(memory(fixture(compressed,false)));Gpu gpu=new Gpu(false)){
            assertEquals(32,pbp.sectorCount());assertEquals(1,pbp.discCount());IsoDisc iso=new IsoDisc((SectorSource)pbp);assertArrayEquals(BootTest.executable(),iso.bootExecutable());Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=ExeLoader.load(iso.bootExecutable(),bus);cpu.step();assertEquals(42,cpu.r[1]);
            assertArrayEquals(Arrays.copyOfRange(raw(42,false),15*2352,17*2352),pbp.read(15L*2352,2*2352));
        }
    }
    @Test public void audioTrackMetadataAndPartialLastBlockRemainReadable()throws Exception{
        try(PbpDisc pbp=new PbpDisc(memory(fixture(true,true)))){
            assertEquals(34,pbp.sectorCount());assertEquals(2,pbp.lastTrack());assertEquals(32,pbp.trackStart(2));assertEquals(2,pbp.trackNumber(30));assertEquals(0,pbp.trackIndex(30));assertEquals(2,pbp.trackRelative(30));assertTrue(pbp.audioTrack(32));assertEquals(1,pbp.trackIndex(32));assertEquals(0x34,pbp.readSector(33)[100]);assertArrayEquals(BootTest.executable(),new IsoDisc((SectorSource)pbp).bootExecutable());
            byte[] copy=pbp.readSector(0);copy[1]=0;assertEquals(255,pbp.readSector(0)[1]&255);assertThrows(EOFException.class,()->pbp.readSector(34));
        }
    }
    @Test public void multiDiscOffsetsAreRelativeToThePsarAndSelectTheRequestedDisc()throws Exception{
        byte[] first=body(raw(42,false),true,false),second=body(raw(43,false),false,false);int secondAt=(0x1000+first.length+0xfff)&~0xfff;
        byte[] title=new byte[secondAt+second.length];System.arraycopy("PSTITLEIMG000000".getBytes("US-ASCII"),0,title,0,16);word(title,0x200,0x1000);word(title,0x204,secondAt);System.arraycopy(first,0,title,0x1000,first.length);System.arraycopy(second,0,title,secondAt,second.length);
        byte[] pbp=container(title);for(int i=0;i<2;i++)try(PbpDisc disc=new PbpDisc(memory(pbp),i)){assertEquals(2,disc.discCount());byte[] exe=new IsoDisc((SectorSource)disc).bootExecutable();assertEquals(42+i,exe[2048]&255);}assertThrows(IOException.class,()->new PbpDisc(memory(pbp),2));
    }
    @Test public void encryptedPspAndInvalidOffsetsGiveSpecificErrors()throws Exception{
        byte[] encrypted=fixture(true,false);System.arraycopy(new byte[]{0,'P','G','D'},0,encrypted,0x100+0x400,4);assertTrue(assertThrows(IOException.class,()->new PbpDisc(memory(encrypted))).getMessage().contains("Encrypted"));
        byte[] psp=new byte[512];psp[1]='P';psp[2]='B';psp[3]='P';word(psp,36,256);assertTrue(assertThrows(IOException.class,()->new PbpDisc(memory(psp))).getMessage().contains("PSP"));
        byte[] bad=fixture(true,false);word(bad,36,Integer.MAX_VALUE);assertThrows(IOException.class,()->new PbpDisc(memory(bad)));
        byte[] truncated=Arrays.copyOf(fixture(true,false),1000);assertThrows(IOException.class,()->new PbpDisc(memory(truncated)));
        byte[] missing=fixture(true,false);missing[0x100+0x4000+4]=0;missing[0x100+0x4000+5]=0;assertThrows(IOException.class,()->new PbpDisc(memory(missing)));
    }
    @Test public void malformedDeflateFailsWithoutReturningAStaleCachedBlock()throws Exception{
        byte[] bad=fixture(true,false);Arrays.fill(bad,0x100+0x100000,bad.length,(byte)255);try(PbpDisc disc=new PbpDisc(memory(bad))){assertThrows(IOException.class,()->disc.readSector(0));assertThrows(IOException.class,()->disc.readSector(0));}
    }
    @Test public void ownsTheSourceOnFailureAndClosesOnce()throws Exception{
        int[] closed={0};ByteSource source=new ByteSource(){public long length(){return 100;}public byte[] read(long at,int n){return new byte[n];}public void close(){closed[0]++;}};
        assertThrows(IOException.class,()->new PbpDisc(source));assertEquals(1,closed[0]);PbpDisc disc=new PbpDisc(memory(fixture(true,false)));disc.close();disc.close();assertThrows(IOException.class,()->disc.readSector(0));
    }
}
