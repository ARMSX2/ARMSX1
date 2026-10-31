package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class VideoClockTest {
    @Test public void vblankRaisesInterruptAndFieldsAdvance(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801074,1);g.tickVideo(b,1);assertFalse(b.interruptPending());g.tickVideo(b,570000);assertTrue(b.interruptPending());assertEquals(1,g.fields);}
    }
    @Test public void hblankClockAdvancesRootCounterOne(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801114,0x100);g.tickVideo(b,22000);int lines=b.read16(0x1f801110);assertTrue(lines>=10 && lines<=12);}
    }
    @Test public void palHasLongerFieldPeriod(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);g.control(0x08000008);g.tickVideo(b,570000);assertEquals(0,g.fields);g.tickVideo(b,120000);assertEquals(1,g.fields);}
    }
    @Test public void dotClockFeedsRootCounterZero(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801104,0x100);g.tickVideo(b,1000);int dots=b.read16(0x1f801100);assertTrue(dots>=158 && dots<=159);}
    }
}
