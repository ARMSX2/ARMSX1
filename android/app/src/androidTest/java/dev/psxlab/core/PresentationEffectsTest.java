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
    @Test public void shaderChangesPixelsParametersSubframesAndFallsBack()throws Exception{
        EngineAuditActivity activity=screen();File directory=new File(inst.getTargetContext().getCacheDir(),"psxlab-effects-test");assertTrue(directory.mkdirs()||directory.isDirectory());
        for(String name:new String[]{"psxlab-phases.slangp","psxlab-phases.slang"})try(InputStream input=inst.getContext().getAssets().open(name)){Files.copy(input,new File(directory,name).toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);}
        String preset=new File(directory,"psxlab-phases.slangp").getAbsolutePath();
        try{for(int scale:new int[]{1,3})try(Gpu gpu=new Gpu(false,2,scale)){
            ShaderChain.backend(2);fill(gpu);ShaderChain.request(false,"");show(gpu,activity);int plain=pixel(activity);assertTrue(Integer.toHexString(plain),((plain>>8)&255)>150);
            assertTrue(ShaderChain.parameters(preset),ShaderChain.parameters(preset).contains("Tint"));
            ShaderChain.request(true,preset);gpu.prepareShaders();assertTrue(ShaderChain.status(),ShaderChain.active());
            show(gpu,activity);int tinted=pixel(activity);assertTrue(Integer.toHexString(tinted),((tinted>>8)&255)<((tinted>>16)&255)-20);
            ShaderChain.parameters(preset,new String[]{"Tint"},new float[]{0});show(gpu,activity);int red=pixel(activity);assertTrue(Integer.toHexString(red),(red&65535)<256&&((red>>16)&255)>150);
            ShaderChain.request(false,"");show(gpu,activity);assertEquals("Disable shader restores the plain static image",plain,pixel(activity));
            ShaderChain.request(true,preset);gpu.prepareShaders();show(gpu,activity);assertTrue(ShaderChain.status(),ShaderChain.active());
            VulkanPresenter output=vulkan(gpu);assertNotNull(output);
            ShaderChain.phase(2,1,60,4f/3);assertTrue(output.presentSubframe(activity.gameSurface()));SystemClock.sleep(50);red=pixel(activity);assertTrue(Integer.toHexString(red),((red>>16)&255)>150&&((red>>8)&255)<3);
            ShaderChain.phase(2,2,60,4f/3);assertTrue(output.presentSubframe(activity.gameSurface()));SystemClock.sleep(50);int green=pixel(activity);assertTrue(Integer.toHexString(green),((green>>8)&255)>150&&((green>>16)&255)<3);
            ShaderChain.request(true,new File(directory,"missing.slangp").getAbsolutePath());gpu.prepareShaders();show(gpu,activity);assertFalse(ShaderChain.active());assertEquals(plain,pixel(activity));
            ShaderChain.request(false,"");show(gpu,activity);assertEquals(plain,pixel(activity));
            android.util.Log.i("PSXEffectsAudit","PASS shader scale="+scale+" "+gpu.renderStatistics());
        }}finally{ShaderChain.request(false,"");inst.runOnMainSync(activity::finish);for(File file:directory.listFiles())assertTrue(file.delete());assertTrue(directory.delete());}
    }
}
