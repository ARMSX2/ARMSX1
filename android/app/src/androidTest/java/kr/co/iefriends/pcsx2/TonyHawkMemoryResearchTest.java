// SPDX-License-Identifier: GPL-3.0-or-later
package kr.co.iefriends.pcsx2;

import android.content.Context;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Opt-in developer harness. User game/BIOS/RAM are never included in either APK.
 *  Uses its own root/cards/states and restores frontend settings in finally.
 *  Commands: ID snapshot LABEL | ID press ANDROID_KEY[,KEY] MS | ID wait MS |
 *  ID hold KEY | ID release KEY | ID resume | ID load SLOT | ID save SLOT |
 *  ID cheats PATH | ID disable | ID stop.
 *  Snapshot pauses execution and saves RAM/PNG/frame metadata for comparison.
 *  Existing game cheats and enabled selections are not loaded or modified. */
@RunWith(AndroidJUnit4.class)
public class TonyHawkMemoryResearchTest {
    private static void write(File file, String text) throws Exception {
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    @Test public void researchUserSuppliedUsaGame() throws Exception {
        String gameUri=InstrumentationRegistry.getArguments().getString("researchGameUri");
        org.junit.Assume.assumeTrue("Opt-in researchGameUri required",gameUri!=null);
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root=new File(context.getFilesDir(),"tony-cheat-research");
        assertTrue(root.isDirectory() || root.mkdirs());
        File output=new File(context.getExternalFilesDir(null),"tony-cheat-research");
        assertTrue(output.isDirectory() || output.mkdirs());
        File game=new File(root,"Tony-USA.chd");
        try(InputStream input=context.getContentResolver().openInputStream(Uri.parse(gameUri))){
            assertNotNull("User ROM inaccessible",input);
            Files.copy(input,game.toPath(),StandardCopyOption.REPLACE_EXISTING);
        }
        NativeApp.mContext=new WeakReference<>(context);
        assertFalse(NativeApp.hasNoNativeBinary);
        assertEquals("Wrong ROM: no research on another region", "SLUS-00860",NativeApp.getDiscSerialForPath(game.getAbsolutePath()));
        File settings=new File(context.getFilesDir(),"settings.toml");
        byte[] previous=settings.isFile()?Files.readAllBytes(settings.toPath()):null;
        HandlerThread consumer=new HandlerThread("TonyResearchFrames");consumer.start();
        ImageReader reader=ImageReader.newInstance(960,720,PixelFormat.RGBA_8888,3);
        reader.setOnImageAvailableListener(source->{try(Image frame=source.acquireLatestImage()){}},new Handler(consumer.getLooper()));
        long owner=20261014L, token=0;
        Thread vm=null;
        AtomicReference<Throwable> failure=new AtomicReference<>();
        try {
            write(settings,"[cpu]\nexecution_mode=\"cached\"\nfast_boot=true\n[video]\nrenderer=\"software\"\ninternal_scale=1\ngpu_backend=\"opengl\"\n");
            NativeApp.initialize(root.getAbsolutePath(),new File(context.getFilesDir(),"swan-smoke/bios").getAbsolutePath(),android.os.Build.VERSION.SDK_INT);
            NativeApp.configureSwanSession("tony-original-research-20261014",gameUri);
            NativeApp.setShaderChain(false,"");
            NativeApp.onNativeSurfaceCreated(owner);
            NativeApp.onNativeSurfaceChanged(reader.getSurface(),960,720,owner);
            token=NativeApp.prepareVMRun();assertTrue(token>0);
            final long run=token;
            vm=new Thread(()->{try{assertTrue(NativeApp.runVMThreadForSession(game.getAbsolutePath(),run));}catch(Throwable error){failure.set(error);}},"TonyOriginalResearch");
            vm.start();
            long start=SystemClock.uptimeMillis();
            while(!NativeApp.hasActiveVM() && failure.get()==null && SystemClock.uptimeMillis()-start<30000)SystemClock.sleep(50);
            assertNull(failure.get());assertTrue("Core did not boot",NativeApp.hasActiveVM());
            long frameDeadline=SystemClock.uptimeMillis()+30000;
            while(NativeApp.getPresentedFrameCount()<120 && failure.get()==null && SystemClock.uptimeMillis()<frameDeadline)SystemClock.sleep(50);
            assertTrue("No research frames",NativeApp.getPresentedFrameCount()>=120);
            assertEquals("SLUS-00860",NativeApp.getGameSerial());
            assertEquals(0,NativeApp.cheatsArmedCount());
            write(new File(output,"ready.txt"),"SLUS-00860\nresearch sandbox only\n");
            String last="";
            File command=new File(root,"command.txt");
            // A fresh session must not replay a previous session's final command.
            if(command.isFile())last=new String(Files.readAllBytes(command.toPath()),StandardCharsets.UTF_8).trim().split("\\s+")[0];
            long deadline=SystemClock.uptimeMillis()+20*60*1000;
            while(SystemClock.uptimeMillis()<deadline && failure.get()==null){
                if(!command.isFile()){SystemClock.sleep(100);continue;}
                String line=new String(Files.readAllBytes(command.toPath()),StandardCharsets.UTF_8).trim();
                String[] fields=line.split("\\s+");
                if(fields.length<2 || fields[0].equals(last)){SystemClock.sleep(100);continue;}
                last=fields[0];
                String action=fields[1];
                if(action.equals("stop"))break;
                if(action.equals("snapshot")){
                    assertTrue(fields.length==3 && fields[2].matches("[A-Za-z0-9_-]+"));
                    NativeApp.pause();
                    byte[] memory=NativeApp.getSwanMemorySnapshot();
                    assertEquals(2*1024*1024,memory.length);
                    // Verify byte consistency against existing diagnostic API while paused.
                    for(int offset:new int[]{0,0x1000,memory.length-1})assertEquals(memory[offset]&255,NativeApp.getSwanMemoryByte(offset));
                    Files.write(new File(output,fields[2]+".ram").toPath(),memory);
                    NativeApp.saveScreenshot(new File(output,fields[2]+".png").getAbsolutePath());
                    write(new File(output,fields[2]+".txt"),"serial=SLUS-00860\nframe="+NativeApp.getSwanGuestFrame()+"\ncheats="+NativeApp.cheatsArmedCount()+"\n");
                }else if(action.equals("press")){
                    String[] keys=fields[2].split(",");int duration=Integer.parseInt(fields[3]);
                    assertTrue(duration>=1 && duration<=5000);
                    NativeApp.resume();for(String key:keys)NativeApp.setPadButton(Integer.parseInt(key),32767,true);
                    SystemClock.sleep(duration);for(String key:keys)NativeApp.setPadButton(Integer.parseInt(key),32767,false);
                }else if(action.equals("resume")){NativeApp.resume();
                }else if(action.equals("hold") || action.equals("release")){
                    NativeApp.resume();NativeApp.setPadButton(Integer.parseInt(fields[2]),32767,action.equals("hold"));
                }else if(action.equals("wait")){int duration=Integer.parseInt(fields[2]);assertTrue(duration>=0 && duration<=30000);NativeApp.resume();SystemClock.sleep(duration);NativeApp.pause();
                }else if(action.equals("save")){NativeApp.pause();assertTrue(NativeApp.saveStateToSlot(Integer.parseInt(fields[2])));
                }else if(action.equals("load")){NativeApp.pause();assertTrue(NativeApp.loadStateFromSlot(Integer.parseInt(fields[2])));
                }else if(action.equals("cheats")){
                    File catalogue=new File(root,fields[2]);
                    assertEquals("Only sandbox code files allowed",root.getCanonicalFile(),catalogue.getCanonicalFile().getParentFile());
                    int count=NativeApp.cheatsLoad(catalogue.getAbsolutePath());assertTrue(count>0);
                    java.util.List<String> names=new java.util.ArrayList<>();
                    for(String row:NativeApp.cheatsList().split("\u001e"))names.add(row.split("\u001f")[0]);
                    assertEquals(count,NativeApp.cheatsApply(true,names.toArray(new String[0])));
                }else if(action.equals("disable")){assertEquals(0,NativeApp.cheatsApply(false,new String[0]));
                }else{fail("Unknown research action: "+action);}
                write(new File(output,"done.txt"),last+" "+action+"\n");
            }
            assertNull(failure.get());
        } finally {
            if(token>0){NativeApp.shutdownVMRun(token);if(vm!=null)vm.join(15000);NativeApp.releaseVMRun(token);}
            NativeApp.onNativeSurfaceDestroyed(owner);reader.close();consumer.quitSafely();consumer.join(3000);
            if(previous!=null)Files.write(settings.toPath(),previous);else Files.deleteIfExists(settings.toPath());
        }
        assertTrue(vm==null || !vm.isAlive());
        assertNull(failure.get());
        assertEquals(0,NativeApp.getSwanMemorySnapshot().length);
    }
}
