package dev.psxlab.core;
import org.junit.Test;
import static dev.psxlab.core.CdRomTest.*;
import static org.junit.Assert.*;

public class CdDriveTimingTest {
    @Test public void nativeStatusPollingUsesEventBoundariesAndKeepsBankAndResultCoherent(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.setEventScheduling(true);b.write8(BASE+1,1);long before=b.deviceDispatches;
            for(int i=0;i<10000;i++){b.nativeWrite(BASE,1,1,8,0);b.nativeRead(BASE+3,1,8,0);}
            assertEquals(3,(int)b.nativeRead(BASE+3,1,0,0)&7);
            assertEquals(16,(int)b.nativeRead(BASE+1,1,0,0));
            assertTrue("Status polling must not dispatch all devices on every read",b.deviceDispatches-before<2500);
        }
    }
    @Test public void explicitSeekCompletesBeforeReadWithoutSeekingTwice(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,2,0,2,4);ack(b);command(b,0x15);assertEquals(3,irq(b));assertEquals(0x42,response(b)[0]);ack(b);
            b.cdrom.tick(10000);assertEquals(0,irq(b));assertEquals(0,b.cdrom.sectorsRead);awaitIrq(b,2);assertEquals(2,response(b)[0]);ack(b);
            command(b,6);assertEquals(0x22,response(b)[0]);ack(b);b.cdrom.tick(SECTOR-1);assertEquals(0,b.cdrom.sectorsRead);b.cdrom.tick(1);assertEquals(1,b.cdrom.sectorsRead);
        }
    }
    @Test public void speedChangeDelaysFirstDataButPreservesSubsequentSectorCadence(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x0e,128);ack(b);command(b,6);ack(b);b.cdrom.tick(CdDriveTiming.HZ/2);assertEquals(0,b.cdrom.sectorsRead);
            firstSector(b);ack(b);b.cdrom.tick(SECTOR/2-1);assertEquals(1,b.cdrom.sectorsRead);b.cdrom.tick(1);assertEquals(2,b.cdrom.sectorsRead);
        }
    }
    @Test public void getIdReplacesTocReplyWhilePhysicalTocReadStillTakesTime(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x1e);ack(b);command(b,0x1a);ack(b);awaitIrq(b,2);assertEquals(8,response(b).length);ack(b);
            command(b,6);ack(b);b.cdrom.tick(CdDriveTiming.TOC/2);assertEquals(0,b.cdrom.sectorsRead);firstSector(b);assertEquals(1,irq(b));
        }
    }
    @Test public void repeatedReadCommandDoesNotResetActiveStream(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,0,0);ack(b);command(b,6);assertEquals(0x22,response(b)[0]);ack(b);
            b.cdrom.tick(SECTOR);assertEquals(2,b.cdrom.sectorsRead);assertEquals(1,irq(b));
        }
    }
    @Test public void oldStateWithoutMechanicalFieldsLoadsWithNoPendingMotion()throws Exception{
        try(Gpu a=new Gpu(false);Gpu g=new Gpu(false)){
            Bus original=new Bus(a),restored=new Bus(g);StateCodec.Node state=StateCodec.capture(original.cdrom);
            for(String field:new String[]{"driveTicks","seekTicks","seekTarget","readAfterSeek","explicitSeek","seekReady"})state.fields.remove(field);
            StateCodec.apply(state,restored.cdrom);assertEquals(original.cdrom.diagnostic(),restored.cdrom.diagnostic());
        }
    }
}
