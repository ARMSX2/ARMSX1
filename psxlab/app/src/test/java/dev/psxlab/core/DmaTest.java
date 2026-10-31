package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class DmaTest {
    @Test public void otcBuildsReverseOrderingTableAndRaisesIrq(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010f0,0x0f654321);b.write32(0x1f8010f4,0x00c00000);b.write16(0x1f801074,8);
            b.write32(0x1f8010e0,0x100);b.write32(0x1f8010e4,3);b.write32(0x1f8010e8,0x11000002);
            assertEquals(0xfc,b.read32(0x100));assertEquals(0xf8,b.read32(0xfc));assertEquals(0xffffff,b.read32(0xf8));
            assertEquals(0x100,b.read32(0x1f8010e0));assertEquals(0,b.read32(0x1f8010e8)&0x11000000);assertTrue(b.interruptPending());
            assertEquals(0xc0000000,b.read32(0x1f8010f4)&0xc0000000);
            b.write32(0x1f8010f4,0x40c00000);assertEquals(0,b.read32(0x1f8010f4)&0xc0000000);
        }
    }
    @Test public void gpuLinkedListUsesCommandOrderAndStopsAtMarker(){
        for(boolean threaded:new boolean[]{false,true})try(Gpu g=new Gpu(threaded)){
            Bus b=new Bus(g);b.write32(0x100,0x03000200);b.write32(0x104,0xa0000000);b.write32(0x108,0);b.write32(0x10c,0x00010002);
            b.write32(0x200,0x01ffffff);b.write32(0x204,0x1234abcd);
            b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a8,0x01000401);
            g.write(0xc0000000);g.write(0);g.write(0x00010002);assertEquals(0x1234abcd,g.read());assertEquals(0xffffff,b.read32(0x1f8010a0));
        }
    }
    @Test public void blockDmaTransfersFromGpuToRam(){
        try(Gpu g=new Gpu(true)){
            Bus b=new Bus(g);g.write(0xa0000000);g.write(0);g.write(0x00010002);g.write(0x7c00001f);g.write(0xc0000000);g.write(0);g.write(0x00010002);
            b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a4,0x00010001);b.write32(0x1f8010a8,0x01000200);
            assertEquals(0x7c00001f,b.read32(0x100));assertEquals(0x104,b.read32(0x1f8010a0));
        }
    }
    @Test public void disabledDmaWaitsForMasterEnable(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010e0,0x100);b.write32(0x1f8010e4,1);b.write32(0x1f8010e8,0x11000002);assertEquals(0,b.read32(0x100));b.write32(0x1f8010f0,0x0f654321);assertEquals(0xffffff,b.read32(0x100));
        }
    }
    @Test public void cyclicListFailsExplicitly(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x100,0x100);b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);assertThrows(IllegalStateException.class,()->b.write32(0x1f8010a8,0x01000401));}
    }
}
