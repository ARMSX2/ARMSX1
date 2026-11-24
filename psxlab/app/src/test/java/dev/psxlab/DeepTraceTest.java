package dev.psxlab;

import dev.psxlab.core.*;
import org.junit.Test;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.Inflater;
import static org.junit.Assert.*;

public class DeepTraceTest {
    @Test public void offWithoutTheFlagFile()throws Exception{
        assertNull(DeepTrace.openIfRequested(Files.createTempDirectory("trace-off").toFile()));
    }
    @Test public void baseAndDeltaRecordsRebuildGuestMemoryAtEachSnapshot()throws Exception{
        File dir=Files.createTempDirectory("trace-on").toFile();assertTrue(new File(dir,DeepTrace.FLAG).createNewFile());
        DeepTrace trace=DeepTrace.openIfRequested(dir);assertNotNull(trace);
        List<byte[]> expected=new ArrayList<>();
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);
            for(int i=0;i<5;i++){
                bus.write32(0x80010000+i*4,0x1000+i);bus.write32(0x801FFFFC,i);cpu.pc=0x80020000+i*4;
                trace.onVm(i*DeepTrace.INTERVAL,cpu,bus,i==3?8:0);
                trace.onVm(i*DeepTrace.INTERVAL+1,cpu,bus,0); // inside the interval: ignored
                expected.add(bus.snapshotRam());
                Thread.sleep(30); // let the writer drain so nothing is dropped
            }
        }
        trace.close();assertEquals(0,trace.dropped());
        ByteBuffer in=ByteBuffer.wrap(Files.readAllBytes(new File(dir,DeepTrace.FILE).toPath())).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic=new byte[8];in.get(magic);assertEquals("ARMSXDT1",new String(magic,"US-ASCII"));
        byte[] ram=null;int n=0;
        while(in.hasRemaining()){
            int type=in.getInt();long field=in.getLong();in.getLong();int pc=in.getInt();for(int r=0;r<32;r++)in.getInt();int pad=in.getInt();
            assertEquals(n*DeepTrace.INTERVAL,field);assertEquals(0x80020000+n*4,pc);assertEquals(n==3?8:0,pad);
            if(type==1){byte[] packed=new byte[in.getInt()];in.get(packed);Inflater inflater=new Inflater();inflater.setInput(packed);ram=new byte[2*1024*1024];assertEquals(ram.length,inflater.inflate(ram));inflater.end();}
            else{assertEquals(2,type);int count=in.getInt();ByteBuffer view=ByteBuffer.wrap(ram).order(ByteOrder.LITTLE_ENDIAN);for(int c=0;c<count;c++){int word=in.getInt();view.putInt(word*4,in.getInt());}assertTrue("deltas carry only changed words",count<=3);}
            assertArrayEquals("snapshot "+n,expected.get(n),ram);n++;
        }
        assertEquals(5,n);
    }
}
