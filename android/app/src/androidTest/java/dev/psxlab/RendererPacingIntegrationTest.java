package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.*;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

/** Isolated session roots; optionally reads an explicitly supplied owned gameUri. */
public class RendererPacingIntegrationTest {
    private static int underruns(String summary){
        Matcher match=Pattern.compile("(\\d+) underruns").matcher(summary);
        assertTrue(summary,match.find());return Integer.parseInt(match.group(1));
    }
    @Test public void nativeAndHdOutputsKeepGuestRateAndAudioAcrossRenderers()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        File fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        String game=InstrumentationRegistry.getArguments().getString("gameUri");boolean ownedGame=game!=null;
        if(game==null)game=new File(fixtures,"game.chd").toString();final String source=game;
        File root=new File(context.getExternalFilesDir(null),"performance/renderer-pacing-"+System.currentTimeMillis());assertTrue(root.mkdirs());
        for(String mode:new String[]{"software","opengl","vulkan"})for(int scale:new int[]{1,4}){
            File session=new File(root,mode+"-"+scale);assertTrue(session.mkdirs());File config=new File(session,"settings.toml");
            Files.write(config.toPath(),("[cpu]\nfast_boot=true\n[video]\ngpu_backend=\""+mode+"\"\nrenderer=\""+(mode.equals("software")?"software":"hardware")+"\"\ninternal_scale="+scale+"\n[audio]\nmuted=true\n").getBytes(StandardCharsets.UTF_8));
            ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{});assertTrue(backend.initialize(session.toString(),fixtures.toString(),config.toString()));backend.selectBios("SCPH1001.BIN");
            try(ImageReader reader=ImageReader.newInstance(640,480,PixelFormat.RGBA_8888,4)){
                backend.displayMetadata(640,480,60);backend.surface(reader.getSurface());
                Thread vm=new Thread(()->backend.run(source),"PSX-Renderer-Pacing-Test");vm.start();
                long start=SystemClock.elapsedRealtimeNanos(),sampleStart=0,firstFrames=0;int firstUnderruns=-1,images=0;
                try{
                    while(SystemClock.elapsedRealtimeNanos()-start<(ownedGame?30_000_000_000L:6_000_000_000L)){
                        try(Image image=reader.acquireLatestImage()){if(image!=null)images++;}
                        assertEquals(mode+" "+scale, "",backend.lastError());assertTrue(vm.isAlive());
                        if(sampleStart==0&&SystemClock.elapsedRealtimeNanos()-start>=(ownedGame?25_000_000_000L:3_000_000_000L)){sampleStart=SystemClock.elapsedRealtimeNanos();firstFrames=backend.frames();firstUnderruns=underruns(backend.audioDiagnostic());}
                        SystemClock.sleep(3);
                    }
                    double seconds=(SystemClock.elapsedRealtimeNanos()-sampleStart)/1e9,fps=(backend.frames()-firstFrames)/seconds;
                    JSONObject report=new JSONObject(backend.diagnosticReport());double nominal=report.getDouble("nominalFps");
                    report.put("sampleFps",fps);report.put("sampleSeconds",seconds);report.put("consumedImages",images);report.put("warmupUnderruns",firstUnderruns);
                    Files.write(new File(session,"report.json").toPath(),report.toString(2).getBytes(StandardCharsets.UTF_8));
                    assertEquals("Actual display API for "+mode+" "+scale,mode.equals("vulkan")?"Vulkan":"OpenGL",report.getString("displayRenderer"));
                    // Static boot screens may retain one image across many guest frames.
                    // Pixel/mode-change tests separately verify changing display content.
                    assertTrue("Consumed display buffers: "+images+", "+report,images>=1);if(ownedGame)assertTrue("Nonzero guest audio",backend.audioNonzero()>0);
                    assertTrue(mode+" "+scale+" FPS="+fps+" nominal="+nominal,fps>nominal*.95&&fps<nominal*1.05);
                    assertEquals("Audio underruns during steady sample: "+backend.audioDiagnostic(),firstUnderruns,underruns(backend.audioDiagnostic()));
                    android.util.Log.i("RendererPacingTest",mode+" "+scale+"x FPS="+fps+" "+backend.audioDiagnostic());
                }finally{backend.stop();vm.join(15000);assertFalse("Renderer stops",vm.isAlive());}
            }
        }
    }
}
