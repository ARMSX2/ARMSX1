package dev.psxlab.core;

import org.junit.Test;
import java.io.IOException;
import java.util.Map;
import static org.junit.Assert.*;

public class SubchannelPatchTest {
    private static byte[] sbi(int... entry){byte[] bytes=new byte[4+entry.length];bytes[0]='S';bytes[1]='B';bytes[2]='I';for(int i=0;i<entry.length;i++)bytes[4+i]=(byte)entry[i];return bytes;}

    @Test public void entriesAreKeyedByDiscLbaWithTheirQBytes()throws IOException{
        // 03:08:05 is MSF frame (3*60+8)*75+5 = 14105, LBA 13955.
        Map<Integer,byte[]> entries=SubchannelPatch.parse(sbi(0x03,0x08,0x05,1, 0x41,0x01,0x01,0x07,0x06,0x05,0x00,0x23,0x08,0x05));
        assertEquals(1,entries.size());
        assertArrayEquals(new byte[]{0x41,0x01,0x01,0x07,0x06,0x05,0x00,0x23,0x08,0x05},entries.get(13955));
    }
    @Test public void malformedFilesAreRejected(){
        assertThrows(IOException.class,()->SubchannelPatch.parse(new byte[]{'S','B','X',0}));
        assertThrows(IOException.class,()->SubchannelPatch.parse(sbi(0x03,0x08,0x05,1, 0,0,0,0,0)));
        assertThrows(IOException.class,()->SubchannelPatch.parse(sbi(0x03,0x6A,0x05,1, 0,0,0,0,0,0,0,0,0,0)));
        assertThrows(IOException.class,()->SubchannelPatch.parse(sbi(0x03,0x08,0x05,2, 0,0,0,0,0,0,0,0,0,0)));
    }
}
