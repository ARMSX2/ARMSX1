package dev.psxlab.core;
import java.io.IOException;
public interface SectorSource {
    /** Raw 2352-byte sector at a logical data-track LBA (00:02:00 is LBA zero). */
    byte[] readSector(int lba)throws IOException;
    int sectorCount();
    /** Emulated disc-region identifier inferred from the executable serial, not lead-in authentication. */
    default char region(){return ' ';}
}
