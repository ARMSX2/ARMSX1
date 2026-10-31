package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class GpuBatchOrderTest {
    private static void rect(Gpu g,int color,int x){g.write(0x60000000|color);g.write(x);g.write(0x10001);}
    private static int word(Gpu g){g.write(0xc0000000);g.write(0);g.write(0x10002);return g.read();}
    @Test public void fillAfterPendingDrawPreservesCommandOrder(){try(Gpu g=new Gpu(true)){rect(g,255,0);g.write(0x0200ff00);g.write(0);g.write(0x10010);assertEquals(0x03e003e0,word(g));}}
    @Test public void uploadAndMaskChangesStayOrderedWithinBatches(){try(Gpu g=new Gpu(true)){g.write(0xe4000000|127|(127<<10));rect(g,255,0);g.write(0xa0000000);g.write(0);g.write(0x10001);g.write(0x7c00);g.write(0xe6000001);rect(g,0xff00,1);assertEquals(0x83e07c00,word(g));}}
    @Test public void readbackFlushesPartialPrimitiveBatch(){try(Gpu g=new Gpu(false)){rect(g,255,0);assertEquals(31,word(g)&65535);}}
}
