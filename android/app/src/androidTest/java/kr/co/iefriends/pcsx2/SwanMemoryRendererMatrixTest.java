// SPDX-License-Identifier: GPL-3.0-or-later
package kr.co.iefriends.pcsx2;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Functional checks in the existing private app. Not an on-screen FPS benchmark. */
@RunWith(AndroidJUnit4.class)
public class SwanMemoryRendererMatrixTest {
    interface Ready { boolean get(); }
    static void until(String label,long timeout,Ready ready) {
        long end=SystemClock.uptimeMillis()+timeout;
        while(SystemClock.uptimeMillis()<end) {if(ready.get())return;SystemClock.sleep(50);}
        assertTrue(label+"\n"+NativeApp.getSwanDiagnostics(),ready.get());
    }
    static void write(File file,String text) throws Exception {
        Files.write(file.toPath(),text.getBytes(StandardCharsets.UTF_8));
    }
    @Test public void memoryPathLifecycle() throws Exception {
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        assertEquals("Private app required", "com.aaron.armsx1.renderthreadtest",context.getPackageName());
        String backend=InstrumentationRegistry.getArguments().getString("backend","vulkan");
        String memory=InstrumentationRegistry.getArguments().getString("memory","LUT");
        boolean rewrite="true".equals(InstrumentationRegistry.getArguments().getString("rewrite","false"));
        assertTrue(Arrays.asList("vulkan","opengl","angle","software").contains(backend));
        assertTrue(Arrays.asList("LUT","MMap").contains(memory));
        File external=context.getExternalFilesDir(null);assertNotNull(external);
        File fixtures=new File(external,"cpu-matrix-fixtures");
        File bios=new File(external,"bios/SCPH1001.BIN");assertEquals(524288,bios.length());
        File checkpoint=new File(fixtures,"current-game-checkpoint.swanstate");
        String title="Crash Bandicoot 2 - Cortex Strikes Back (USA)";
        String game=InstrumentationRegistry.getArguments().getString("game","/storage/emulated/0/ROMs/psx/Crash Bandicoot 2 - Cortex Strikes Back (USA).chd");
        assertTrue("ROM must be readable",new File(game).canRead());
        File root=new File(external,"cpu-matrix-results/"+backend+"-"+memory+"-"+rewrite);
        assertTrue(root.mkdirs()||root.isDirectory());
        File settings=new File(context.getFilesDir(),"settings.toml");
        byte[] original=settings.isFile()?Files.readAllBytes(settings.toPath()):null;
        NativeApp.mContext=new WeakReference<>(context);assertFalse(NativeApp.hasNoNativeBinary);
        HandlerThread consumer=new HandlerThread("MatrixImageConsumer");consumer.start();
        ImageReader reader=ImageReader.newInstance(960,720,PixelFormat.RGBA_8888,3);
        AtomicInteger imageFrames=new AtomicInteger();
        reader.setOnImageAvailableListener(r->{try(Image image=r.acquireLatestImage()){if(image!=null)imageFrames.incrementAndGet();}},new Handler(consumer.getLooper()));
        try {
            for(int launch=0;launch<2;launch++) {
                String config="[bios]\nsearch_path=\""+bios.getParent()+"\"\npreferred_model=\"SCPH-1001\"\n"+
                    "[cpu]\nexecution_mode=\"cached\"\nfast_boot=false\nfastmem_mode=\""+memory+"\"\nfastmem_rewrite="+rewrite+"\nblock_linking=true\ninstruction_cache=false\n"+
                    "[video]\nrenderer=\""+(backend.equals("software")?"software":"hardware")+"\"\ngpu_backend=\""+backend+"\"\ninternal_scale=3\n"+
                    "threaded_hardware_backend=true\nthreaded_gl_submission=false\noptimal_frame_pacing=true\nvsync=true\nsync_host_refresh=false\nmax_queued_frames=2\nshader_precompile=\"Lazy\"\npgxp=false\n"+
                    "[runtime]\nframe_limit=true\nspeed_percent=100\n[emulation]\nrunahead=0\nrewind=false\n"+
                    "[test]\ncpu_optimization_probe="+"true".equals(InstrumentationRegistry.getArguments().getString("cpu_probe","false"))+"\n";
                write(settings,config);
                NativeApp.initialize(root.getAbsolutePath(),bios.getParent(),android.os.Build.VERSION.SDK_INT);
                NativeApp.setGlDriver(backend.equals("angle")?"angle":"system");NativeApp.setShaderChain(false,"");
                NativeApp.setDisplayRefreshRate(60);
                NativeApp.configureSwanSession(title,game);
                NativeApp.configureSwanCardPaths(new File(root,"swanstation/cards/one.mcd").getAbsolutePath(),new File(root,"swanstation/cards/two.mcd").getAbsolutePath());
                long owner=launch+1;
                NativeApp.onNativeSurfaceCreated(owner);NativeApp.onNativeSurfaceChanged(reader.getSurface(),960,720,owner);
                long token=NativeApp.prepareVMRun();assertTrue(token>0);
                AtomicReference<Throwable> failure=new AtomicReference<>();AtomicReference<Boolean> booted=new AtomicReference<>();
                Thread vm=new Thread(()->{try{booted.set(NativeApp.runVMThreadForSession(game,token));}catch(Throwable error){failure.set(error);}},"MatrixVM");
                vm.start();
                try {
                    until("Boot failed",30000,NativeApp::hasActiveVM);
                    final int bootFrames=checkpoint.isFile()?150:600;
                    until("BIOS video stopped",30000,()->NativeApp.getPresentedFrameCount()>bootFrames);
                    assertEquals("SCUS-94154",NativeApp.getGameSerial());
                    for(int slot=1;slot<=2;slot++)assertTrue("Card escaped test root",new File(NativeApp.getSwanCardPath(slot)).getCanonicalPath().startsWith(root.getCanonicalPath()+"/"));
                    String active=NativeApp.getActiveRenderer();
                    String expected=backend.equals("vulkan")?"Vulkan":backend.equals("angle")?"ANGLE":backend.equals("software")?"Software":"hardware";
                    assertTrue("Unexpected renderer: "+active,active.contains(expected));
                    NativeApp.pause();
                    File target=new File(NativeApp.getSwanStatePath(0));
                    assertTrue("State escaped test root",target.getCanonicalPath().startsWith(root.getCanonicalPath()+"/"));
                    target.getParentFile().mkdirs();
                    // Use a fresh private checkpoint with this harness's actual
                    // ROM path. Never bypass source/media or card safety guards.
                    if(!checkpoint.isFile()) {
                        assertTrue("Fixture checkpoint save failed",NativeApp.saveStateToSlot(0));
                        Files.copy(target.toPath(),checkpoint.toPath(),StandardCopyOption.REPLACE_EXISTING);
                    } else Files.copy(checkpoint.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING);
                    assertEquals("Checkpoint load failed",0,NativeApp.loadStateFromSlotChecked(0,false));
                    NativeApp.resume();int base=NativeApp.getPresentedFrameCount();
                    until("Checkpoint did not advance",15000,()->NativeApp.getPresentedFrameCount()>base+180);
                    until("No platform audio samples",10000,()->NativeApp.getAudioNonZeroSampleCount()>100);
                    until("No image consumer frames",10000,()->imageFrames.get()>10);
                    until("Effective CPU configuration not reported",6000,()->NativeApp.getSwanDiagnostics().contains("effective_fastmem="+memory+" ")&&
                        NativeApp.getSwanDiagnostics().contains("effective_fastmem_rewrite="+(rewrite?1:0)));
                    if("true".equals(InstrumentationRegistry.getArguments().getString("checked","false"))) {
                        int expectedChecked=memory.equals("LUT")&&!rewrite?1:0;
                        assertTrue("Unexpected checked LUT path: "+NativeApp.getSwanDiagnostics(),
                            NativeApp.getSwanDiagnostics().contains("effective_checked_lut="+expectedChecked));
                    }
                    NativeApp.setPadButton(96,32767,true);
                    assertEquals("Cross input not registered",1,NativeApp.getControlInputSnapshot()[0]);
                    SystemClock.sleep(100);NativeApp.setPadButton(96,32767,false);NativeApp.resetKeyStatus();
                    assertEquals("Input stuck",0,NativeApp.getControlInputSnapshot()[0]);
                    NativeApp.pause();SystemClock.sleep(100);int paused=NativeApp.getPresentedFrameCount();
                    SystemClock.sleep(200);assertEquals("Pause did not stop CPU frames",paused,NativeApp.getPresentedFrameCount());
                    if(launch==0&&"true".equals(InstrumentationRegistry.getArguments().getString("cpu_probe","false"))) {
                        byte[] probeBefore=NativeApp.getSwanMemorySnapshot();
                        String report=NativeApp.runSwanCpuOptimizationTests();
                        write(new File(root,"cpu-probe.json"),report);
                        assertTrue("Native CPU probe failed: "+report,new JSONObject(report).getBoolean("passed"));
                        assertArrayEquals("CPU probe changed game RAM",probeBefore,NativeApp.getSwanMemorySnapshot());
                    }
                    byte[] before=NativeApp.getSwanMemorySnapshot();assertTrue(before.length>=2*1024*1024);
                    assertTrue("State save failed",NativeApp.saveStateToSlot(9));
                    assertEquals("State reload failed",0,NativeApp.loadStateFromSlotChecked(9,false));
                    assertArrayEquals("RAM changed during paused round trip",before,NativeApp.getSwanMemorySnapshot());
                    File screenshot=new File(root,"run-"+launch+".png");NativeApp.saveScreenshot(screenshot.getAbsolutePath());
                    Bitmap image=BitmapFactory.decodeFile(screenshot.getAbsolutePath());assertNotNull("Screenshot missing",image);
                    Set<Integer> colors=new HashSet<>();
                    for(int y=0;y<image.getHeight();y+=17)for(int x=0;x<image.getWidth();x+=19)colors.add(image.getPixel(x,y));
                    image.recycle();assertTrue("Game image is blank/solid",colors.size()>32);
                    NativeApp.resume();int resumed=NativeApp.getPresentedFrameCount();
                    until("Resume stalled",10000,()->NativeApp.getPresentedFrameCount()>resumed+30);
                    if(launch==0&&backend.equals("vulkan")) {
                        NativeApp.setRunahead(1);int runahead=NativeApp.getPresentedFrameCount();
                        until("Runahead stalled",10000,()->NativeApp.getPresentedFrameCount()>runahead+30);
                        NativeApp.setRunahead(0);NativeApp.setRewind(true,3,4);
                        until("No rewind snapshots",10000,()->NativeApp.rewindBytesHeld()>NativeApp.rewindSnapshotBytes()*2);
                        NativeApp.pause();int guest=NativeApp.getSwanGuestFrame();NativeApp.rewindStepBack();
                        assertTrue("Rewind did not restore guest frames",NativeApp.getSwanGuestFrame()<guest);
                        NativeApp.setRewind(false,3,4);NativeApp.resume();
                    }
                    JSONObject result=new JSONObject().put("backend",backend).put("memory",memory).put("rewrite",rewrite).put("launch",launch)
                        .put("result","PASS").put("renderer",active).put("colored_samples",colors.size())
                        .put("diagnostics",NativeApp.getSwanDiagnostics());
                    write(new File(root,"run-"+launch+".json"),result.toString(2));
                    android.util.Log.i("SwanMemoryMatrix","PASS "+backend+" "+memory+" rewrite="+rewrite+" launch="+launch);
                } finally {
                    NativeApp.resetKeyStatus();NativeApp.shutdownVMRun(token);vm.join(15000);
                    assertFalse("Shutdown hung",vm.isAlive());NativeApp.releaseVMRun(token);NativeApp.onNativeSurfaceDestroyed(owner);
                }
                assertNull("VM failed",failure.get());assertEquals(Boolean.TRUE,booted.get());
            }
        } finally {
            if(original!=null)Files.write(settings.toPath(),original);else Files.deleteIfExists(settings.toPath());
            reader.close();consumer.quitSafely();consumer.join(3000);
        }
    }
}
