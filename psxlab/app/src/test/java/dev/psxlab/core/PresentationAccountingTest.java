package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class PresentationAccountingTest {
    @Test public void rejectedPicturesAreNotReportedAsShownOrStoredInGuestSaves()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);
            byte[] before=SaveState.capture(cpu,bus,gpu,"display");
            gpu.recordPresent(0,1,0,3,1,true);
            gpu.recordPresent(3,4,0,6,1,false);
            gpu.recordPresent(6,7,0,9,1,false);
            assertEquals(1,gpu.displays);assertEquals(2,gpu.rejectedDisplays);
            assertArrayEquals(before,SaveState.capture(cpu,bus,gpu,"display"));
            SaveState.restore(before,cpu,bus,gpu,"display");
            gpu.recordPresent(9,10,0,12,1,true);
            assertEquals(2,gpu.displays);assertEquals(2,gpu.rejectedDisplays);
        }
    }
}
