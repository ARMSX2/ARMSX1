package dev.psxlab.core;
import org.junit.*;
import org.junit.rules.TemporaryFolder;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import static org.junit.Assert.*;

public class SessionAudioRecorderTest {
    @Rule public TemporaryFolder temp=new TemporaryFolder();
    private static String text(File file)throws IOException{return new String(Files.readAllBytes(file.toPath()),java.nio.charset.StandardCharsets.UTF_8);}
    private static byte[] pcm(short[] samples)throws IOException{ByteArrayOutputStream out=new ByteArrayOutputStream();for(short s:samples){out.write(s);out.write(s>>>8);}return out.toByteArray();}
    private static byte[] readStream(File root,String name)throws IOException{
        File[] parts=root.listFiles((d,n)->n.startsWith(name+"-")&&n.endsWith(".wav"));Arrays.sort(parts,Comparator.comparing(File::getName));ByteArrayOutputStream data=new ByteArrayOutputStream();
        for(File part:parts){byte[] bytes=Files.readAllBytes(part.toPath());assertEquals("RIFF",new String(bytes,0,4,java.nio.charset.StandardCharsets.US_ASCII));int length=(bytes[40]&255)|((bytes[41]&255)<<8)|((bytes[42]&255)<<16)|((bytes[43]&255)<<24);assertEquals(bytes.length-44,length);data.write(bytes,44,length);}return data.toByteArray();
    }
    @Test public void rotatedSegmentsRetainEveryFrameOfBothStreams()throws Exception{
        File root=new File(temp.getRoot(),"session");SessionAudioRecorder r=new SessionAudioRecorder(root,3,128);short[] source={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},processed={-1,-2,-3,-4,-5,-6,-7,-8};
        r.append(false,source,source.length);r.append(true,processed,processed.length);r.close();r.awaitFinished(3000);
        assertArrayEquals(pcm(source),readStream(root,"source"));assertArrayEquals(pcm(processed),readStream(root,"processed"));
        String index=text(new File(root,"segments.tsv"));assertTrue(index.contains("source\tsource-0003.wav\t6\t8"));assertTrue(text(new File(root,"session.txt")).contains("status=COMPLETE"));
    }
    @Test public void checkpointMakesAnActiveRecordingReadableAndRecordingContinues()throws Exception{
        File root=new File(temp.getRoot(),"session");SessionAudioRecorder r=new SessionAudioRecorder(root,20,128);
        try{r.append(false,new short[]{1,2,3,4},4);r.checkpoint();assertArrayEquals(pcm(new short[]{1,2,3,4}),readStream(root,"source"));r.append(false,new short[]{5,6},2);}finally{r.close();r.awaitFinished(3000);}
        assertArrayEquals(pcm(new short[]{1,2,3,4,5,6}),readStream(root,"source"));
    }
    @Test public void diskTraceKeepsEventsAfterTheMemoryRingWraps()throws Exception{
        File root=new File(temp.getRoot(),"session");SessionAudioRecorder r=new SessionAudioRecorder(root,20,128);AudioEventTrace t=new AudioEventTrace(2);t.setSink(r::event);
        for(int i=0;i<50;i++)t.record(i,i,i,i,i,"event="+i);r.close();r.awaitFinished(3000);
        String disk=text(new File(root,"events.tsv"));assertTrue(disk.contains("event=0\n"));assertTrue(disk.contains("event=49\n"));assertEquals(51,disk.lines().count());assertFalse(t.snapshot().contains("event=0\n"));
    }
}
