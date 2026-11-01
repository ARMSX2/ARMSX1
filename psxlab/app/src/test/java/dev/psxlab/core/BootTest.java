package dev.psxlab.core;
import org.junit.Test;
import java.io.*;
import java.nio.charset.StandardCharsets;
import static org.junit.Assert.*;

public class BootTest {
    @Test public void fastDiscBootReturnsToTheFirmwareInsteadOfExecutingAnInjectedGame()throws Exception{
        byte[] bios=new byte[512*1024];word(bios,0,0x3c1fbfc0);word(bios,4,0x37ff0040);word(bios,8,0x3c088003);word(bios,12,0x01000008);word(bios,16,0);word(bios,0x40,0x2402002a);
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);bus.write32(0x80,0x3c1a0000);Cpu cpu=BiosBoot.discBoot(bios,bus,()->false);
            assertEquals(BiosBoot.SHELL_ENTRY,cpu.pc);assertEquals(0xbfc00040,cpu.r[31]);
            int status=cpu.cop0[12];for(int i=0;i<5;i++)cpu.step();assertEquals(0xbfc00040,cpu.pc);assertEquals(status,cpu.cop0[12]);
            assertEquals(0,gpu.status()&0x800000);assertEquals(0x3c1a0000,bus.read32(0x80));cpu.step();assertEquals(42,cpu.r[2]);
        }
    }
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
    @Test public void fastLoadedGamesCanCallCodeInTheBiosExecutableHeaderBuffer()throws Exception{
        byte[] exe=executable();int headerBuffer=0x8000b070,helper=headerBuffer+0x100;
        // A translated game may call a helper embedded in the nominally unused EXE header.
        word(exe,0x100,0x2402002a);word(exe,0x104,0x03e00008);word(exe,0x108,0);
        word(exe,2048,0x0c000000|((helper>>>2)&0x03ffffff));word(exe,2052,0);
        word(exe,2056,0x24430001);word(exe,2060,0x1000ffff);word(exe,2064,0);
        try(Gpu g=new Gpu(false)){
            Bus bus=new Bus(g,new PadState(),false);Cpu cpu=new Cpu(bus);cpu.cop0[12]=0x401;bus.write32(0x80,0x12345678);
            ExeLoader.loadInto(exe,bus,cpu);
            for(int i=0;i<2048;i++)assertEquals("header byte "+i,exe[i]&255,bus.read8(headerBuffer+i));
            assertEquals(0x401,cpu.cop0[12]);assertEquals(0x12345678,bus.read32(0x80));
            cpu.run(32);assertEquals(42,cpu.r[2]);assertEquals(43,cpu.r[3]);assertEquals(0,cpu.exceptions);
        }
    }
    @Test public void rejectedExecutableDoesNotOverwriteTheBiosHeaderOrKernel()throws Exception{
        byte[] exe=executable();word(exe,28,exe.length);
        try(Gpu g=new Gpu(false)){
            Bus bus=new Bus(g,new PadState(),false);bus.write32(0xb070,0x12345678);
            assertThrows(IOException.class,()->ExeLoader.load(exe,bus));assertEquals(0x12345678,bus.read32(0xb070));
        }
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
