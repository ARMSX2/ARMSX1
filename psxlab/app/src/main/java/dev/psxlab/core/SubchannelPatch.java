package dev.psxlab.core;
import java.io.IOException;
import java.util.*;

/**
 * Redump SBI subchannel corrections. LibCrypt-protected PAL discs (CTR, Spyro 3, Ape Escape, ...)
 * read the Q subchannel of a few sectors with GetlocP. On a real disc those sectors carry altered Q
 * data with a bad CRC, so the drive keeps reporting the previous valid position; a disc image whose
 * drive reports the sector's own position is treated as a copy and sabotaged.
 */
public final class SubchannelPatch {
    private SubchannelPatch(){}
    private static final int MAX_BYTES=1024*1024;

    /** Q bytes (control/ADR, track, index, relative MSF, zero, absolute MSF; no CRC) by disc LBA. */
    public static Map<Integer,byte[]> parse(byte[] bytes)throws IOException{
        if(bytes.length>MAX_BYTES||bytes.length<4||(bytes.length-4)%14!=0||bytes[0]!='S'||bytes[1]!='B'||bytes[2]!='I'||bytes[3]!=0)
            throw new IOException("Invalid SBI file");
        Map<Integer,byte[]> entries=new HashMap<>();
        for(int at=4;at<bytes.length;at+=14){
            int minute=bcd(bytes[at]),second=bcd(bytes[at+1]),frame=bcd(bytes[at+2]);
            if(second>=60||frame>=75||bytes[at+3]!=1)throw new IOException("Unsupported SBI entry");
            entries.put((minute*60+second)*75+frame-150,Arrays.copyOfRange(bytes,at+4,at+14));
        }
        return entries;
    }
    private static int bcd(byte value)throws IOException{
        int v=value&255;if((v&15)>9||(v>>4)>9)throw new IOException("Invalid BCD in SBI file");return (v>>4)*10+(v&15);
    }
}
