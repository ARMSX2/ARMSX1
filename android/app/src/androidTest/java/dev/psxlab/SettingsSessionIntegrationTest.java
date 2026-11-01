package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.core.Cpu;
import java.io.File;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.Callable;
import org.junit.Test;
import static org.junit.Assert.*;

/** A second game must resolve its config without inheriting the first game's live overrides. */
public class SettingsSessionIntegrationTest {
    private static void word(byte[] data,int at,int value){for(int i=0;i<4;i++)data[at+i]=(byte)(value>>>(8*i));}
    private Object field(Object target,String name)throws Exception{Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(target);}
    private Object boundary(ArmSxBackend backend,Callable<?> action)throws Exception{Method m=ArmSxBackend.class.getDeclaredMethod("call",Callable.class,Object.class);m.setAccessible(true);return m.invoke(backend,action,null);}
    @Test public void stoppedSessionReleasesOverridesAndNextGameUsesItsGlobalBaseline()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File fixtures=new File(context.getExternalFilesDir(null),"fixtures"),root=new File(context.getExternalFilesDir(null),"performance/settings-session-"+System.currentTimeMillis());assertTrue(root.mkdirs());
        File config=new File(root,"settings.toml"),game=new File(root,"loop.exe");
        Files.write(config.toPath(),("[video]\ngpu_backend=\"opengl\"\nrenderer=\"hardware\"\ninternal_scale=1\ndisplay_aspect=\"wide16x9\"\nwidescreen_hack=false\n[audio]\nmuted=true\n").getBytes(StandardCharsets.UTF_8));
        byte[] exe=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,exe,0,8);word(exe,16,0x80010000);word(exe,24,0x80010000);word(exe,28,2048);word(exe,2048,0x1000ffff);Files.write(game.toPath(),exe);
        ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{},new File(root,"logs"));assertTrue(backend.initialize(root.toString(),fixtures.toString(),config.toString()));backend.selectBios("SCPH1001.BIN");
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4)){
            backend.surface(reader.getSurface());reader.setOnImageAvailableListener(r->{try(Image image=r.acquireLatestImage()){}},new android.os.Handler(android.os.Looper.getMainLooper()));
            backend.displayAspectCustom(20f/9f);backend.displayAspect(3);backend.widescreenHack(true);backend.overscanCrop(1);
            for(int session=0;session<2;session++){
                Thread vm=new Thread(()->backend.run(game.toString()),"PSX-Settings-Test");vm.start();
                try{
                    long end=SystemClock.uptimeMillis()+10000;while((!backend.active()||backend.frames()<4)&&SystemClock.uptimeMillis()<end){assertEquals("",backend.lastError());SystemClock.sleep(5);}assertTrue(backend.frames()>=4);backend.pause(true);
                    assertEquals(session==0,boundary(backend,()->((Cpu)field(backend,"cpu")).gte.widescreenEnabled()));
                    assertEquals(session==0?20f/9f:16f/9f,(Float)field(backend,"outputAspect"),0.001f);
                }finally{backend.stop();vm.join(15000);assertFalse(vm.isAlive());}
                assertEquals("",backend.lastError());
                if(session==1){assertNull(field(backend,"wideOverride"));assertEquals(-1,field(backend,"aspectOverride"));assertEquals(-1,field(backend,"cropOverride"));}
            }
        }
    }
}
