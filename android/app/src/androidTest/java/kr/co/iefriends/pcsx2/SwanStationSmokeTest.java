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
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

/** Device fixtures are user-supplied, app-private, and never APK assets. */
@RunWith(AndroidJUnit4.class)
public class SwanStationSmokeTest {
    private interface Ready { boolean get(); }
    private static void until(String message, long timeout, Ready ready) {
        long deadline=SystemClock.uptimeMillis()+timeout;
        while(SystemClock.uptimeMillis()<deadline) {
            if(ready.get())return;
            SystemClock.sleep(50);
        }
        assertTrue(message,ready.get());
    }
    @Test public void bootTwiceWithVideoAudioInputAndStates() throws Exception {
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root=new File(context.getFilesDir(),"swan-smoke");
        File game=new File(root,"Crash2.chd");
        Context testContext=InstrumentationRegistry.getInstrumentation().getContext();
        for(String name:new String[]{"swan-shader.slangp","swan-shader.slang"})try(java.io.InputStream stream=testContext.getAssets().open(name)){
            Files.copy(stream,new File(root,name).toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
        File preset=new File(root,"swan-shader.slangp");
        assertTrue("User test game missing",game.isFile());
        NativeApp.mContext=new WeakReference<>(context);
        assertFalse("Frontend library could not load",NativeApp.hasNoNativeBinary);
        HandlerThread readerThread=new HandlerThread("SwanImageConsumer");readerThread.start();
        ImageReader reader=ImageReader.newInstance(960,720,PixelFormat.RGBA_8888,3);
        AtomicInteger colored=new AtomicInteger();
        AtomicBoolean lastFrameBlank=new AtomicBoolean(true);
        reader.setOnImageAvailableListener(source->{
            try(Image image=source.acquireLatestImage()){
                if(image==null)return;
                Image.Plane plane=image.getPlanes()[0];
                java.nio.ByteBuffer bytes=plane.getBuffer();
                boolean hasColor=false;
                for(int y=0;y<720;y+=17)for(int x=0;x<960;x+=19){
                    int offset=y*plane.getRowStride()+x*plane.getPixelStride();
                    if(offset+2<bytes.limit() && ((bytes.get(offset)&255)>20||(bytes.get(offset+1)&255)>20||(bytes.get(offset+2)&255)>20)) {
                        hasColor=true;
                    }
                }
                lastFrameBlank.set(!hasColor);
                if(hasColor)colored.incrementAndGet();
            }
        },new Handler(readerThread.getLooper()));
        File settings=new File(context.getFilesDir(),"settings.toml");
        byte[] originalSettings=settings.isFile()?Files.readAllBytes(settings.toPath()):null;
        long cancelledToken=0L;
        try {
            // The user fixture is named Crash2.chd but its mastered boot executable is SCUS_941.54.
            assertEquals("SCUS-94154",NativeApp.getDiscSerialForPath(game.getAbsolutePath()));
            assertEquals("",NativeApp.getDiscSerialForPath(new File(root,"missing.chd").getAbsolutePath()));
            String achievementHash=NativeApp.getAchievementsHashForPath(game.getAbsolutePath());
            assertTrue("CHD content hash missing",achievementHash.matches("[0-9a-f]{32}"));
            assertEquals(achievementHash,NativeApp.getAchievementsHashForPath(game.getAbsolutePath()));
            assertEquals("",NativeApp.getAchievementsHashForPath(new File(root,"missing.chd").getAbsolutePath()));
            assertTrue("Shader parameters missing",NativeApp.shaderPresetParams(preset.getAbsolutePath()).contains("Tint"));
            for(int launch=0;launch<8;launch++){
                // Reuse one Surface for two hardware and two software cold boots.
                // No implicit state load; BIOS animation remains enabled.
                String renderer=(launch<2||launch>=4)?"hardware":"software";
                String config="[cpu]\nexecution_mode=\"cached\"\nfast_boot=false\n[video]\nrenderer=\""+renderer+"\"\ninternal_scale=3\ngpu_backend=\""+(launch>=6?"angle":launch>=4?"vulkan":"opengl")+"\"\n";
                try(FileOutputStream stream=new FileOutputStream(settings)){
                    stream.write(config.getBytes(StandardCharsets.UTF_8));
                }
                NativeApp.initialize(root.getAbsolutePath(),new File(root,"bios").getAbsolutePath(),android.os.Build.VERSION.SDK_INT);
                NativeApp.setGlDriver(launch>=6?"angle":"system");
                NativeApp.setShaderChain(false,"");
                assertFalse("Achievement integration must remain softcore",NativeApp.isHardcoreMode());
                if(launch==0){
                    cancelledToken=NativeApp.prepareVMRun();assertTrue(cancelledToken>0);
                    NativeApp.shutdownVMRun(cancelledToken);
                    assertFalse("Cancelled reservation started a VM",NativeApp.runVMThreadForSession(game.getAbsolutePath(),cancelledToken));
                    assertFalse(NativeApp.hasActiveVM());NativeApp.releaseVMRun(cancelledToken);
                }
                colored.set(0);
                long surfaceOwner=launch+1;
                NativeApp.onNativeSurfaceCreated(surfaceOwner);
                NativeApp.onNativeSurfaceChanged(reader.getSurface(),960,720,surfaceOwner);
                if(launch>0)NativeApp.onNativeSurfaceDestroyed(surfaceOwner-1);
                AtomicReference<Throwable> failure=new AtomicReference<>();
                AtomicReference<Boolean> booted=new AtomicReference<>();
                final long sessionToken=NativeApp.prepareVMRun();assertTrue("Session reservation failed",sessionToken>0);
                // A late close from an older cancelled launch must not stop this one.
                NativeApp.shutdownVMRun(cancelledToken);
                Thread vm=new Thread(()->{try{booted.set(NativeApp.runVMThreadForSession(game.getAbsolutePath(),sessionToken));}catch(Throwable t){failure.set(t);}},"SwanSmokeVM");
                vm.start();
                try {
                    until("Core failed to start: "+failure.get(),20000,NativeApp::hasActiveVM);
                    until("No presented frames",30000,()->NativeApp.getPresentedFrameCount()>300);
                    until("Surface stayed black",15000,()->colored.get()>5);
                    until("No non-silent samples delivered to platform audio",20000,()->NativeApp.getAudioNonZeroSampleCount()>100);
                    String active=NativeApp.getActiveRenderer();
                    assertEquals("Active serial stubbed out","SCUS-94154",NativeApp.getGameSerial());
                    assertTrue(active,active.contains(launch>=6?"ANGLE":launch>=4?"Vulkan":launch<2?"hardware":"Software"));
                    NativeApp.setPadButton(108,32767,true);SystemClock.sleep(100);NativeApp.setPadButton(108,32767,false);
                    NativeApp.setPadAnalog(0,255,128);NativeApp.resetKeyStatus();
                    NativeApp.pause();SystemClock.sleep(100);
                    int count=NativeApp.getPresentedFrameCount();SystemClock.sleep(250);
                    assertEquals("Pause did not stop stepping",count,NativeApp.getPresentedFrameCount());
                    assertTrue("State save failed",NativeApp.saveStateToSlot(1));
                    assertEquals("Unchanged cards must not warn",0,NativeApp.loadStateFromSlotChecked(1,false));
                    assertTrue("State preview missing",NativeApp.getImageSlot(1).length>100);
                    assertTrue("State load failed",NativeApp.loadStateFromSlot(1));
                    assertTrue("Autosave slot failed",NativeApp.saveAutosaveState());
                    assertTrue("Autosave reload failed",NativeApp.loadAutosaveState());
                    File stateFile=new File(NativeApp.getSwanStatePath(1));assertTrue(stateFile.isFile());
                    byte[] validState=Files.readAllBytes(stateFile.toPath());
                    byte[] corrupt=validState.clone();corrupt[corrupt.length-1]^=1;
                    Files.write(stateFile.toPath(),corrupt);
                    assertEquals("Corrupt payload was accepted",-1,NativeApp.loadStateFromSlotChecked(1,true));
                    Files.write(stateFile.toPath(),validState);
                    assertTrue("Memory card path is not the core's card",new File(NativeApp.getSwanCardPath(1)).isFile());
                    File catalogue=new File(root,"smoke.cht");
                    Files.write(catalogue.toPath(),("[RAM byte test]\n301F0000 005A\n[Not supported]\nFF000000 0000\n").getBytes(StandardCharsets.UTF_8));
                    assertEquals(2,NativeApp.cheatsLoad(catalogue.getAbsolutePath()));
                    assertTrue(NativeApp.cheatsList().contains("RAM byte test"));
                    assertEquals(1,NativeApp.cheatsApply(true,new String[]{"RAM byte test"}));
                    NativeApp.resume();until("GameShark did not write RAM",3000,()->NativeApp.getSwanMemoryByte(0x1f0000)==0x5a);
                    NativeApp.pause();assertEquals(0,NativeApp.cheatsApply(false,new String[]{"RAM byte test"}));
                    assertEquals(0,NativeApp.cheatsArmedCount());assertTrue(NativeApp.loadStateFromSlot(1));
                    assertEquals(-1,NativeApp.cheatsLoad(new File(root,"missing.cht").getAbsolutePath()));
                    NativeApp.setRewind(true,3,4);NativeApp.resume();
                    until("Rewind captured no states",5000,()->NativeApp.rewindBytesHeld()>NativeApp.rewindSnapshotBytes()*2);
                    NativeApp.pause();SystemClock.sleep(50);int guestBefore=NativeApp.getSwanGuestFrame();
                    NativeApp.rewindStepBack();assertTrue("Rewind did not restore an earlier guest frame",NativeApp.getSwanGuestFrame()<guestBefore);
                    NativeApp.setRewind(false,3,4);assertEquals(0,NativeApp.rewindBytesHeld());
                    NativeApp.setRunahead(1);NativeApp.setStatisticsEnabled(true);NativeApp.resume();
                    double[] stats=new double[31];until("No frame statistics",5000,()->NativeApp.getStatistics(stats)==31);
                    assertTrue(stats[0]>0);assertTrue(stats[27]>0);assertTrue(stats[25]>=0);assertTrue(stats[23]>0&&stats[24]>0);
                    NativeApp.setAdpfEnabled(true);NativeApp.setAffinityMode(3);SystemClock.sleep(150);
                    NativeApp.setAffinityMode(0);NativeApp.setAdpfEnabled(false);
                    NativeApp.pause();NativeApp.setRunahead(0);NativeApp.setStatisticsEnabled(false);
                    NativeApp.setDisplayAspect(2);NativeApp.setStretchMode(false);NativeApp.setIntegerScaling(false);
                    NativeApp.setShaderChainParams(preset.getAbsolutePath(),new String[]{"Tint"},new float[]{0.6f});
                    colored.set(0);NativeApp.setShaderChain(true,preset.getAbsolutePath());NativeApp.resume();
                    until("Shader chain did not start",20000,()->NativeApp.getActiveRenderer().contains("shader chain"));
                    until("Shader image stayed black",5000,()->colored.get()>5);
                    for(int angle:new int[]{90,180,270,0}){colored.set(0);NativeApp.setDisplayRotation(angle);
                        until("Rotated shader image stayed black at "+angle,5000,()->colored.get()>3);}
                    NativeApp.setShaderChain(false,"");
                    until("Shader chain did not disable",5000,()->!NativeApp.getActiveRenderer().contains("shader chain"));
                    for(int angle:new int[]{90,180,270,0}){colored.set(0);NativeApp.setDisplayRotation(angle);
                        until("Rotated plain image stayed black at "+angle,8000,()->colored.get()>3);}
                    NativeApp.pause();NativeApp.onNativeSurfaceDestroyed(surfaceOwner);SystemClock.sleep(100);
                    colored.set(0);NativeApp.onNativeSurfaceChanged(reader.getSurface(),960,720,surfaceOwner);NativeApp.resume();
                    until("Surface recreation did not resume video",8000,()->colored.get()>3);
                    assertFalse("Missing disc accepted",NativeApp.changeDisc(new File(root,"missing.chd").getAbsolutePath()));
                    assertTrue("Disc reinsertion failed",NativeApp.changeDisc(game.getAbsolutePath()));
                    if(launch<2||launch>=4){File textures=new File(root,"texture-smoke-"+launch+"-"+System.nanoTime());NativeApp.setPs1TextureOptions(true,false,textures.getAbsolutePath());
                        NativeApp.resetGame();until("No VRAM texture PNG dumped: "+java.util.Arrays.toString(NativeApp.getSwanTextureMetrics()),15000,()->{
                            File[] files=new File(textures,"dump").listFiles();return files!=null&&files.length>0&&NativeApp.getSwanTextureMetrics()[3]>0;});
                        File[] images=new File(textures,"dump").listFiles();assertNotNull("Texture dump directory missing",images);
                        assertTrue("No VRAM texture PNG dumped",images.length>0);Bitmap dumped=BitmapFactory.decodeFile(images[0].getAbsolutePath());assertNotNull(dumped);dumped.recycle();
                        for(File file:images)Files.copy(file.toPath(),new File(new File(textures,"replacements"),file.getName()).toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                        NativeApp.setPs1TextureOptions(false,true,textures.getAbsolutePath());NativeApp.resetGame();
                        until("Replacement PNG never reached the core",15000,()->NativeApp.getSwanTextureMetrics()[4]>0);NativeApp.setPs1TextureOptions(false,false,"");}
                    NativeApp.setDisplayAspect(0);NativeApp.pause();
                    File png=new File(root,"smoke-"+launch+".png");NativeApp.saveScreenshot(png.getAbsolutePath());
                    Bitmap image=BitmapFactory.decodeFile(png.getAbsolutePath());assertNotNull("PNG decode failed",image);
                    android.util.Log.i("SwanSmoke","launch="+launch+" renderer="+active+" video="+image.getWidth()+"x"+image.getHeight()+" colored="+colored.get()+" audio="+NativeApp.getAudioNonZeroSampleCount());
                    image.recycle();
                    NativeApp.resume();
                    until("Resume stalled",5000,()->NativeApp.getPresentedFrameCount()>count+10);
                } finally {
                    NativeApp.shutdownVMRun(sessionToken);vm.join(15000);
                    assertFalse("Shutdown hung",vm.isAlive());
                    NativeApp.releaseVMRun(sessionToken);
                    until("Closed game left its last frame on the retained Surface",3000,lastFrameBlank::get);
                }
                assertNull("VM failed",failure.get());assertEquals(Boolean.TRUE,booted.get());
            }
        } finally {
            if(originalSettings!=null)Files.write(settings.toPath(),originalSettings);
            else Files.deleteIfExists(settings.toPath());
            NativeApp.onNativeSurfaceDestroyed(8L);reader.close();readerThread.quitSafely();readerThread.join(3000);
        }
    }
}
