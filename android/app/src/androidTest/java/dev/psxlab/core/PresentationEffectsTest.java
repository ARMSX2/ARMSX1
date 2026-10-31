package dev.psxlab.core;

import android.app.Instrumentation;
import android.content.*;
import android.graphics.Bitmap;
import android.os.*;
import android.view.*;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.EngineAuditActivity;
import java.io.*;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Verifies actual posted pixels, not just saved settings or a successful JNI call. */
@RunWith(AndroidJUnit4.class)
public class PresentationEffectsTest {
    private final Instrumentation inst=InstrumentationRegistry.getInstrumentation();
    private EngineAuditActivity screen()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("effectsAudit"));
        EngineAuditActivity activity=(EngineAuditActivity)inst.startActivitySync(new Intent(inst.getTargetContext(),EngineAuditActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        long end=SystemClock.uptimeMillis()+10000;
        while((activity.gameSurface()==null||!activity.hasWindowFocus()||activity.bufferWidth()==0)&&SystemClock.uptimeMillis()<end)SystemClock.sleep(25);
        assertTrue(activity.hasWindowFocus());return activity;
    }
    private static int pixel(EngineAuditActivity activity)throws Exception{
        Bitmap image=Bitmap.createBitmap(32,24,Bitmap.Config.ARGB_8888);CountDownLatch done=new CountDownLatch(1);int[] result={-1};
        PixelCopy.request(activity.gameSurface(),image,code->{result[0]=code;done.countDown();},new Handler(Looper.getMainLooper()));
        assertTrue(done.await(5,TimeUnit.SECONDS));assertEquals(PixelCopy.SUCCESS,result[0]);try{return image.getPixel(16,12)&0xffffff;}finally{image.recycle();}
    }
    private static void fill(Gpu gpu){
        gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(3200<<12));gpu.control(0x07000000|(240<<10));
        gpu.write(0x02c0c0c0);gpu.write(0);gpu.write(320|(240<<16));
    }
    private static void show(Gpu gpu,EngineAuditActivity activity){
        AtomicReference<Gpu.Frame> fallback=new AtomicReference<>();gpu.requestSurfaceFrame(activity.gameSurface(),false,fallback::set);gpu.sync();assertNull(fallback.get());SystemClock.sleep(50);
    }
    private static VulkanPresenter vulkan(Gpu gpu)throws Exception{var field=Gpu.class.getDeclaredField("vulkan");field.setAccessible(true);return (VulkanPresenter)field.get(gpu);}
    @Test public void nativeCpuHonoursProjectionToggleWithoutChangingStateLayout(){
        assertEquals("true",InstrumentationRegistry.getArguments().getString("effectsAudit"));
        for(boolean compiler:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);cpu.cop0[12]=0x40000000;
            cpu.gte.writeControl(0,4096);cpu.gte.writeControl(2,4096);cpu.gte.writeControl(4,4096);cpu.gte.writeControl(26,1000);
            cpu.gte.writeData(0,100);cpu.gte.writeData(1,1000);bus.write32(0x10000,0x4a080001);
            assertTrue(cpu.enableNative(compiler,true));
            try{for(boolean wide:new boolean[]{false,true,false}){cpu.gte.setWidescreen(wide);cpu.jump(0x80010000);cpu.run(1);assertEquals(wide?75:100,(short)cpu.gte.readData(14));assertEquals(0,cpu.exceptions);}}
            finally{cpu.closeNative();}
        }
    }
}
