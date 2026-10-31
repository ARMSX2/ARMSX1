package dev.psxlab.core;
import java.io.IOException;
public interface SectorSource {
    /** Raw 2352-byte data/audio sector at a logical disc LBA (00:02:00 is LBA zero). */
    byte[] readSector(int lba)throws IOException;
    int sectorCount();
    /** Track 0 denotes lead-out; track addresses are INDEX 01 disc LBAs. */
    default int lastTrack(){return 1;}
    default int trackStart(int track){if(track==0)return sectorCount();if(track==1)return 0;throw new IllegalArgumentException("No such disc track");}
    default int trackNumber(int lba){return 1;}
    default int trackIndex(int lba){return 1;}
    default int trackRelative(int lba){return Math.max(0,lba);}
    default boolean audioTrack(int lba){return false;}
    /** Emulated disc-region identifier inferred from the executable serial, not lead-in authentication. */
    default char region(){return ' ';}
}
