package dev.psxlab.core;

import androidx.test.platform.app.InstrumentationRegistry;
import android.graphics.PixelFormat;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import dev.psxlab.ArmSxBackend;
import java.io.*;
import java.nio.file.Files;
import java.util.Random;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import static org.junit.Assert.*;

public class GpuReplayIntegrationTest {
    private static File folder(){File out=new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getExternalFilesDir(null),"graphics-replay-validation");assertTrue(out.isDirectory()||out.mkdirs());return out;}
    private static EngineLog logger(File folder){EngineLog log=new EngineLog(folder,new EngineLog.Clock(){public long wall(){return System.nanoTime();}public long cpu(){return android.os.Debug.threadCpuTimeNanos();}public long tid(){return android.os.Process.myTid();}});log.configure(true,EngineLog.INFO);return log;}
    private static void send(Gpu gpu,int... words){for(int word:words)gpu.write(word);}

    @Test public void nativeVulkanCaptureAndTextAttachmentReplayWithoutGameAssets()throws Exception{
        File folder=folder();try(EngineLog log=logger(new File(folder,"native-log"));Gpu gpu=new Gpu(true,2,1,true)){
            gpu.setJournal(log);gpu.control(0x03000000);gpu.control(0x08000001);
            send(gpu,0xa0000000,0,1024|(512<<16));Random random=new Random(0x5245504c);for(int i=0;i<262144;i++)gpu.write(random.nextInt());
            send(gpu,0xe3000000,0xe407ffff,0x02000000,0,320|(240<<16));gpu.completeFrameWithoutPresentation(frame->{},null);gpu.sync();
            assertTrue(gpu.renderer(),gpu.renderer().contains("native Vulkan"));
            for(int frame=0;frame<5;frame++){
                send(gpu,0x600000ff,16+frame*30|(24<<16),20|(40<<16));
                send(gpu,0xe1000118,0x65000000,24+frame*30|(100<<16),10|(10<<8),12|(12<<16));
                send(gpu,0xa0000000,4|(4<<16),4|(1<<16),0x03e0001f,0x7fff7c00);
                send(gpu,0x80000000,4|(4<<16),4|(8<<16),4|(1<<16));gpu.completeFrameWithoutPresentation(image->{},null);
            }
            GpuReplay.Capture capture=gpu.captureGraphics();assertArrayEquals(capture.after(),GpuReplay.replay(capture));
            File binary=new File(folder,"native.armgfx");GpuReplay.write(binary,"{\"game_id\":\"synthetic-native-validation\",\"renderer\":\"native Vulkan 1x\"}",capture);
            assertArrayEquals(capture.after(),GpuReplay.replay(GpuReplay.read(binary).capture()));assertTrue("Test attachment must exceed ordinary log-tail truncation",binary.length()>512*1024);
            try(Writer report=new BufferedWriter(new FileWriter(new File(folder,"native-report.txt")))){GpuReplayReport.append(report,binary);}
        }
    }

    @Test public void backendAutomaticallySavesBeforePauseAndStopClearTheSurface()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File fixtures=new File(context.getExternalFilesDir(null),"fixtures"),game=new File(fixtures,"game.chd"),bios=new File(fixtures,"SCPH1001.BIN");
        assertTrue("Existing licensed Thor fixture required",game.isFile()&&bios.isFile());File folder=new File(folder(),"backend"),logs=new File(folder,"logs");assertTrue(folder.isDirectory()||folder.mkdirs());
        File config=new File(folder,"settings.toml");Files.write(config.toPath(),"[video]\ngpu_backend = \"vulkan\"\nrenderer = \"hardware\"\ninternal_scale = 1\n[cpu]\nfast_boot = true\n[runtime]\nlogging_enabled = true\nlog_level = 2\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        AtomicReference<String> notice=new AtomicReference<>("");ArmSxBackend backend=new ArmSxBackend(context,p->{},notice::set,logs);
        assertTrue(backend.initialize(new File(folder,"host").getAbsolutePath(),bios.getAbsolutePath(),config.getAbsolutePath()));
        HandlerThread consumer=new HandlerThread("replay-surface-consumer");consumer.start();
        try(ImageReader reader=ImageReader.newInstance(640,480,PixelFormat.RGBA_8888,3)){
            reader.setOnImageAvailableListener(source->{try(var image=source.acquireLatestImage()){}},new Handler(consumer.getLooper()));backend.surface(reader.getSurface());
            Thread vm=new Thread(()->backend.run(game.getAbsolutePath()),"replay-test-vm");vm.start();
            try{
                long deadline=android.os.SystemClock.uptimeMillis()+25000;
                while(backend.frames()<12&&vm.isAlive()&&android.os.SystemClock.uptimeMillis()<deadline)Thread.sleep(20);
                assertTrue(backend.diagnosticReport()+"\n"+notice.get(),backend.frames()>=12);
                backend.pause(true);backend.flushDiagnosticLog(5000);File capture=new File(logs,GpuReplay.FILE_NAME);assertTrue(backend.diagnosticReport(),capture.isFile());
                GpuReplay.Document paused=GpuReplay.read(capture);assertTrue(paused.metadata(),paused.metadata().contains("\"reason\":\"pause\"")||paused.metadata().contains("\"reason\":\"report\""));
                assertTrue(paused.metadata().contains("\"dma\""));assertTrue(paused.metadata().contains("\"cd_data\""));
                backend.stop();vm.join(15000);assertFalse("VM did not finish its stop capture",vm.isAlive());
                GpuReplay.Document stopped=GpuReplay.read(capture);assertArrayEquals("Cleanup must not replace the captured scene",paused.capture().display(),stopped.capture().display());assertArrayEquals(paused.capture().after(),stopped.capture().after());
                assertTrue(backend.diagnosticReport().contains("\"graphicsCapture\""));
            }finally{backend.stop();vm.join(15000);backend.surface(null);}
        }finally{consumer.quitSafely();consumer.join(3000);}
    }
}
