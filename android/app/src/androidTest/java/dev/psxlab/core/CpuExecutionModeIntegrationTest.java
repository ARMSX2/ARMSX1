package dev.psxlab.core;

import android.content.Context;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.ArmSxBackend;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import static org.junit.Assert.*;

public class CpuExecutionModeIntegrationTest {
    @Rule public final TemporaryFolder folder=new TemporaryFolder();

    @Test public void gameStartupHonorsCpuExecutionMode() throws Exception {
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root=folder.newFolder();
        File state=new File(root,"psxlab");
        assertTrue(state.mkdir());
        assertTrue(new File(state,"migration-backup.txt").createNewFile());
        File bios=new File(root,"test-bios.bin");
        ByteBuffer rom=ByteBuffer.allocate(512*1024).order(ByteOrder.LITTLE_ENDIAN);
        rom.putInt(0x24420001); // addiu v0,v0,1
        rom.putInt(0x0bf00000); // j 0xbfc00000
        rom.putInt(0);          // nop
        Files.write(bios.toPath(),rom.array());

        File config=new File(root,"settings.toml");
        ArmSxBackend backend=new ArmSxBackend(context,paused->{},notice->{},new File(root,"logs"));
        String video="[video]\nrenderer = \"software\"\ngpu_backend = \"opengl\"\n";
        Files.write(config.toPath(),video.getBytes(StandardCharsets.UTF_8));
        String recompiler=run(backend,root,bios,config,"ARM64 recompiler");
        assertTrue(recompiler,recompiler.matches("(?s).*JIT instructions=[1-9][0-9]* .*"));

        Files.write(config.toPath(),(video+"[cpu]\nexecution_mode = \"cached_interpreter\"\n").getBytes(StandardCharsets.UTF_8));
        String cached=run(backend,root,bios,config,"Native cached interpreter");
        assertTrue(cached,cached.contains("ARM64 blocks=0 JIT instructions=0"));
        assertTrue(cached,cached.matches("(?s).*interpreted=[1-9][0-9]* .*"));

        Files.write(config.toPath(),(video+"[cpu]\nexecution_mode = \"interpreter\"\n").getBytes(StandardCharsets.UTF_8));
        String interpreter=run(backend,root,bios,config,"Java interpreter");
        assertTrue(interpreter,interpreter.startsWith("CPU Java interpreter"));
    }

    private String run(ArmSxBackend backend,File root,File bios,File config,String expectedMode) throws Exception {
        assertTrue(backend.lastError(),backend.initialize(root.getPath(),bios.getPath(),config.getPath()));
        try(ImageReader display=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,3)){
            backend.surface(display.getSurface());
            FutureTask<Boolean> session=new FutureTask<>(()->backend.run(""));
            new Thread(session,"test-psx-vm").start();
            try {
                long deadline=SystemClock.elapsedRealtime()+10_000;
                do {
                    if(session.isDone())fail(backend.lastError());
                    SystemClock.sleep(20);
                    try(Image image=display.acquireLatestImage()){}
                    JSONObject report=new JSONObject(backend.diagnosticReport());
                    if(report.getLong("frames")>0&&!report.getString("core").equals("Not started")){
                        String incidents=report.getJSONObject("timeline").getJSONArray("incidents").toString();
                        assertTrue(incidents,incidents.contains("CPU execution mode: "+expectedMode));
                        return report.getString("core");
                    }
                } while(SystemClock.elapsedRealtime()<deadline);
                throw new AssertionError("CPU did not produce a diagnostic sample for "+expectedMode);
            } finally {
                backend.surface(null);
                backend.stop();
                assertTrue(backend.lastError(),session.get(10,TimeUnit.SECONDS));
            }
        }
    }
}
