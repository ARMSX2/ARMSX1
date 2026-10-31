package dev.psxlab.core;

import android.app.Instrumentation;
import android.content.*;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import android.graphics.Bitmap;
import android.view.PixelCopy;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.EngineAuditActivity;
import dev.psxlab.ArmSxBackend;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Explicit A/B measurements; real BIOS/game fixtures stay on the test device only. */
@RunWith(AndroidJUnit4.class)
public class EnginePerformanceAuditTest {
    private static String read(File file)throws IOException{return new String(Files.readAllBytes(file.toPath()),StandardCharsets.UTF_8);}
    private static void write(File file,String text)throws IOException{Files.write(file.toPath(),text.getBytes(StandardCharsets.UTF_8));}
    private EngineAuditActivity screen(Instrumentation instrumentation)throws Exception{
        Context c=instrumentation.getTargetContext();assertEquals("com.nanodata.armsx",c.getPackageName());
        assertEquals("Explicit performance test required","true",InstrumentationRegistry.getArguments().getString("performanceAudit"));
        EngineAuditActivity activity=(EngineAuditActivity)instrumentation.startActivitySync(new Intent(c,EngineAuditActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        long end=SystemClock.uptimeMillis()+10000;
        while((activity.gameSurface()==null||!activity.gameSurface().isValid()||!activity.hasWindowFocus())&&SystemClock.uptimeMillis()<end)SystemClock.sleep(25);
        assertNotNull(activity.gameSurface());assertTrue(activity.hasWindowFocus());return activity;
    }
    private static JSONObject threads()throws Exception{
        JSONObject result=new JSONObject();File[] tasks=new File("/proc/self/task").listFiles();assertNotNull(tasks);
        for(File task:tasks)try{
            String name=read(new File(task,"comm")).trim();
            if(!name.startsWith("PSX-"))continue;
            String stat=read(new File(task,"stat"));String[] fields=stat.substring(stat.lastIndexOf(')')+2).split(" ");
            result.put(name,result.optLong(name)+Long.parseLong(fields[11])+Long.parseLong(fields[12]));
        }catch(IOException ignored){}
        return result;
    }
    @Test public void gameScene()throws Exception{
        Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();Context c=instrumentation.getTargetContext();
        String renderer=InstrumentationRegistry.getArguments().getString("renderer","hardware");int scale=Integer.parseInt(InstrumentationRegistry.getArguments().getString("scale","1"));
        EngineAuditActivity activity=screen(instrumentation);File fixtures=new File(c.getExternalFilesDir(null),"fixtures");File bios=new File(fixtures,"SCPH1001.BIN"),game=new File(fixtures,"game.chd");
        assertEquals(524288,bios.length());assertTrue(game.length()>1000000);
        File dir=new File(c.getExternalFilesDir(null),"performance/"+renderer+"-"+scale+"-"+System.currentTimeMillis());assertTrue(dir.mkdirs());
        File config=new File(dir,"settings.toml");write(config,"[cpu]\nfast_boot=false\n[video]\nrenderer=\""+renderer+"\"\ninternal_scale="+scale+"\n");
        ArmSxBackend backend=new ArmSxBackend(c,p->{},message->android.util.Log.e("PSXPerf",message));assertTrue(backend.initialize(dir.getAbsolutePath(),fixtures.getAbsolutePath(),config.getAbsolutePath()));backend.selectBios(bios.getName());backend.surface(activity.gameSurface());backend.traceStartup(true);
        Thread vm=new Thread(()->backend.run(game.getAbsolutePath()),"PSX-Perf-CPU");long started=SystemClock.elapsedRealtimeNanos();vm.start();
        JSONObject begin=null,end=null;long sampleStarted=0,sampleEnded=0,lastOsd=0;String report="";
        try{
            while(SystemClock.elapsedRealtimeNanos()-started<30000000000L){
                assertTrue("Audit left the foreground",activity.hasWindowFocus());assertTrue("Core error: "+backend.lastError(),backend.lastError().isEmpty());
                if(SystemClock.elapsedRealtimeNanos()-lastOsd>500000000L){activity.showFps(backend.fps());lastOsd=SystemClock.elapsedRealtimeNanos();}
                if(begin==null&&SystemClock.elapsedRealtimeNanos()-started>=10000000000L){begin=threads();sampleStarted=SystemClock.elapsedRealtimeNanos();}
                SystemClock.sleep(100);
            }
            sampleEnded=SystemClock.elapsedRealtimeNanos();end=threads();
            java.lang.reflect.Field graphics=ArmSxBackend.class.getDeclaredField("gpu");graphics.setAccessible(true);
            report=new JSONObject(backend.diagnosticReport()).put("gpuWork",((Gpu)graphics.get(backend)).renderStatistics()).toString(2);
        }finally{backend.stop();vm.join(10000);assertFalse("VM shutdown",vm.isAlive());}
        write(new File(dir,"gpu-ready.tsv"),backend.startupTiming());write(new File(dir,"report.json"),report);
        JSONObject profile=new JSONObject().put("beforeTicks",begin).put("afterTicks",end).put("wallNanos",sampleEnded-sampleStarted).put("ticksPerSecond",Os.sysconf(OsConstants._SC_CLK_TCK));
        write(new File(dir,"threads.json"),profile.toString(2));assertTrue(backend.frames()>800);assertTrue(backend.audioNonzero()>100);assertTrue(backend.lastError(),backend.lastError().isEmpty());
        android.util.Log.i("PSXPerf","Saved "+dir);
    }
    /** Observe the real Compose host, with its normal input and display lifecycle. */
    @Test public void normalGameScene()throws Exception{
        Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();Context c=instrumentation.getTargetContext();
        assertEquals("true",InstrumentationRegistry.getArguments().getString("performanceAudit"));
        android.content.SharedPreferences prefs=c.getSharedPreferences("ARMSX2",Context.MODE_PRIVATE);
        assertTrue(prefs.edit().putString("ui.osdMode","Min").commit());
        String path=InstrumentationRegistry.getArguments().getString("game","/sdcard/ROMs/psx/Crash Bandicoot - Warped (USA).chd");
        instrumentation.startActivitySync(new Intent(Intent.ACTION_VIEW,android.net.Uri.fromFile(new File(path))).setClassName(c,"com.armsx2.Main").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        java.lang.reflect.Field field=kr.co.iefriends.pcsx2.NativeApp.class.getDeclaredField("lab");field.setAccessible(true);ArmSxBackend backend=null;
        long waitUntil=SystemClock.uptimeMillis()+60000;
        while(SystemClock.uptimeMillis()<waitUntil){backend=(ArmSxBackend)field.get(null);if(backend!=null&&backend.active()&&backend.frames()>30)break;SystemClock.sleep(100);}
        assertNotNull(backend);assertTrue("Normal game did not start",backend.active());backend.traceStartup(true);
        File dir=new File(c.getExternalFilesDir(null),"performance/normal-"+System.currentTimeMillis());assertTrue(dir.mkdirs());
        java.lang.reflect.Field traceField=ArmSxBackend.class.getDeclaredField("startupTiming");traceField.setAccessible(true);StartupTiming trace=(StartupTiming)traceField.get(backend);
        JSONArray samples=new JSONArray();long started=SystemClock.elapsedRealtimeNanos(),lastChunk=started;int chunk=0;
        int seconds=Integer.parseInt(InstrumentationRegistry.getArguments().getString("seconds","90"));
        while(SystemClock.elapsedRealtimeNanos()-started<seconds*1000000000L){
            samples.put(new JSONObject().put("ns",System.nanoTime()).put("frames",backend.frames()).put("fps",backend.fps()).put("active",backend.active()));
            if(SystemClock.elapsedRealtimeNanos()-lastChunk>15000000000L){write(new File(dir,"gpu-ready-"+(chunk++)+".tsv"),trace.tsv());trace.reset();write(new File(dir,"rates.json"),samples.toString(2));write(new File(dir,"report.json"),backend.diagnosticReport());lastChunk=SystemClock.elapsedRealtimeNanos();}
            SystemClock.sleep(100);
        }
        write(new File(dir,"rates.json"),samples.toString(2));write(new File(dir,"gpu-ready.tsv"),backend.startupTiming());write(new File(dir,"report.json"),backend.diagnosticReport());backend.traceStartup(false);
        android.util.Log.i("PSXPerf","Saved "+dir);
    }
    @Test public void nativeCorrectness(){
        assertTrue("Explicit native validation",InstrumentationRegistry.getArguments().getString("nativeCheck","false").equals("true"));
        android.util.Log.i("PSXPerf",NativeCpuVerifier.verify());
        android.util.Log.i("PSXPerf",Gte.verifyNative());
        android.util.Log.i("PSXPerf",HardwareRasterizer.verify());
        android.util.Log.i("PSXPerf",HardwareRasterizer.verifySoftwareDisplayUploads());
        for(int[] option:new int[][]{{0,1},{1,1},{2,1},{1,3}})try(Gpu gpu=new Gpu(true,option[0],option[1])){
            int[] commands={0xa0000000,0,0x00010004,0x00020001,0x00040003,0xc0000000,0,0x00010004};
            for(int word:commands)gpu.write(word);assertEquals(0x00020001,gpu.read());
            for(int word:new int[]{0xa0000000,2,0x00010002,0x00080007})gpu.write(word);
            assertEquals("Ordered threaded VRAM read after intervening upload",0x00080007,gpu.read());
        }
        android.util.Log.i("PSXPerf",Spu.verifyNative());
        android.util.Log.i("PSXPerf",VulkanPresenter.verify());
    }
    @Test public void presentationWork()throws Exception{
        Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();Context c=instrumentation.getTargetContext();EngineAuditActivity activity=screen(instrumentation);activity.showLabel("GPU work test");
        JSONArray results=new JSONArray();
        for(boolean hardwareDrawing:new boolean[]{false,true})try(Gpu gpu=new Gpu(false,hardwareDrawing)){
            gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(3200<<12));gpu.control(0x07000000|(240<<10));
            gpu.write(0x02404040);gpu.write(0);gpu.write(320|(240<<16));
            java.lang.reflect.Field field=Gpu.class.getDeclaredField("hardware");field.setAccessible(true);HardwareRasterizer raster=(HardwareRasterizer)field.get(gpu);assertNotNull(raster);
            for(boolean changing:new boolean[]{false,true}){
                String before=gpu.renderStatistics();long cpu=Debug.threadCpuTimeNanos(),started=System.nanoTime();
                for(int frame=0;frame<120;frame++){
                    if(changing){int shade=96+(frame%4)*8;gpu.write(0x02000000|shade|(shade<<8)|(shade<<16));gpu.write(144|(112<<16));gpu.write(32|(16<<16));}
                    gpu.requestSurfaceFrame(activity.gameSurface(),true,value->assertNull("Direct presenter unavailable",value));
                }
                int[] state={0,0,0,0,3200,0,240,1};Gpu.Frame captured=raster.capture(state);long elapsed=System.nanoTime()-started,used=Debug.threadCpuTimeNanos()-cpu;
                Bitmap posted=Bitmap.createBitmap(320,240,Bitmap.Config.ARGB_8888);CountDownLatch ready=new CountDownLatch(1);int[] result={-1};
                PixelCopy.request(activity.gameSurface(),posted,code->{result[0]=code;ready.countDown();},new Handler(Looper.getMainLooper()));assertTrue(ready.await(5,TimeUnit.SECONDS));assertEquals(PixelCopy.SUCCESS,result[0]);
                int expected=captured.pixels()[120*320+160],actual=posted.getPixel(160,120);posted.recycle();
                for(int shift:new int[]{0,8,16})assertTrue("Displayed pixel stale",Math.abs(((expected>>>shift)&255)-((actual>>>shift)&255))<=2);
                results.put(new JSONObject().put("mode",hardwareDrawing?"hardware":"software").put("changing",changing).put("frames",120).put("wallNanos",elapsed).put("cpuNanos",used).put("before",before).put("after",gpu.renderStatistics()).put("center",expected));
            }
        }
        File folder=new File(c.getExternalFilesDir(null),"performance");folder.mkdirs();File output=new File(folder,"presentation-"+System.currentTimeMillis()+".json");write(output,results.toString(2));android.util.Log.i("PSXPerf","Saved "+output);
    }
    private static void assertCenterMatches(EngineAuditActivity activity,Gpu gpu)throws Exception{
        Gpu.Frame image=gpu.displayFrame();Bitmap posted=Bitmap.createBitmap(320,240,Bitmap.Config.ARGB_8888);CountDownLatch ready=new CountDownLatch(1);int[] result={-1};
        PixelCopy.request(activity.gameSurface(),posted,code->{result[0]=code;ready.countDown();},new Handler(Looper.getMainLooper()));assertTrue(ready.await(5,TimeUnit.SECONDS));assertEquals(PixelCopy.SUCCESS,result[0]);
        int expected=image.pixels()[(image.height()/2)*image.width()+image.width()/2],actual=posted.getPixel(160,120);posted.recycle();
        for(int shift:new int[]{0,8,16})assertTrue("Vulkan image stale: expected="+Integer.toHexString(expected)+" actual="+Integer.toHexString(actual),Math.abs(((expected>>>shift)&255)-((actual>>>shift)&255))<=2);
    }
    private static void present(EngineAuditActivity activity,Gpu gpu,boolean filter){gpu.requestSurfaceFrame(activity.gameSurface(),filter,value->assertNull("Vulkan unavailable",value));}
    @Test public void vulkanReuseInvalidation()throws Exception{
        Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();EngineAuditActivity activity=screen(instrumentation);activity.showLabel("Vulkan reuse check");JSONArray phases=new JSONArray();
        try(Gpu gpu=new Gpu(false,2,1)){
            gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(3200<<12));gpu.control(0x07000000|(240<<10));
            gpu.write(0x02404040);gpu.write(0);gpu.write(320|(240<<16));
            java.lang.reflect.Field field=Gpu.class.getDeclaredField("vulkan");field.setAccessible(true);VulkanPresenter output=(VulkanPresenter)field.get(gpu);assertNotNull(output);
            for(boolean changing:new boolean[]{false,true}){
                long[] before=output.counters();long started=System.nanoTime();
                for(int frame=0;frame<120;frame++){
                    if(changing){int shade=96+(frame%4)*8;gpu.write(0x02000000|shade|(shade<<8)|(shade<<16));gpu.write(144|(112<<16));gpu.write(32|(16<<16));}
                    present(activity,gpu,true);
                }
                long elapsed=System.nanoTime()-started;long[] after=output.counters();assertEquals(gpu.renderStatistics(),changing?120:1,after[0]-before[0]);assertEquals(gpu.renderStatistics(),changing?0:119,after[1]-before[1]);
                assertEquals((changing?120:1)*320L*240*4,after[2]-before[2]);assertCenterMatches(activity,gpu);
                phases.put(new JSONObject().put("changing",changing).put("wallNanos",elapsed).put("work",gpu.renderStatistics()));
            }
            long before=output.counters()[0];present(activity,gpu,false);assertEquals(before+1,output.counters()[0]);assertCenterMatches(activity,gpu);
            activity.resizeSurface(640,480);long end=SystemClock.uptimeMillis()+5000;while((activity.bufferWidth()!=640||activity.bufferHeight()!=480)&&SystemClock.uptimeMillis()<end)SystemClock.sleep(20);
            assertEquals(640,activity.bufferWidth());assertEquals(480,activity.bufferHeight());before=output.counters()[0];present(activity,gpu,false);assertEquals(before+1,output.counters()[0]);assertCenterMatches(activity,gpu);
            android.view.Surface old=activity.gameSurface();activity.recreateSurface();end=SystemClock.uptimeMillis()+5000;while((activity.gameSurface()==old||activity.gameSurface()==null||!activity.gameSurface().isValid()||activity.bufferWidth()==0)&&SystemClock.uptimeMillis()<end)SystemClock.sleep(20);
            assertNotSame(old,activity.gameSurface());assertNotNull(activity.gameSurface());before=output.counters()[0];present(activity,gpu,false);assertEquals(before+1,output.counters()[0]);assertCenterMatches(activity,gpu);
            // Geometry, packed 24-bit and disabled-display changes must replace cached pixels.
            for(int command:new int[]{0x05000020,0x08000010,0x03000001,0x03000000}){before=output.counters()[0];gpu.control(command);present(activity,gpu,false);assertEquals(before+1,output.counters()[0]);assertCenterMatches(activity,gpu);}
            phases.put(new JSONObject().put("invalidationChecks","filter, resize, recreated surface, geometry, 24-bit, display enable").put("work",gpu.renderStatistics()));
        }
        File folder=new File(instrumentation.getTargetContext().getExternalFilesDir(null),"performance");folder.mkdirs();write(new File(folder,"vulkan-reuse-"+System.currentTimeMillis()+".json"),phases.toString(2));
    }
}
