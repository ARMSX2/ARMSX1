package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class GpuFailureTest {
    private static void inject(Gpu gpu,RuntimeException failure)throws Exception{java.lang.reflect.Field f=Gpu.class.getDeclaredField("failure");f.setAccessible(true);f.set(gpu,failure);}
    /** The launch used try-with-resources: close() threw the same failure and the JVM replaced it with "Self-suppression not permitted". */
    @Test public void closeDoesNotRethrowAFailureTheCallerAlreadyReceived()throws Exception{
        RuntimeException cause=new IllegalStateException("2x HD drawing is unavailable");
        try{
            try(Gpu gpu=new Gpu(false)){inject(gpu,cause);gpu.read();}
            fail("expected the GPU failure");
        }catch(IllegalStateException e){assertSame(cause,e);assertEquals(0,e.getSuppressed().length);}
    }
    @Test public void closeStillReportsAFailureNobodyHasSeen()throws Exception{
        RuntimeException cause=new IllegalStateException("unseen");
        Gpu gpu=new Gpu(false);inject(gpu,cause);
        try{gpu.close();fail("expected the GPU failure");}catch(IllegalStateException e){assertSame(cause,e);}
    }
}
