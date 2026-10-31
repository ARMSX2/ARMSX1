package dev.psxlab;

import android.content.Context;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.core.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Original synthetic ISO/BIN fixtures exercise the actual adapter mount path on Android. */
@RunWith(AndroidJUnit4.class)
public class CueLaunchIntegrationTest {
    private static void word(byte[] b,int p,int value){for(int i=0;i<4;i++)b[p+i]=(byte)(value>>>(i*8));}
    private static int record(byte[] b,int p,String name,int sector,int size,int flags){byte[] n=name.getBytes(StandardCharsets.US_ASCII);int length=33+n.length+(n.length%2==0?1:0);b[p]=(byte)length;word(b,p+2,sector);word(b,p+10,size);b[p+25]=(byte)flags;b[p+32]=(byte)n.length;System.arraycopy(n,0,b,p+33,n.length);return length;}
    private static byte[] exe(){byte[] b=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,b,0,8);word(b,16,0x80010000);word(b,24,0x80010000);word(b,28,2048);word(b,2048,0x2401002a);word(b,2052,0x08004001);return b;}
    private static byte[] iso(){
        byte[] b=new byte[32*2048];int p=16*2048;b[p]=1;System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII),0,b,p+1,5);b[p+6]=1;record(b,p+156,"\0",20,2048,2);
        String cnf="BOOT = cdrom:\\SLUS_999.99;1\r\n";p=20*2048;p+=record(b,p,"SYSTEM.CNF;1",21,cnf.length(),0);record(b,p,"SLUS_999.99;1",22,4096,0);System.arraycopy(cnf.getBytes(StandardCharsets.US_ASCII),0,b,21*2048,cnf.length());System.arraycopy(exe(),0,b,22*2048,4096);return b;
    }
    private static byte[] pbp(byte[] raw)throws Exception{
        byte[][] parts=new byte[2][];int total=0;for(int i=0;i<2;i++){java.util.zip.Deflater d=new java.util.zip.Deflater(6,true);try{d.setInput(raw,i*37632,37632);d.finish();byte[] out=new byte[40000];int n=d.deflate(out);assertTrue(d.finished());parts[i]=java.util.Arrays.copyOf(out,n);total+=n;}finally{d.end();}}
        int base=0x100;byte[] result=new byte[base+0x100000+total];result[1]='P';result[2]='B';result[3]='P';word(result,36,base);System.arraycopy("PSISOIMG0000".getBytes(StandardCharsets.US_ASCII),0,result,base,12);word(result,base+0xbfc,0x100000);
        int toc=base+0x800;result[toc+2]=(byte)0xa0;result[toc+7]=1;result[toc+12]=(byte)0xa1;result[toc+17]=1;result[toc+22]=(byte)0xa2;result[toc+28]=2;result[toc+29]=0x32;result[toc+30]=0x41;result[toc+32]=1;result[toc+38]=2;
        int cursor=0;for(int i=0;i<2;i++){word(result,base+0x4000+i*32,cursor);result[base+0x4000+i*32+4]=(byte)parts[i].length;result[base+0x4000+i*32+5]=(byte)(parts[i].length>>>8);System.arraycopy(parts[i],0,result,base+0x100000+cursor,parts[i].length);cursor+=parts[i].length;}return result;
    }
    private static IsoDisc disc(Object media)throws Exception{Field field=media.getClass().getDeclaredField("disc");field.setAccessible(true);return (IsoDisc)field.get(media);}
    @Test public void stagedCueFollowsBinsBootsExecutableAndClosesEveryTrack()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("cueAudit"));
        Context c=InstrumentationRegistry.getInstrumentation().getTargetContext();File dir=new File(c.getCacheDir(),"cue-audit-"+System.nanoTime());assertTrue(dir.mkdirs());
        byte[] cooked=iso(),raw=new byte[32*2352];for(int i=0;i<32;i++){raw[i*2352+15]=2;System.arraycopy(cooked,i*2048,raw,i*2352+24,2048);}
        File bin=new File(dir,"track-00.bin"),audio1=new File(dir,"track-01.bin"),audio2=new File(dir,"track-02.bin"),cue=new File(dir,"game.cue");
        Files.write(bin.toPath(),raw);Files.write(audio1.toPath(),new byte[2352*153]);Files.write(audio2.toPath(),new byte[2352*2]);
        Files.write(cue.toPath(),("FILE \"track-00.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\nFILE \"track-01.bin\" BINARY\nTRACK 02 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:02:00\nFILE \"track-02.bin\" BINARY\nTRACK 03 AUDIO\nINDEX 01 00:00:00\n").getBytes(StandardCharsets.UTF_8));
        ArmSxBackend backend=new ArmSxBackend(c,p->{},m->{},new File(dir,"logs"));Method mount=ArmSxBackend.class.getDeclaredMethod("openMedia",String.class);mount.setAccessible(true);
        int before=new File("/proc/self/fd").list().length;
        try{
            for(int trial=0;trial<12;trial++)try(AutoCloseable media=(AutoCloseable)mount.invoke(backend,trial%2==0?cue.getAbsolutePath():cue.toURI().toString())){
                IsoDisc d=disc(media);assertEquals(3,d.lastTrack());assertEquals(182,d.trackStart(2));assertEquals(185,d.trackStart(3));assertEquals(187,d.sectorCount());assertEquals('A',d.region());
                Field payload=media.getClass().getDeclaredField("executable");payload.setAccessible(true);assertArrayEquals(exe(),(byte[])payload.get(media));
                try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=ExeLoader.load((byte[])payload.get(media),bus);cpu.step();assertEquals(42,cpu.r[1]);bus.cdrom.insert(d);assertEquals(2352,d.readSector(182).length);}
            }
            int after=new File("/proc/self/fd").list().length;assertTrue("Track descriptors leaked: before="+before+" after="+after,after<=before+8);
            assertTrue(audio1.delete());
            InvocationTargetException error=assertThrows(InvocationTargetException.class,()->mount.invoke(backend,cue.getAbsolutePath()));assertTrue(error.getCause() instanceof FileNotFoundException);assertTrue(error.getCause().getMessage().contains("track-01.bin"));
            // Other mount types continue through their original raw-data path.
            try(AutoCloseable media=(AutoCloseable)mount.invoke(backend,bin.getAbsolutePath())){assertEquals(1,disc(media).lastTrack());assertArrayEquals(exe(),disc(media).bootExecutable());}
            File eboot=new File(dir,"EBOOT.PBP");Files.write(eboot.toPath(),pbp(raw));
            try(AutoCloseable media=(AutoCloseable)mount.invoke(backend,eboot.getAbsolutePath())){assertArrayEquals(exe(),disc(media).bootExecutable());assertEquals(1,disc(media).lastTrack());}
            File invalid=new File(dir,"bad.cue");Files.write(invalid.toPath(),"FILE missing.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00".getBytes(StandardCharsets.UTF_8));
            InvocationTargetException unsupported=assertThrows(InvocationTargetException.class,()->mount.invoke(backend,invalid.getAbsolutePath()));assertTrue(unsupported.getCause().getMessage().contains("Unsupported CUE file type"));assertTrue(invalid.delete());
        }finally{for(File f:dir.listFiles())if(f.isFile())assertTrue(f.delete());assertTrue(dir.delete());}
    }
}
