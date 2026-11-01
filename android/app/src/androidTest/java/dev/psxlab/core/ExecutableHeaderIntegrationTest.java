package dev.psxlab.core;

import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import static org.junit.Assert.*;

/** Original fixture models a translation calling into the full BIOS EXE header buffer. */
public class ExecutableHeaderIntegrationTest {
    @Test public void executableEntryInRamMirrorWorksWithBothNativeCpuModes()throws Exception{
        byte[] exe=new byte[4096];System.arraycopy("PS-X EXE".getBytes(java.nio.charset.StandardCharsets.US_ASCII),0,exe,0,8);
        word(exe,16,0x80210000);word(exe,24,0x80210000);word(exe,28,2048);word(exe,2048,0x2402002a);word(exe,2052,0x1000ffff);
        for(boolean recompile:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=ExeLoader.load(exe,bus);assertTrue(cpu.enableNative(recompile,true,true));
            try{cpu.run(128);assertEquals(42,cpu.r[2]);assertEquals(0x2402002a,bus.read32(0x10000));}finally{cpu.closeNative();}
        }
    }
    private static void word(byte[] data,int at,int value){for(int i=0;i<4;i++)data[at+i]=(byte)(value>>>(8*i));}
    @Test public void fastBootPreservesHeaderHelpersWithARealBiosInEveryCpuMode()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        byte[] bios=Files.readAllBytes(new File(context.getExternalFilesDir(null),"fixtures/SCPH1001.BIN").toPath());
        byte[] exe=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,exe,0,8);
        word(exe,16,0x80010000);word(exe,24,0x80010000);word(exe,28,2048);
        word(exe,0x100,0x2402002a);word(exe,0x104,0x03e00008);word(exe,0x108,0);
        word(exe,2048,0x0c000000|(((0x8000b070+0x100)>>>2)&0x03ffffff));word(exe,2052,0);word(exe,2056,0x24430001);word(exe,2060,0x1000ffff);word(exe,2064,0);
        for(int mode=0;mode<3;mode++)try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=BiosBoot.initialize(bios,bus,()->false);
            int kernel=bus.read32(0x80),status=cpu.cop0[12];bus.cdrom.prepareBootHandoff();ExeLoader.loadInto(exe,bus,cpu);
            assertEquals(kernel,bus.read32(0x80));assertEquals(status,cpu.cop0[12]);
            for(int i=0;i<2048;i++)assertEquals("mode="+mode+" header byte="+i,exe[i]&255,bus.read8(0xa000b070+i));
            long exceptions=cpu.exceptions;cpu.cop0[12]&=~1; // Isolate the fixture from a newly due vblank IRQ.
            if(mode!=0)assertTrue(cpu.enableNative(mode==2,true,true));
            try{cpu.run(128);assertEquals(42,cpu.r[2]);assertEquals(43,cpu.r[3]);assertEquals(exceptions,cpu.exceptions);}
            finally{cpu.closeNative();}
        }
    }
}
