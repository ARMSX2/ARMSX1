package dev.psxlab.core;

import java.lang.reflect.Field;
import java.util.Arrays;
import org.junit.Test;
import static org.junit.Assert.*;

public class DisplayCropTest {
    @Test public void nonePreservesEveryPixel(){
        int[] s={0x20000,0,4,615,3175,16,256,1},before=s.clone();
        assertSame(s,DisplayCrop.apply(s,0));assertArrayEquals(before,s);
    }
    @Test public void smallAndFullUseNativeDimensions(){
        assertArrayEquals(new int[]{0x20000,10,11,695,3095,23,249,1},DisplayCrop.apply(new int[]{0x20000,0,4,615,3175,16,256,1},1));
        assertArrayEquals(new int[]{0x20000,20,19,775,3015,31,241,1},DisplayCrop.apply(new int[]{0x20000,0,4,615,3175,16,256,1},2));
    }
    @Test public void packedMoviesRetainTheirRedBytePhaseAndWrapVram(){
        // 368px / 32 is odd: use a 10px inset, not an 11px halfword truncation.
        int[] s=DisplayCrop.apply(new int[]{0x210000,1016,510,0,2576,20,308,0},1);
        assertEquals(7,s[1]);assertEquals(7,s[2]);assertEquals(348,(s[4]-s[3])/7);assertEquals(270,s[6]-s[5]);
    }
    @Test public void interlacedCropAdvancesBothRowsPerScanline(){
        int[] s=DisplayCrop.apply(new int[]{0x4a0000,0,508,0,2560,16,256,0},1);
        assertEquals(10,s[1]);assertEquals(10,s[2]);assertEquals(452,(s[6]-s[5])*2);
    }
    @Test public void tinyDiagnosticFramesAreNotCropped(){
        int[] s={0,0,0,0,80,0,8,0},before=s.clone();DisplayCrop.apply(s,2);assertArrayEquals(before,s);
    }
    @Test public void softwareCropRemovesStaleBorderWithoutChangingGuestVram()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Field f=Gpu.class.getDeclaredField("vram");f.setAccessible(true);int[] ram=(int[])f.get(gpu);
            Arrays.fill(ram,0x3e0);Arrays.fill(ram,240*1024,244*1024,0x7c1f);int[] original=ram.clone();
            gpu.control(0x08000001);gpu.control(0x05001000);gpu.control(0x06000000|615|(3175<<12));gpu.control(0x07000000|16|(256<<10));gpu.control(0x03000000);
            Gpu.Frame full=gpu.displayFrame();assertEquals(320,full.width());assertEquals(240,full.height());assertEquals(0xffff00ff,full.pixels()[full.pixels().length-1]);
            gpu.configureCrop(1);Gpu.Frame crop=gpu.displayFrame();assertEquals(300,crop.width());assertEquals(226,crop.height());
            for(int p:crop.pixels())assertEquals(0xff00ff00,p);assertArrayEquals(original,ram);
            gpu.configureCrop(0);assertArrayEquals(full.pixels(),gpu.displayFrame().pixels());
        }
    }
    @Test public void loadingStateKeepsTheHostsCropSelection()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);byte[] state=SaveState.capture(cpu,bus,gpu,"crop");
            gpu.configureCrop(1);SaveState.restore(state,cpu,bus,gpu,"crop");
            assertEquals(240,gpu.displayFrame().width());assertEquals(226,gpu.displayFrame().height());
        }
    }
}
