package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
import java.util.concurrent.*;
public class AsyncScanoutTest {
    @Test public void queuedScanoutCapturesDisplayRegistersAndPriorDraws()throws Exception{
        try(Gpu g=new Gpu(true)){
            g.control(0x03000000);g.write(0x020000ff);g.write(0);g.write(0x10010);
            CompletableFuture<Gpu.Frame> first=new CompletableFuture<>();g.requestDisplayFrame(first::complete);
            g.control(0x05000010);g.write(0x0200ff00);g.write(16);g.write(0x10010);
            CompletableFuture<Gpu.Frame> second=new CompletableFuture<>();g.requestDisplayFrame(second::complete);
            assertEquals(0xffff0000,first.get(2,TimeUnit.SECONDS).pixels()[0]);
            assertEquals(0xff00ff00,second.get(2,TimeUnit.SECONDS).pixels()[0]);
        }
    }
    @Test public void inlineGpuAlsoDeliversScanout(){try(Gpu g=new Gpu(false)){
        CompletableFuture<Gpu.Frame> frame=new CompletableFuture<>();g.requestDisplayFrame(frame::complete);
        assertTrue(frame.isDone());assertEquals(0xff000000,frame.join().pixels()[0]);
    }}
}
