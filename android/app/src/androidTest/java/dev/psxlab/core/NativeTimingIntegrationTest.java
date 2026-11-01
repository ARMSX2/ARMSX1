package dev.psxlab.core;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.util.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class NativeTimingIntegrationTest {
    @Test public void openGlSeparatesCommandUploadWindowAndSwapWaits()throws Exception{
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4);HardwareRasterizer raster=new HardwareRasterizer(4)){
            raster.configureProfiling(true);raster.fill(0xff,0,320|(240<<16));
            raster.upload(new int[]{0x001f001f},0,0,0,2,2,0);
            assertTrue(raster.present(reader.getSurface(),new int[]{0,0,0,0,3200,0,240,0}));
            for(String path:new String[]{"gpu.commands.upload","gpu.present.gles.window","gpu.present.gles.swap"}){
                Map<?,?> row=stage(raster.nativeTimings(),path);
                assertTrue(path,((Number)row.get("calls")).longValue()>0);
                assertTrue(path,((Number)row.get("wall_ns")).longValue()>0);
                assertEquals((long)android.os.Process.myTid(),row.get("tid"));
                assertTrue(row.get("source").toString().contains("hardware_gpu.cpp"));
            }
            long calls=((Number)stage(raster.nativeTimings(),"gpu.commands.upload").get("calls")).longValue();
            raster.configureProfiling(false);raster.upload(new int[]{0x03e003e0},0,0,0,2,2,0);
            assertEquals(calls,stage(raster.nativeTimings(),"gpu.commands.upload").get("calls"));
        }
    }
    private static Map<?,?> stage(Map<String,Object> profile,String path){
        for(Object entry:(List<?>)profile.get("stages")){Map<?,?> row=(Map<?,?>)entry;if(path.equals(row.get("path")))return row;}
        throw new AssertionError("Missing stage "+path+": "+profile);
    }
    @Test public void nativeScopesExposePathsThreadsAndRealTimings()throws Exception{
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);Cpu c=new Cpu(b);int[] code={0x24420001,0x1000fffe,0};for(int i=0;i<code.length;i++)b.write32(0x10000+i*4,code[i]);
            assertTrue(c.enableNative(true,true,true));c.configureProfiling(true);
            try{c.run(1000);Map<String,Object> metrics=c.nativeTimings();Map<String,Object> timing=(Map<String,Object>)metrics.get("timing");Map<?,?> compile=stage(timing,"cpu.compile");
                assertTrue(((Number)compile.get("wall_ns")).longValue()>0);assertEquals((long)android.os.Process.myTid(),compile.get("tid"));
                assertTrue(compile.get("source").toString().contains("compileArm64"));
                b.write32(0x10000,0x24420002);c.jump(0x80010000);c.run(1000);metrics=c.nativeTimings();Map<?,?> cache=(Map<?,?>)metrics.get("cache");assertEquals("80010000",cache.get("changed_instruction_address"));assertEquals("24420001",cache.get("previous_instruction"));assertEquals("24420002",cache.get("replacement_instruction"));
                c.configureProfiling(false);long count=((Number)stage((Map<String,Object>)metrics.get("timing"),"cpu.compile").get("calls")).longValue();b.write32(0x10000,0x24420003);c.jump(0x80010000);c.run(1000);
                assertEquals(count,stage((Map<String,Object>)c.nativeTimings().get("timing"),"cpu.compile").get("calls"));
            }finally{c.closeNative();}
        }
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4);HardwareRasterizer raster=new HardwareRasterizer(3);VulkanPresenter output=new VulkanPresenter(3)){
            raster.configureProfiling(true);output.configureProfiling(true);raster.fill(0xff,0,320|(240<<16));
            assertTrue(raster.presentResolved(reader.getSurface(),new int[]{0,0,0,0,3200,0,240,0},output.nativeHandle()));
            long end=SystemClock.uptimeMillis()+5000;boolean image=false;while(SystemClock.uptimeMillis()<end){try(Image frame=reader.acquireLatestImage()){if(frame!=null){image=true;break;}}SystemClock.sleep(2);}assertTrue(image);
            Map<String,Object> gpu=output.nativeTimings();for(String path:new String[]{"gpu.vulkan.fence_wait","gpu.vulkan.acquire","gpu.vulkan.submit","gpu.vulkan.present"}){
                Map<?,?> row=stage(gpu,path);assertTrue(((Number)row.get("calls")).longValue()>0);assertEquals((long)android.os.Process.myTid(),row.get("tid"));assertTrue(row.get("source").toString().contains("vulkan_output.cpp"));
            }
            assertTrue(((Number)stage(raster.nativeTimings(),"gpu.vram.transfer").get("calls")).longValue()>0);
        }
    }
}
