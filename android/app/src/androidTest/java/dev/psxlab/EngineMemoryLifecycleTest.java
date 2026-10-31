package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Debug;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.file.Files;
import java.util.*;
import org.json.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Repeated private sessions; player settings, cards and save states are untouched. */
@RunWith(AndroidJUnit4.class)
public class EngineMemoryLifecycleTest {
    @Test public void rendererRestartsReleaseWorkersAndReachAMemoryPlateau()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("memoryAudit"));
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        File root=new File(context.getExternalFilesDir(null),"performance/memory-soak-"+System.currentTimeMillis());assertTrue(root.mkdirs());JSONArray measurements=new JSONArray();
        for(int round=0;round<4;round++){
            for(String mode:new String[]{"software","opengl","vulkan"}){
                File session=new File(root,round+"-"+mode);assertTrue(session.mkdirs());File config=new File(session,"settings.toml");
                Files.write(config.toPath(),("[cpu]\nfast_boot=true\n[video]\ngpu_backend=\""+mode+"\"\nrenderer=\""+(mode.equals("software")?"software":"hardware")+"\"\ninternal_scale="+(mode.equals("software")?1:3)+"\n[audio]\nmuted=true\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
                ArmSxBackend backend=new ArmSxBackend(context,value->{},message->{});assertTrue(backend.initialize(session.getAbsolutePath(),fixtures.getAbsolutePath(),config.getAbsolutePath()));backend.selectBios("SCPH1001.BIN");
                try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4)){
                    backend.surface(reader.getSurface());Thread vm=new Thread(()->backend.run(new File(fixtures,"game.chd").getAbsolutePath()),"PSX-Memory-Audit-CPU");vm.start();
                    try{
                        long deadline=SystemClock.uptimeMillis()+1300;while(SystemClock.uptimeMillis()<deadline){try(Image image=reader.acquireLatestImage()){}assertEquals("",backend.lastError());SystemClock.sleep(5);}
                        assertTrue("Frames in "+mode,backend.frames()>0);backend.pause(true);backend.surface(null);backend.surface(reader.getSurface());backend.pause(false);
                        deadline=SystemClock.uptimeMillis()+200;while(SystemClock.uptimeMillis()<deadline){try(Image image=reader.acquireLatestImage()){}SystemClock.sleep(5);}
                    }finally{backend.stop();vm.join(15000);assertFalse("VM teardown "+mode,vm.isAlive());}
                }
                assertEquals("",backend.lastError());
            }
            System.gc();System.runFinalization();System.gc();SystemClock.sleep(250);
            ArrayList<String> remaining=new ArrayList<>();for(Thread thread:Thread.getAllStackTraces().keySet())if(thread.isAlive()&&Set.of("PSX-GPU","PSX-Audio","PSX-DSP","PSX-Memory-Audit-CPU").contains(thread.getName()))remaining.add(thread.getName());
            assertTrue("Leaked session workers: "+remaining,remaining.isEmpty());Debug.MemoryInfo memory=new Debug.MemoryInfo();Debug.getMemoryInfo(memory);
            JSONObject row=new JSONObject().put("round",round).put("pss_kib",memory.getTotalPss()).put("native_heap_bytes",Debug.getNativeHeapAllocatedSize()).put("java_heap_used_bytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory()).put("session_workers",remaining.size());measurements.put(row);
            Files.write(new File(root,"memory.json").toPath(),measurements.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));android.util.Log.i("EngineMemoryLifecycleTest",row.toString());
        }
        // Warm every renderer before assessing growth. A finite soak identifies
        // sustained growth; it cannot prove the absence of every possible leak.
        long growth=measurements.getJSONObject(3).getLong("pss_kib")-measurements.getJSONObject(1).getLong("pss_kib");
        assertTrue("Post-warm-up PSS grew by "+growth+" KiB; inspect "+root,growth<64*1024);
    }
}
