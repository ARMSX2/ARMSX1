package dev.psxlab.core;

import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.GZIPOutputStream;
import static org.junit.Assert.*;

public class GpuReplayTest {
    private static EngineLog logger(File directory){
        EngineLog log=new EngineLog(directory,new EngineLog.Clock(){public long wall(){return System.nanoTime();}public long cpu(){return 0;}public long tid(){return 1;}});log.configure(true,EngineLog.INFO);return log;
    }
    private static void send(Gpu gpu,int... words){for(int word:words)gpu.write(word);}
    private static GpuReplay.Capture scene(Gpu gpu){
        gpu.control(0x03000000);gpu.control(0x08000001);gpu.control(0x06000000|600|(3160<<12));gpu.control(0x07000000|16|(256<<10));
        send(gpu,0xe3000000,0xe407ffff,0xa0000000,512|(300<<16),0x10002,0x03e0001f);
        gpu.completeFrameWithoutPresentation(frame->{},null);gpu.sync();
        send(gpu,0x020000ff,0,0x40020,0x6000ff00,2|(2<<16),5|(3<<16));
        send(gpu,0xe6000001,0xa0000000,10|(5<<16),0x10003,0x001f7c00,0x000003e0,0xe6000000);
        send(gpu,0x80000000,10|(5<<16),20|(6<<16),0x10003);
        send(gpu,0xe1000118,0x65000000,30|(10<<16),44<<8,2|(1<<16));
        send(gpu,0x480000ff,2|(20<<16),12|(20<<16),12|(25<<16),0x50005000);
        gpu.completeFrameWithoutPresentation(frame->{},null);return gpu.captureGraphics();
    }
    @Test public void softwareAndThreadedCapturesReplayTheActualCommandsAndDisplay()throws Exception{
        for(boolean threaded:new boolean[]{false,true})try(EngineLog log=logger(Files.createTempDirectory("gpu-replay").toFile());Gpu gpu=new Gpu(threaded)){
            gpu.setJournal(log);GpuReplay.Capture capture=scene(gpu);assertNotNull(capture);
            assertArrayEquals(capture.after(),GpuReplay.replay(capture));
            Set<Integer> kinds=new HashSet<>();for(GpuReplay.Operation operation:capture.operations())kinds.add(operation.kind());
            assertTrue(kinds.toString(),kinds.containsAll(List.of(GpuReplay.BATCH,GpuReplay.FILL,GpuReplay.COPY,GpuReplay.UPLOAD,GpuReplay.FRAME)));
            assertEquals(0x801f,capture.after()[6*1024+21]);assertEquals(0x83e0,capture.after()[6*1024+22]);
            assertEquals(31,capture.after()[10*1024+30]);assertEquals(0x3e0,capture.after()[10*1024+31]);
            Gpu.Frame frame=gpu.displayFrame();GpuReplay.Picture picture=GpuReplay.picture(capture.after(),capture.display());
            assertEquals(frame.width(),picture.width());assertEquals(frame.height(),picture.height());assertArrayEquals(frame.pixels(),picture.pixels());
        }
    }
    @Test public void compressedAttachmentRoundTripsWithoutLogTailTruncation()throws Exception{
        File folder=Files.createTempDirectory("gpu-replay-report").toFile(),captureFile=new File(folder,GpuReplay.FILE_NAME);
        try(EngineLog log=logger(folder);Gpu gpu=new Gpu(false)){
            gpu.setJournal(log);GpuReplay.Capture capture=scene(gpu);String metadata="{\"game_id\":\"test\",\"label\":\"サンプル\"}";
            GpuReplay.write(captureFile,metadata,capture);GpuReplay.Document decoded=GpuReplay.read(captureFile);
            assertEquals(metadata,decoded.metadata());assertArrayEquals(capture.before(),decoded.capture().before());assertArrayEquals(capture.after(),GpuReplay.replay(decoded.capture()));
            StringWriter report=new StringWriter();GpuReplayReport.append(report,captureFile);String text=report.toString();
            String encoded=text.substring(text.indexOf(GpuReplayReport.BEGIN)+GpuReplayReport.BEGIN.length(),text.indexOf(GpuReplayReport.END)).replaceAll("\\s","");
            assertArrayEquals(Files.readAllBytes(captureFile.toPath()),Base64.getDecoder().decode(encoded));assertTrue(text.contains("sha256="));
        }
    }
    @Test public void guestMemoryRoundTripsAndVersionOneCapturesStillRead()throws Exception{
        File folder=Files.createTempDirectory("gpu-replay-memory").toFile(),captureFile=new File(folder,GpuReplay.FILE_NAME);
        for(boolean nativeRam:new boolean[]{false,true})try(EngineLog log=logger(folder);Gpu gpu=new Gpu(false)){
            gpu.setJournal(log);GpuReplay.Capture capture=scene(gpu);
            Bus bus=new Bus(gpu,new PadState(),false);if(nativeRam)bus.enableNativeMemory();
            bus.write32(0x800bede4,0x1000ffff);bus.write32(0x801ffffc,0xcafef00d);bus.write32(0x1f800000,0x12345678);bus.write32(0x1f8003fc,0x0badf00d);
            // Park a block high in sound RAM the way Linda Cube stores files there, then capture it.
            bus.spu.write(0x1f801da6,0x7B560/8);for(int i=0;i<4;i++)bus.spu.writeTransfer(0x1234+i);bus.spu.write(0x1f801d80,0x3fff);
            GpuReplay.write(captureFile,"{}",capture,bus.snapshotRam(),bus.snapshotScratchpad(),bus.spu.snapshotRam(),bus.spu.snapshotRegisters());GpuReplay.Document decoded=GpuReplay.read(captureFile);
            assertNotNull(decoded.spuRam());assertEquals(GpuReplay.SPU_RAM_BYTES,decoded.spuRam().length);
            java.nio.ByteBuffer spu=java.nio.ByteBuffer.wrap(decoded.spuRam()).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            for(int i=0;i<4;i++)assertEquals(0x1234+i,spu.getShort(0x7B560+i*2)&65535);
            assertEquals("main volume left register",0x3fff,decoded.spuRegisters()[(0x1f801d80-0x1f801c00)/2]);
            assertEquals("transfer address register",0x7B560/8,decoded.spuRegisters()[(0x1f801da6-0x1f801c00)/2]);
            assertNotNull(decoded.ram());assertEquals(GpuReplay.RAM_BYTES,decoded.ram().length);assertEquals(GpuReplay.SCRATCHPAD_BYTES,decoded.scratchpad().length);
            java.nio.ByteBuffer ram=java.nio.ByteBuffer.wrap(decoded.ram()).order(java.nio.ByteOrder.LITTLE_ENDIAN),scratch=java.nio.ByteBuffer.wrap(decoded.scratchpad()).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            assertEquals(0x1000ffff,ram.getInt(0x0bede4));assertEquals(0xcafef00d,ram.getInt(0x1ffffc));assertEquals(0x12345678,scratch.getInt(0));assertEquals(0x0badf00d,scratch.getInt(0x3fc));
            assertArrayEquals(capture.after(),GpuReplay.replay(decoded.capture()));
            StringWriter report=new StringWriter();GpuReplayReport.append(report,captureFile);assertTrue(report.toString().contains("graphics replay v3"));
            try{GpuReplay.write(captureFile,"{}",capture,new byte[16],bus.snapshotScratchpad());fail("Partial RAM accepted");}catch(IOException expected){}
        }
        // A capture written before memory snapshots existed: version 1, nothing after the final VRAM.
        File old=new File(folder,"version-one.armgfx");
        try(DataOutputStream out=new DataOutputStream(new GZIPOutputStream(new FileOutputStream(old)))){
            out.writeLong(0x41524d5358475055L);out.writeInt(1);out.writeUTF("{}");for(int i=0;i<4;i++)out.writeLong(0);
            out.writeInt(0);out.writeInt(0);out.writeInt(0);out.writeInt(600);out.writeInt(3160);out.writeInt(16);out.writeInt(256);out.writeInt(0);for(int i=0;i<7;i++)out.writeInt(0);
            for(int i=0;i<GpuReplay.VRAM_WORDS;i++)out.writeShort(0);out.writeInt(0);for(int i=0;i<GpuReplay.VRAM_WORDS;i++)out.writeShort(0);
        }
        GpuReplay.Document legacy=GpuReplay.read(old);assertNull(legacy.ram());assertNull(legacy.scratchpad());assertEquals(0,legacy.capture().operations().size());
    }
    @Test public void checkpointRetainsThePreviousWindowAndFreezeDoesNotFollowLaterChanges(){
        int[] vram=new int[GpuReplay.VRAM_WORDS],display={0,0,0,0,320,0,1,0},environment=new int[7];GpuReplayRecorder recorder=new GpuReplayRecorder(vram,1,0);
        recorder.add(GpuReplay.FILL,new int[]{255,0,0x10010});Arrays.fill(vram,0,16,31);recorder.checkpoint(vram,display.clone(),environment.clone(),2,2_000_000_000L);
        recorder.add(GpuReplay.FILL,new int[]{0xff00,16,0x10010});Arrays.fill(vram,16,32,0x3e0);
        GpuReplay.Capture capture=recorder.capture(vram,display.clone(),environment.clone(),3,3_000_000_000L);Arrays.fill(vram,0x7fff);
        assertEquals(1,capture.firstFrame());assertEquals(2,capture.operations().size());assertArrayEquals(capture.after(),GpuReplay.replay(capture));assertEquals(31,capture.after()[0]);assertEquals(0,capture.before()[0]);
    }
    @Test public void captureIsOffWithoutLoggingAndHostBuffersStayOutOfGuestStates()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            gpu.completeFrameWithoutPresentation(frame->{},null);assertNull(gpu.captureGraphics());
            File folder=Files.createTempDirectory("gpu-replay-state").toFile();try(EngineLog log=logger(folder)){
                gpu.setJournal(log);scene(gpu);StateCodec.Node state=StateCodec.capture(gpu);
                for(String field:List.of("replayRecorder","replayDisplay","replayFrame"))assertFalse(state.fields.containsKey(field));
                log.configure(false,EngineLog.INFO);gpu.completeFrameWithoutPresentation(frame->{},null);assertNull(gpu.captureGraphics());
            }
        }
    }
    @Test public void corruptAndOversizedCaptureInputsAreRejected()throws Exception{
        File file=Files.createTempFile("gpu-replay-invalid",".armgfx").toFile();
        try(DataOutputStream out=new DataOutputStream(new GZIPOutputStream(new FileOutputStream(file)))){
            out.writeLong(0x41524d5358475055L);out.writeInt(1);out.writeUTF("{}");for(int i=0;i<4;i++)out.writeLong(0);for(int i=0;i<15;i++)out.writeInt(0);for(int i=0;i<GpuReplay.VRAM_WORDS;i++)out.writeShort(0);
            out.writeInt(1);out.writeInt(GpuReplay.BATCH);out.writeInt(GpuReplay.MAX_WORDS+1);
        }
        try{GpuReplay.read(file);fail("Oversized command must be rejected before allocation");}catch(IOException expected){assertTrue(expected.getMessage().contains("Oversized"));}
        Files.write(file.toPath(),new byte[]{1,2,3});try{GpuReplay.read(file);fail("Truncated capture accepted");}catch(IOException expected){}
    }
    @Test public void movieHeaderDiagnosticsRetainDmaResultsAndAreExcludedFromSaveStates()throws Exception{
        try(EngineLog log=logger(Files.createTempDirectory("movie-header-trace").toFile());Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);bus.dma.setJournal(log);CdRomTest.readFirst(bus,9,0);bus.write8(CdRomTest.BASE+3,0x80);
            bus.write32(0x1f8010f0,0x0765c321);bus.write32(0x1f8010b0,0x10000);bus.write32(0x1f8010b4,4);bus.write32(0x1f8010b8,0x11000000);
            @SuppressWarnings("unchecked") List<int[]> transfers=(List<int[]>)bus.dma.loggingSnapshot().get("transfers");
            int[] completed=transfers.get(transfers.size()-1);assertEquals(1,completed[1]);assertEquals(3,completed[2]);assertEquals(0x0c0b0a09,completed[9]);
            assertEquals(16,bus.cdrom.dataSnapshot().get("fifo_position"));assertEquals(2032,bus.cdrom.dataSnapshot().get("fifo_remaining"));
            assertEquals(0x0c0b0a09,((int[])bus.cdrom.dataSnapshot().get("accepted_fifo_header_words"))[0]);
            StateCodec.Node dma=StateCodec.capture(bus.dma);assertFalse(dma.fields.containsKey("traceTransfers"));assertFalse(dma.fields.containsKey("traceSequence"));
        }
    }
}
