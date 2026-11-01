package dev.psxlab;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

/** An intentional invalid guest read verifies fault-time evidence survives VM teardown. */
public class CrashContextIntegrationTest {
    private static void word(byte[] data,int at,int value){for(int i=0;i<4;i++)data[at+i]=(byte)(value>>>(8*i));}
    @Test public void reportRetainsFailingInstructionAndRegistersAfterVmTeardown()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root=new File(context.getExternalFilesDir(null),"performance/crash-context-"+System.currentTimeMillis());assertTrue(root.mkdirs());
        File config=new File(root,"settings.toml"),exeFile=new File(root,"fault.exe");
        Files.write(config.toPath(),("[cpu]\nfast_boot=true\n[video]\ngpu_backend=\"opengl\"\nrenderer=\"hardware\"\ninternal_scale=1\n[audio]\nmuted=true\n[runtime]\nlogging_enabled=true\nlog_level=2\n").getBytes(StandardCharsets.UTF_8));
        byte[] exe=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,exe,0,8);
        word(exe,16,0x80010000);word(exe,24,0x80010000);word(exe,28,2048);
        word(exe,2048,0x3c08ffff);word(exe,2052,0x3508ff00);word(exe,2056,0x81020000);Files.write(exeFile.toPath(),exe);
        ArmSxBackend backend=new ArmSxBackend(context,p->{},m->{},new File(root,"logs"));
        assertTrue(backend.initialize(root.toString(),new File(context.getExternalFilesDir(null),"fixtures").toString(),config.toString()));backend.selectBios("SCPH1001.BIN");
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4)){
            backend.surface(reader.getSurface());reader.setOnImageAvailableListener(r->{try(Image image=r.acquireLatestImage()){}},new android.os.Handler(android.os.Looper.getMainLooper()));
            Thread vm=new Thread(()->backend.run(exeFile.toString()),"PSX-Crash-Context-Test");vm.start();vm.join(15000);
            if(vm.isAlive()){backend.stop();vm.join(15000);fail("Fault fixture did not finish");}
        }
        assertFalse(backend.active());assertTrue(backend.lastError(),backend.lastError().contains("FFFFFF00"));
        String cpu=new JSONObject(backend.diagnosticReport()).getString("failureCpuContext");
        assertTrue(cpu,cpu.contains("PC=80010008 instruction=81020000"));assertTrue(cpu,cpu.contains("r8=FFFFFF00"));
        assertTrue(new JSONObject(new String(Files.readAllBytes(new File(root,"psxlab/last-session.json").toPath()),StandardCharsets.UTF_8)).getString("failureCpuContext").contains("instruction=81020000"));
        assertTrue(backend.flushDiagnosticLog(5000));
        assertTrue(new String(Files.readAllBytes(new File(root,"logs/engine-current.jsonl").toPath()),StandardCharsets.UTF_8).contains("runtime.cpu_context"));
    }
}
