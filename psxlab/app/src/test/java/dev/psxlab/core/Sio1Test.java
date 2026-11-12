package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public final class Sio1Test {
    @Test public void ridgeRacerControlWriteDoesNotThrow() {
        Sio1 sio=new Sio1();
        sio.write(0x0a,2,0x0003);
        assertEquals(3,sio.read(0x0a,2));
        assertEquals(5,sio.read(0x04,2)&5);
    }
    @Test public void modeBaudAndReset() {
        Sio1 sio=new Sio1();
        sio.write(8,2,0x1234);
        sio.write(14,2,0x5678);
        assertEquals(0x1234,sio.read(8,2));
        assertEquals(0x5678,sio.read(14,2));
        sio.write(10,2,0x40);
        assertEquals(0,sio.read(8,2));
        assertEquals(0,sio.read(14,2));
        assertEquals(0,sio.read(10,2));
    }
    @Test public void busRoutesSio1WithoutAffectingSio0() {
        Bus bus=new Bus(new Gpu());
        bus.write16(0x1f80105a,3);
        assertEquals(3,bus.read16(0x1f80105a));
        bus.write16(0x1f80105e,0x3456);
        assertEquals(0x3456,bus.read16(0x1f80105e));
    }
}
