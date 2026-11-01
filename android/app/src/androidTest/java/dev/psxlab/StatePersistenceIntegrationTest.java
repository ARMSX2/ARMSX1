package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.core.Bus;
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.Callable;
import org.junit.Test;
import static org.junit.Assert.*;

/** Run writeState and reopenState in separate instrumentation processes with the same auditId.
 * Uses an isolated data root; never writes the player's slots, cards or configuration. */
public class StatePersistenceIntegrationTest {
    private static final int ADDRESS=0x1fff00,MARKER=0x53415645;
    private File root()throws Exception{
        String id=InstrumentationRegistry.getArguments().getString("auditId","");
        assertTrue("Pass a unique auditId",id.matches("[a-zA-Z0-9_-]{1,64}"));
        return new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getExternalFilesDir(null),"performance/state-persistence-"+id);
    }
    private Object boundary(ArmSxBackend backend,Callable<?> job)throws Exception{
        Method call=ArmSxBackend.class.getDeclaredMethod("call",Callable.class,Object.class);call.setAccessible(true);return call.invoke(backend,job,null);
    }
    private Bus bus(ArmSxBackend backend)throws Exception{Field field=ArmSxBackend.class.getDeclaredField("bus");field.setAccessible(true);return (Bus)field.get(backend);}
    private void run(boolean reopen)throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File root=root(),fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        if(!reopen){assertFalse("Use a new auditId",root.exists());assertTrue(root.mkdirs());Files.write(new File(root,"settings.toml").toPath(),("[cpu]\nfast_boot=true\n[video]\ngpu_backend=\"opengl\"\nrenderer=\"hardware\"\ninternal_scale=1\n[audio]\nmuted=true\n").getBytes(StandardCharsets.UTF_8));}
        else assertTrue(root.isDirectory());
        String source="content://state-persistence-audit/game.chd";
        File staged=new File(root,reopen?"launch-2.chd":"launch-1.chd");Files.copy(new File(fixtures,"game.chd").toPath(),staged.toPath());
        File state=BackendStateStore.file(new File(root,"psxlab"),source,3);byte[] before=reopen?Files.readAllBytes(state.toPath()):null;
        ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{},new File(root,"logs"));assertTrue(backend.initialize(root.toString(),fixtures.toString(),new File(root,"settings.toml").toString()));backend.selectBios("SCPH1001.BIN");backend.configureSession(source);
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4)){
            backend.surface(reader.getSurface());reader.setOnImageAvailableListener(r->{try(Image image=r.acquireLatestImage()){}},new android.os.Handler(android.os.Looper.getMainLooper()));
            Thread vm=new Thread(()->backend.run(staged.toString()),"PSX-State-Persistence-Test");vm.start();
            try{
                long deadline=SystemClock.uptimeMillis()+15000;while(backend.frames()<5&&SystemClock.uptimeMillis()<deadline){assertEquals("",backend.lastError());SystemClock.sleep(10);}assertTrue(backend.frames()>=5);backend.pause(true);
                assertEquals(Boolean.TRUE,boundary(backend,()->{bus(backend).write32(ADDRESS,reopen?0:MARKER);return true;}));
                if(!reopen){assertTrue(backend.lastError(),backend.save(3));assertTrue(backend.save(-1));assertEquals(source,BackendStateStore.metadata(state).game());assertEquals("",BackendStateStore.metadata(state).disc());}
                else{
                    assertEquals(state,backend.stateFile(3));assertTrue(backend.lastError(),backend.load(3,true));
                    assertEquals(MARKER,boundary(backend,()->bus(backend).read32(ADDRESS)));
                    assertArrayEquals(before,Files.readAllBytes(state.toPath()));
                    assertTrue(backend.lastError(),backend.load(-1,true));assertEquals(MARKER,boundary(backend,()->bus(backend).read32(ADDRESS)));
                    backend.widescreenHack(true);backend.displayAspect(3);
                    for(float aspect:new float[]{16f/10f,16f/9f,20f/9f,64f/27f}){
                        backend.displayAspectCustom(aspect);
                        Field cpu=ArmSxBackend.class.getDeclaredField("cpu");cpu.setAccessible(true);
                        assertEquals(dev.psxlab.core.Gte.projectionScale(true,aspect),boundary(backend,()->((dev.psxlab.core.Cpu)cpu.get(backend)).gte.projectionScale()));
                    }
                    backend.displayMetadata(2000,900,60);backend.stretchDisplay(true);
                    Field cpu=ArmSxBackend.class.getDeclaredField("cpu");cpu.setAccessible(true);
                    assertEquals(dev.psxlab.core.Gte.projectionScale(true,20f/9f),boundary(backend,()->((dev.psxlab.core.Cpu)cpu.get(backend)).gte.projectionScale()));
                }
            }finally{backend.stop();vm.join(15000);assertFalse(vm.isAlive());}
        }finally{assertTrue(staged.delete());}
        assertEquals("",backend.lastError());
    }
    @Test public void writeState()throws Exception{run(false);}
    @Test public void reopenState()throws Exception{run(true);}
}
