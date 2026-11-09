package dev.psxlab.core;

import dev.psxlab.ArmSxBackend;
import org.junit.Test;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;
import static org.junit.Assert.*;

/** Exercise the real startup formatter with mounted media, including CUE's null byte-source field. */
public class MediaDiagnosticsTest {
    private static final Class<?> MEDIA;
    private static final Method LAYOUT;
    static {
        try {
            MEDIA=Class.forName("dev.psxlab.ArmSxBackend$Media");
            LAYOUT=ArmSxBackend.class.getDeclaredMethod("discLayout",MEDIA);LAYOUT.setAccessible(true);
        } catch(ReflectiveOperationException e){throw new ExceptionInInitializerError(e);}
    }
    private static final class MemorySource implements ByteSource {
        final byte[] bytes;int reads,closes;
        MemorySource(byte[] bytes){this.bytes=bytes;}
        public long length(){return bytes.length;}
        public byte[] read(long offset,int length)throws IOException{
            if(offset<0||offset+length>bytes.length)throw new EOFException();reads++;
            return Arrays.copyOfRange(bytes,(int)offset,(int)offset+length);
        }
        public void close(){closes++;}
    }
    private static AutoCloseable mount(Class<?> type,Object source)throws Exception{
        return mount(type,source,true);
    }
    private static AutoCloseable mount(Class<?> type,Object source,boolean boot)throws Exception{
        Constructor<?> constructor=MEDIA.getDeclaredConstructor(type,boolean.class);constructor.setAccessible(true);
        return (AutoCloseable)constructor.newInstance(source,boot);
    }
    private static String layout(Object media)throws Exception{return (String)LAYOUT.invoke(null,media);}
    @Test public void cueDataTrackDoesNotDereferenceTheAbsentByteSource()throws Exception{
        MemorySource source=new MemorySource(BootTest.iso());
        CueDisc cue=new CueDisc("FILE data.bin BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00",name->source);
        try(AutoCloseable media=mount(CueDisc.class,cue)){
            Field bytes=MEDIA.getDeclaredField("source");bytes.setAccessible(true);assertNull(bytes.get(media));
            int reads=source.reads;String text=layout(media);
            assertTrue(text,text.contains("CUE"));assertTrue(text,text.contains("1 track(s), 32 sectors"));
            assertTrue(text,text.contains("0 audio track(s)"));assertEquals(reads,source.reads);assertEquals(0,source.closes);
        }
        assertEquals(1,source.closes);
    }
    @Test public void cueAudioTracksRemainInTheStartupAndDiscChangeDescription()throws Exception{
        MemorySource data=new MemorySource(BootTest.iso()),audio=new MemorySource(new byte[2*2352]);
        CueDisc cue=new CueDisc("FILE data.bin BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\nFILE audio.bin BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00",name->name.equals("data.bin")?data:audio);
        try(AutoCloseable media=mount(CueDisc.class,cue)){
            String text=layout(media);assertTrue(text,text.contains("2 track(s), 34 sectors"));
            assertTrue(text,text.contains("2 audio @32"));assertTrue(text,text.contains("1 audio track(s)"));
        }
    }
    @Test public void ordinaryIsoKeepsItsByteSourceAndDataTrackDescription()throws Exception{
        try(AutoCloseable media=mount(ByteSource.class,new MemorySource(BootTest.iso()))){
            String text=layout(media);assertTrue(text,text.startsWith("MemorySource"));
            assertTrue(text,text.contains("1 track(s), 32 sectors"));assertTrue(text,text.contains("0 audio track(s)"));
        }
    }
    @Test public void standaloneExecutableDoesNotNeedDiscMetadata()throws Exception{
        try(AutoCloseable media=mount(ByteSource.class,new MemorySource(BootTest.executable()))){assertEquals("none (executable)",layout(media));}
    }
    @Test public void dataOnlyDiscCanBeInsertedWithoutABootExecutable()throws Exception{
        byte[] iso=BootTest.iso();
        Arrays.fill(iso,20*2048,21*2048,(byte)0);
        assertThrows(InvocationTargetException.class,()->mount(ByteSource.class,new MemorySource(iso)));
        try(AutoCloseable media=mount(ByteSource.class,new MemorySource(iso),false)){
            assertTrue(layout(media).contains("1 track(s), 32 sectors"));
        }
    }
    @Test public void biosOnlyLaunchDoesNotNeedMountedMedia()throws Exception{assertEquals("none (executable)",layout(null));}
}
