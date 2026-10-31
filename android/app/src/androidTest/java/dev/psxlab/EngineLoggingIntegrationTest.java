package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.core.EngineLog;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.*;
import org.junit.Test;
import static org.junit.Assert.*;

/** End-to-end recording/export data check, with isolated settings/cards and no player input. */
public class EngineLoggingIntegrationTest {
    @Test public void runningEngineExportsNativePathsThreadsAudioMemoryAndCheatState()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        File root=new File(context.getExternalFilesDir(null),"performance/logger-"+System.currentTimeMillis());assertTrue(root.mkdirs());File config=new File(root,"settings.toml"),codes=new File(root,"fixture.cht"),logs=new File(root,"logs");
        Files.write(codes.toPath(),"[Patch]\n301FFF00 0063\n".getBytes(StandardCharsets.UTF_8));
        Files.write(config.toPath(),("[cpu]\nfast_boot=true\n[video]\ngpu_backend=\"vulkan\"\nrenderer=\"hardware\"\ninternal_scale=1\n[runtime]\nlogging_enabled=true\nlog_level=2\n[audio]\nmuted=true\n[cheats]\nenabled=true\nfile=\""+codes+"\"\nenabled_codes=[\"Patch\"]\n").getBytes(StandardCharsets.UTF_8));
        ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{},logs);assertTrue(backend.initialize(root.toString(),fixtures.toString(),config.toString()));backend.selectBios("SCPH1001.BIN");
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4)){
            backend.surface(reader.getSurface());Thread vm=new Thread(()->backend.run(new File(fixtures,"game.chd").toString()),"PSX-Logger-Test-CPU");vm.start();
            try{
                long end=SystemClock.uptimeMillis()+2500;while(SystemClock.uptimeMillis()<end){try(Image image=reader.acquireLatestImage()){}assertEquals("",backend.lastError());SystemClock.sleep(3);}
                assertTrue(backend.frames()>30);assertEquals(1,backend.cheatsArmedCount());assertTrue(backend.cheatsList().contains("Patch"));
                JSONObject report=new JSONObject(backend.diagnosticReport());JSONObject nativeTiming=report.getJSONObject("nativeTimings");
                assertTrue(nativeTiming.toString(),nativeTiming.getJSONObject("cpu").getJSONObject("timing").getBoolean("available"));assertTrue(nativeTiming.toString(),nativeTiming.getJSONObject("gpu").getJSONObject("hardware").getBoolean("available"));
                assertTrue(report.getJSONObject("memory").getDouble("processRssMb")>0);assertTrue(report.getString("audio").length()>0);assertEquals(1,report.getJSONObject("cheats").getInt("armed_entries"));
                assertTrue(backend.flushDiagnosticLog(5000));String text=new String(Files.readAllBytes(new File(logs,"engine-current.jsonl").toPath()),StandardCharsets.UTF_8);
                for(String expected:new String[]{"engine.snapshot","cpu.compile","hardware_gpu.cpp","gpu.vulkan","PSX-GPU","PSX-Logger-Test-CPU","sleep_overshoot_ns","audio_buffered_frames","native_heap_allocated_bytes"})assertTrue(expected+" absent from log",text.contains(expected));
                assertEquals("",report.getJSONObject("logging").getString("writer_error"));assertEquals(0,report.getJSONObject("logging").getLong("dropped_records"));
                backend.cheatsApply(false,null);assertEquals(0,backend.cheatsArmedCount());
            }finally{backend.stop();vm.join(15000);assertFalse(vm.isAlive());backend.configureLogging(false,EngineLog.INFO);}
        }
        assertEquals("",backend.lastError());
    }
}
