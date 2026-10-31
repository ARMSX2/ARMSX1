package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class MemoryTimingTest {
    @Test public void biosUsesFourEightBitTransfers(){
        assertEquals(6,MemoryTiming.readWait(0x0013243f,0x00031125,1));
        assertEquals(12,MemoryTiming.readWait(0x0013243f,0x00031125,2));
        assertEquals(24,MemoryTiming.readWait(0x0013243f,0x00031125,4));
        assertEquals(32,MemoryTiming.readWait(0x0013243f,0x00001325,4));
    }
    @Test public void busWidthAndCommonPeriodsChangeTheReadCost(){
        assertEquals(12,MemoryTiming.readWait(0x0013343f,0x00031125,4));
        assertEquals(5,MemoryTiming.readWait(0,0,1));
        assertEquals(11,MemoryTiming.readWait(0,0,4));
        assertEquals(43,MemoryTiming.readWait(0x800,0x8000,4));
    }
}
