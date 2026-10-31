package dev.psxlab;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class AudioLifecycleAuditTest {
    @Test public void diagnosticsStayReadableWhileAudioCloses()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("audioLifecycle"));
        for(int trial=0;trial<8;trial++){
            AudioOutput output=new AudioOutput(true);AtomicBoolean reading=new AtomicBoolean(true);AtomicReference<Throwable> error=new AtomicReference<>();
            Thread reader=new Thread(()->{try{while(reading.get()){output.diagnostic();output.snapshotDiagnostic();output.loggingSnapshot();Thread.yield();}}catch(Throwable t){error.set(t);}},"audio-diagnostic-lifecycle-test");
            reader.start();try{output.diagnostic();output.close();for(int i=0;i<20;i++){assertTrue(output.diagnostic().contains("underruns"));assertEquals(0,output.bufferedFrames());output.snapshotDiagnostic();output.loggingSnapshot();}}
            finally{reading.set(false);reader.join(5000);output.close();}
            assertFalse("Diagnostic reader shutdown",reader.isAlive());if(error.get()!=null)throw new AssertionError("Audio diagnostic/release race",error.get());
        }
    }
}
