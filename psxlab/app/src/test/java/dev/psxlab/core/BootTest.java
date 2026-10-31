package dev.psxlab.core;
import org.junit.Test;
import java.io.*;
import java.nio.charset.StandardCharsets;
import static org.junit.Assert.*;

public class BootTest {
    static void word(byte[] b,int p,int v){for(int i=0;i<4;i++)b[p+i]=(byte)(v>>>(8*i));}
    static byte[] executable(){
        byte[] b=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,b,0,8);
        word(b,16,0x80010000);word(b,20,0x80015000);word(b,24,0x80010000);word(b,28,2048);word(b,48,0x801ff000);word(b,52,32);
        word(b,2048,0x2401002a);word(b,2052,0x08004001);return b;
    }
    static int record(byte[] b,int p,String name,int sector,int size,int flags){
        byte[] n=name.getBytes(StandardCharsets.US_ASCII);int len=33+n.length+(n.length%2==0?1:0);b[p]=(byte)len;word(b,p+2,sector);word(b,p+10,size);b[p+25]=(byte)flags;b[p+32]=(byte)n.length;System.arraycopy(n,0,b,p+33,n.length);return len;
    }
    static byte[] iso(){
        byte[] b=new byte[32*2048];int p=16*2048;b[p]=1;System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII),0,b,p+1,5);b[p+6]=1;record(b,p+156,"\0",20,2048,2);
        String cnf="BOOT = cdrom:\\GAME.EXE;1\r\n";p=20*2048;p+=record(b,p,"SYSTEM.CNF;1",21,cnf.length(),0);record(b,p,"GAME.EXE;1",22,4096,0);
        System.arraycopy(cnf.getBytes(StandardCharsets.US_ASCII),0,b,21*2048,cnf.length());System.arraycopy(executable(),0,b,22*2048,4096);return b;
    }
    static ByteSource source(byte[] b){return new ByteSource(){public byte[] read(long offset,int length)throws IOException{if(offset<0 || offset+length>b.length)throw new EOFException();return java.util.Arrays.copyOfRange(b,(int)offset,(int)offset+length);}public void close(){}};}
    @Test public void initializesCpuAndExecutesLoadedCode()throws Exception{
        try(Gpu g=new Gpu(false)){Cpu c=ExeLoader.load(executable(),new Bus(g));assertEquals(0x80015000,c.r[28]);assertEquals(0x801ff020,c.r[29]);c.step();assertEquals(42,c.r[1]);}
    }
    @Test public void isoFindsConfiguredExecutable()throws Exception{IsoDisc disc=new IsoDisc(source(iso()));assertArrayEquals(executable(),disc.bootExecutable());assertEquals("\\GAME.EXE;1",disc.bootPath);}
    @Test public void rawMode2AndChdUnitLayoutsWork()throws Exception{
        byte[] iso=iso();for(int stride:new int[]{2352,2448}){byte[] raw=new byte[32*stride];for(int i=0;i<32;i++)System.arraycopy(iso,i*2048,raw,i*stride+24,2048);assertArrayEquals(executable(),new IsoDisc(source(raw)).bootExecutable());}
    }
    @Test public void rejectsPayloadOutsideRam()throws Exception{byte[] exe=executable();word(exe,24,0x801ffff0);try(Gpu g=new Gpu(false)){assertThrows(IOException.class,()->ExeLoader.load(exe,new Bus(g)));}}
    @Test public void rejectsTruncatedExe()throws Exception{try(Gpu g=new Gpu(false)){assertThrows(IOException.class,()->ExeLoader.load(java.util.Arrays.copyOf(executable(),2050),new Bus(g)));}}
    @Test public void rejectsBrokenIsoRecords()throws Exception{byte[] iso=iso();iso[20*2048]=2;assertThrows(IOException.class,()->new IsoDisc(source(iso)).bootExecutable());}
    @Test public void rejectsOversizedDirectory()throws Exception{byte[] iso=iso();word(iso,16*2048+156+10,1000000000);assertThrows(IOException.class,()->new IsoDisc(source(iso)).bootExecutable());}
}
