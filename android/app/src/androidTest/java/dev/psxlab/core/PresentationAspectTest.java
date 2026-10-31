package dev.psxlab.core;

import android.app.Instrumentation;
import android.content.*;
import android.graphics.Bitmap;
import android.os.*;
import android.view.PixelCopy;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.EngineAuditActivity;
import java.util.concurrent.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class PresentationAspectTest {
    private static Bitmap capture(EngineAuditActivity a)throws Exception{
        Bitmap bitmap=Bitmap.createBitmap(320,180,Bitmap.Config.ARGB_8888);CountDownLatch done=new CountDownLatch(1);int[] status={-1};
        PixelCopy.request(a.gameSurface(),bitmap,code->{status[0]=code;done.countDown();},new Handler(Looper.getMainLooper()));assertTrue(done.await(5,TimeUnit.SECONDS));assertEquals(PixelCopy.SUCCESS,status[0]);return bitmap;
    }
    private static void edge(EngineAuditActivity a,boolean black)throws Exception{
        Bitmap bitmap=capture(a);try{assertTrue("Image centre",(bitmap.getPixel(160,90)&0xffffff)!=0);assertEquals("Side border",black,(bitmap.getPixel(8,90)&0xffffff)==0);}finally{bitmap.recycle();}
    }
    private static void show(Gpu gpu,EngineAuditActivity a){java.util.concurrent.atomic.AtomicReference<Gpu.Frame> fallback=new java.util.concurrent.atomic.AtomicReference<>();gpu.requestSurfaceFrame(a.gameSurface(),true,fallback::set);gpu.sync();assertNull("Native presenter unavailable",fallback.get());SystemClock.sleep(60);}
    @Test public void aspectChangesInvalidateStaticFramesOnEveryPresenter()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("presentationAudit"));Instrumentation inst=InstrumentationRegistry.getInstrumentation();Context c=inst.getTargetContext();
        EngineAuditActivity a=(EngineAuditActivity)inst.startActivitySync(new Intent(c,EngineAuditActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        long until=SystemClock.uptimeMillis()+10000;while((a.gameSurface()==null||!a.hasWindowFocus()||a.bufferWidth()==0)&&SystemClock.uptimeMillis()<until)SystemClock.sleep(25);assertTrue(a.hasWindowFocus());
        try{for(int mode:new int[]{0,1,2})for(int scale:new int[]{1,3})try(Gpu gpu=mode==0&&scale==1?new Gpu(true,false):new Gpu(true,mode,scale)){
            gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(3200<<12));gpu.control(0x07000000|(240<<10));gpu.write(0x02c08040);gpu.write(0);gpu.write(320|(240<<16));
            gpu.configureDisplay(4f/3,false,false);show(gpu,a);edge(a,true);
            gpu.configureDisplay(16f/9,false,false);show(gpu,a);edge(a,false);
            gpu.configureDisplay(4f/3,false,false);show(gpu,a);edge(a,true);
            gpu.configureDisplay(1,true,false);show(gpu,a);edge(a,false);
            gpu.configureDisplay(4f/3,false,true);show(gpu,a);Bitmap image=capture(a);try{int sw=a.bufferWidth(),sh=a.bufferHeight(),fit=Math.min(sw,Math.round(sh*4f/3));int k=Math.min(fit/(320*scale),sh/(240*scale));int left=(sw-320*scale*k)/2,normalLeft=(sw-fit)/2;int x=Math.max(0,Math.min(319,((left+normalLeft)/2)*320/sw));assertEquals("Integer side margin",0,image.getPixel(x,90)&0xffffff);assertTrue((image.getPixel(160,90)&0xffffff)!=0);}finally{image.recycle();}
            android.util.Log.i("PSXAspectAudit","PASS mode="+mode+" scale="+scale+" "+gpu.renderStatistics());
        }}finally{inst.runOnMainSync(a::finish);}
    }
}
