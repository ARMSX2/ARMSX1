package dev.psxlab.core;

import org.junit.Test;
import org.junit.Rule;
import org.junit.rules.TemporaryFolder;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import static org.junit.Assert.*;

public class EngineLogTest {
    @Test public void dmaYieldRecordsPointerAndSourceWithoutFloodingLog()throws Exception{
        File dir=temp.newFolder();try(EngineLog log=new EngineLog(dir,new Clock());Gpu gpu=new Gpu(false)){
            log.configure(true,EngineLog.INFO);Bus b=new Bus(gpu);b.dma.setJournal(log);
            b.write32(0x100,0x100);b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a8,0x01000401);
            for(int i=0;i<128;i++)b.dma.tick(1024);assertTrue(log.flush(2000));
            String text=new String(Files.readAllBytes(new File(dir,"engine-current.jsonl").toPath()),java.nio.charset.StandardCharsets.UTF_8);
            assertEquals(9,Arrays.stream(text.split("\n")).filter(line->line.contains("dma.gpu.linked.yield")).count());
            assertTrue(text.contains("Dma.java#transfer/tick"));assertTrue(text.contains("\"next_address\":256"));assertTrue(text.contains("\"last_header\":256"));
        }
    }
    @Rule public TemporaryFolder temp=new TemporaryFolder();
    private static final class Clock implements EngineLog.Clock {
        long ns=1000,cpu=100;
        public long wall(){return ns;}
        public long cpu(){return cpu;}
        public long tid(){return 123;}
    }
    @Test public void disabledDoesNoFileWorkAndCannotRestartAfterClose()throws Exception{
        File dir=new File(temp.getRoot(),"off");EngineLog log=new EngineLog(dir,new Clock());
        assertNull(log.frame(1,2));assertNull(log.watch("audio.dsp",0,1));log.event(5,"ignored",Map.of());assertTrue(log.flush(100));assertFalse(dir.exists());
        log.close();log.configure(true,0);assertFalse(log.enabled());assertFalse(dir.exists());
    }
    @Test public void filteringEscapingAndToggleKeepCompleteRecords()throws Exception{
        File dir=temp.newFolder();try(EngineLog log=new EngineLog(dir,new Clock())){
            log.configure(true,2);log.event(1,"hidden",Map.of());log.event(2,"audio.test",Map.of("message","quote\"\\\n\t","invalid",Double.NaN));assertTrue(log.flush(2000));
            String json=new String(Files.readAllBytes(new File(dir,"engine-current.jsonl").toPath()),java.nio.charset.StandardCharsets.UTF_8);assertFalse(json.contains("hidden"));assertTrue(json.contains("quote\\\"\\\\\\n\\t"));assertTrue(json.contains("\"invalid\":null"));assertTrue(json.contains("\"tid\":123"));assertTrue(json.contains("AudioOutput.java"));
            log.configure(false,2);assertTrue(log.flush(2000));long length=new File(dir,"engine-current.jsonl").length();log.event(5,"off",Map.of());assertTrue(log.flush(2000));assertEquals(length,new File(dir,"engine-current.jsonl").length());
        }
    }
    @Test public void rotationAndOversizeStayBounded()throws Exception{
        File dir=temp.newFolder();try(EngineLog log=new EngineLog(dir,new Clock(),16384)){
            log.configure(true,0);for(int i=0;i<30;i++)log.event(2,"row",Map.of("text","x".repeat(4000)));log.event(2,"too.large",Map.of("a","x".repeat(8000),"b","x".repeat(8000),"c","x".repeat(8000)));assertTrue(log.flush(2000));
            for(File f:Objects.requireNonNull(dir.listFiles()))assertTrue(f.length()<=16384);assertTrue(new File(dir,"engine-previous.jsonl").isFile());assertTrue(((Number)log.summary().get("dropped_records")).longValue()>0);
        }
    }
    @Test public void recordsFramePhasesAndCorrelation()throws Exception{
        Clock clock=new Clock();File dir=temp.newFolder();try(EngineLog log=new EngineLog(dir,clock)){
            log.configure(true,2);EngineLog.Frame frame=log.frame(10,0x80010000);clock.ns+=10;clock.cpu+=5;frame.cpuDone(20,0x80010004);frame.pacing(100,150,800);frame.queued();clock.ns+=20;frame.gpuBegin("Vulkan",new int[8]);
            try(EngineLog.Span span=frame.stage("gpu.present.vulkan")){clock.ns+=200;clock.cpu+=2;}frame.presentation(true);frame.complete();assertEquals(0,log.currentFrame());assertTrue(log.flush(2000));
            Map<?,?> value=(Map<?,?>)log.summary().get("latest_frame");assertEquals("gpu.present.vulkan",value.get("largest_phase"));assertEquals(20L,value.get("gpu_queue_ns"));assertEquals(50L,value.get("sleep_overshoot_ns"));assertEquals(5L,value.get("cpu_active_ns"));assertTrue(value.get("interpretation").toString().contains("not proof"));assertTrue(value.get("presentation_timing_domain").toString().contains("not physical"));
        }
    }
    @Test public void newLaunchSeparatesSessionsAndClearsPriorFrame()throws Exception{
        try(EngineLog log=new EngineLog(temp.newFolder(),new Clock())){
            log.configure(true,2);Object first=log.summary().get("session");EngineLog.Frame frame=log.frame(1,2);frame.cpuDone(2,3);frame.queued();frame.gpuBegin("test",new int[8]);frame.complete();assertFalse(((Map<?,?>)log.summary().get("latest_frame")).isEmpty());
            log.startSession("game");assertNotEquals(first,log.summary().get("session"));assertTrue(((Map<?,?>)log.summary().get("latest_frame")).isEmpty());assertTrue(log.flush(2000));
        }
    }
    @Test public void storageFailureIsReportedWithoutThrowingOnProducer()throws Exception{
        File file=temp.newFile();try(EngineLog log=new EngineLog(file,new Clock())){log.configure(true,2);log.event(4,"error",Map.of());assertFalse(log.flush(2000));assertFalse(log.summary().get("writer_error").toString().isEmpty());}
    }
}
