package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

/** Native entry boundaries must retain pending time and react to MMIO schedule changes. */
public class NativeDeadlineTest {
    private static int remaining(long status){return (int)((status>>>32)&0x1fffffff);}
    @Test public void timerProgrammingReschedulesTheNextNativeEvent(){
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);bus.setEventScheduling(true);
            bus.nativeClock(32,0);
            bus.nativeWrite(0x1f801074,4,1<<4,0,0);
            bus.nativeWrite(0x1f801108,2,128,0,0);
            long programmed=bus.nativeWrite(0x1f801104,2,0x18,0,0);
            assertTrue(remaining(programmed)<=128);assertFalse(bus.interruptPending());
            bus.nativeClock(128,0);assertTrue("Timer target IRQ must not be delayed by a stale cached deadline",bus.interruptPending());
            assertEquals(0,bus.timers.read(0x1f801100));
        }
    }
    @Test public void returningToJavaDoesNotDiscardPendingClocks(){
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);bus.setEventScheduling(true);bus.nativeClock(32,0);
            long start=bus.nativeFinish(0,0);int deadline=remaining(start);assertTrue(deadline>=32);
            long finish=bus.nativeFinish(7,0);assertEquals(deadline-7,remaining(finish));
            long resumed=bus.nativeClock(0,0);assertEquals(remaining(finish),remaining(resumed));
            bus.flushClocks();assertEquals(39,bus.timers.read(0x1f801100));
        }
    }
    @Test public void changingTimerClockSourceInvalidatesTheCachedSchedule(){
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);bus.setEventScheduling(true);bus.nativeClock(32,0);
            bus.nativeWrite(0x1f801128,2,16,0,0);
            long divided=bus.nativeWrite(0x1f801124,2,0x218,0,0);
            bus.nativeWrite(0x1f801074,4,1<<6,0,0);
            bus.nativeClock(64,0);assertFalse(bus.interruptPending());
            bus.nativeClock(64,0);assertTrue(bus.interruptPending());
            assertTrue(remaining(divided)<=128);
        }
    }
}
