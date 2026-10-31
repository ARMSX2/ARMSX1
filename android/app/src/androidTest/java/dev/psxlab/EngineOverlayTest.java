package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.lang.reflect.Field;
import java.nio.file.Files;
import kr.co.iefriends.pcsx2.NativeApp;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Private buffers only: validates the actual frontend bridge without changing player settings. */
@RunWith(AndroidJUnit4.class)
public class EngineOverlayTest {
    @Test public void overlayStatisticsAndRendererFollowTheRunningEngine()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("overlayAudit"));
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        Field router=NativeApp.class.getDeclaredField("lab");router.setAccessible(true);ArmSxBackend previous=(ArmSxBackend)router.get(null);
        assertTrue("Do not interrupt a running session",previous==null||!previous.active());
        File fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        for(String output:new String[]{"software","opengl","vulkan"})for(String raster:new String[]{"software","hardware"})for(int scale:output.equals("vulkan")&&raster.equals("hardware")?new int[]{1,4}:new int[]{1}){
            File directory=new File(context.getCacheDir(),"overlay-audit-"+output+"-"+raster+"-"+System.nanoTime());assertTrue(directory.mkdirs());
            File config=new File(directory,"settings.toml");Files.write(config.toPath(),("[video]\ngpu_backend=\""+output+"\"\nrenderer=\""+raster+"\"\ninternal_scale="+scale+"\n[audio]\nmuted=true\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
            ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{});assertTrue(backend.initialize(directory.getAbsolutePath(),fixtures.getAbsolutePath(),config.getAbsolutePath()));backend.selectBios("SCPH1001.BIN");router.set(null,backend);
            try(ImageReader reader=ImageReader.newInstance(640,480,PixelFormat.RGBA_8888,4)){
                backend.surface(reader.getSurface());NativeApp.setStatisticsEnabled(true);
                Thread vm=new Thread(()->backend.run(new File(fixtures,"game.chd").getAbsolutePath()),"Overlay-Audit-CPU");vm.start();
                try{
                    double[] stats=new double[33];long end=SystemClock.uptimeMillis()+8000;boolean ready=false;
                    while(SystemClock.uptimeMillis()<end){try(Image image=reader.acquireLatestImage()){}assertEquals("",backend.lastError());if(NativeApp.getStatistics(stats)==33&&stats[25]>=0){ready=true;break;}SystemClock.sleep(8);}
                    assertTrue("Published OSD snapshot for "+output+"/"+raster,ready);
                    for(int index:new int[]{0,1,2,3,4,6,8,9,10,13,15,16,17,23,24,25,26,27,28,30,31,32})assertTrue("slot "+index+"="+stats[index],Double.isFinite(stats[index])&&stats[index]>=0);
                    assertTrue(stats[6]>0);assertTrue(stats[23]>0);assertTrue(stats[24]>0);assertEquals(scale,stats[30],0);
                    assertEquals(-1,stats[5],0);assertEquals(-1,stats[29],0);
                    String active=NativeApp.getActiveRenderer();assertEquals(output.equals("vulkan")?"Vulkan":"OpenGL",active);
                    assertFalse(new org.json.JSONObject(backend.diagnosticReport()).getJSONObject("logging").getBoolean("enabled"));
                    NativeApp.setStatisticsEnabled(false);assertEquals(0,NativeApp.getStatistics(stats));
                    android.util.Log.i("EngineOverlayTest","PASS "+output+"/"+raster+" renderer="+active+" scale="+scale+" stats="+java.util.Arrays.toString(stats));
                }finally{backend.stop();vm.join(10000);assertFalse(vm.isAlive());}
            }finally{router.set(null,previous);}
        }
    }
}
