package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class ControllerTest {
    private static final int BASE=0x1f801040;
    static void select(Bus b){b.write16(BASE+10,0);b.write16(BASE+8,0x0d);b.write16(BASE+14,0x88);b.write16(BASE+10,0x1003);}
    static int exchange(Bus b,int value){b.write8(BASE,value);b.sio0.tick(1500);assertNotEquals(0,b.read32(BASE+4)&2);int result=b.read8(BASE);b.write16(0x1f801070,0xff7f);b.write16(BASE+10,0x1013);return result;}
    static int[] packet(Bus b,int... bytes){select(b);int[] results=new int[bytes.length];for(int i=0;i<bytes.length;i++)results[i]=exchange(b,bytes[i]);b.write16(BASE+10,0);return results;}
    @Test public void digitalPadRepliesWithIdAndActiveLowButtons(){
        try(Gpu g=new Gpu(false)){PadState p=new PadState();p.update((1<<14)|(1<<3)|(1<<4),128,128,128,128);Bus b=new Bus(g,p);assertArrayEquals(new int[]{255,0x41,0x5a,0xe7,0xbf},packet(b,1,0x42,0,0,0));}
    }
    @Test public void pollSnapshotDoesNotTearDuringButtonUpdate(){
        try(Gpu g=new Gpu(false)){PadState p=new PadState();Bus b=new Bus(g,p);select(b);exchange(b,1);exchange(b,0x42);p.update(0xffff,0,0,0,0);assertEquals(0x5a,exchange(b,0));assertEquals(255,exchange(b,0));assertEquals(255,exchange(b,0));}
    }
    @Test public void digitalStickFallbackAndDisabledStickClicks(){
        PadState p=new PadState();p.update(6,128,128,0,255);assertEquals(0xff3f,p.get().buttonWord(false));assertEquals(0xfff9,p.get().buttonWord(true));
    }
    @Test public void analogModeCanBeEnabledThroughConfiguration(){
        try(Gpu g=new Gpu(false)){PadState p=new PadState();p.update(0,1,2,3,4);Bus b=new Bus(g,p);
            assertEquals(0x41,packet(b,1,0x43,0,1,0)[1]);
            assertArrayEquals(new int[]{255,0xf3,0x5a,0,0,0,0,0,0},packet(b,1,0x44,0,1,0,0,0,0,0));
            packet(b,1,0x43,0,0,0,0,0,0,0);
            assertArrayEquals(new int[]{255,0x73,0x5a,255,255,1,2,3,4},packet(b,1,0x42,0,0,0,0,0,0,0));assertTrue(p.analogMode);
        }
    }
    @Test public void configModeIdentityAndQueriesMatchDualShock(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);packet(b,1,0x43,0,1,0);
            assertArrayEquals(new int[]{255,0xf3,0x5a,1,2,0,2,1,0},packet(b,1,0x45,0,0,0,0,0,0,0));
            assertArrayEquals(new int[]{255,0xf3,0x5a,0,0,1,1,1,20},packet(b,1,0x46,0,1,0,0,0,0,0));
            assertEquals(7,packet(b,1,0x4c,0,1,0,0,0,0,0)[6]);
        }
    }
    @Test public void rumbleMapRoundTripsWithoutClaimingPhysicalOutput(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);packet(b,1,0x43,0,1,0);
            assertArrayEquals(new int[]{255,0xf3,0x5a,255,255,255,255,255,255},packet(b,1,0x4d,0,0,1,255,255,255,255));
            assertArrayEquals(new int[]{255,0xf3,0x5a,0,1,255,255,255,255},packet(b,1,0x4d,0,255,255,255,255,255,255));
        }
    }
    @Test public void manualAnalogToggleRespectsSoftwareLock(){
        try(Gpu g=new Gpu(false)){PadState p=new PadState();Bus b=new Bus(g,p);p.toggleAnalog();assertEquals(0x73,packet(b,1,0x42,0,0,0,0,0,0,0)[1]);packet(b,1,0x43,0,1,0,0,0,0,0);packet(b,1,0x44,0,1,3,0,0,0,0);packet(b,1,0x43,0,0,0,0,0,0,0);p.toggleAnalog();assertEquals(0x73,packet(b,1,0x42,0,0,0,0,0,0,0)[1]);}
    }
    @Test public void emptyPortAndMemoryCardDoNotAcknowledge(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);select(b);b.write16(BASE+10,0x3003);assertEquals(255,exchange(b,1));assertEquals(0,b.sio0.ackPulses);select(b);assertEquals(255,exchange(b,0x81));assertEquals(0,b.sio0.ackPulses);}
    }
    @Test public void lastPacketByteHasNoAckPulse(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);assertArrayEquals(new int[]{255,0x41,0x5a,255,255},packet(b,1,0x42,0,0,0));assertEquals(4,b.sio0.ackPulses);}
    }
    @Test public void serialBaudDelaysReceiveAndAck(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);select(b);b.write8(BASE,1);assertEquals(0,b.read32(BASE+4)&2);b.sio0.tick(1087);assertEquals(0,b.read32(BASE+4)&2);b.sio0.tick(1);assertNotEquals(0,b.read32(BASE+4)&2);assertEquals(0,b.sio0.ackPulses);b.sio0.tick(159);assertEquals(0,b.sio0.ackPulses);b.sio0.tick(1);assertEquals(1,b.sio0.ackPulses);assertNotEquals(0,b.read32(BASE+4)&128);b.sio0.tick(96);assertEquals(0,b.read32(BASE+4)&128);}
    }
    @Test public void ackIrqIsEdgeTriggeredAndAcknowledged(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801074,128);select(b);b.write8(BASE,1);b.sio0.tick(1500);assertTrue(b.interruptPending());assertNotEquals(0,b.read32(BASE+4)&512);b.write16(0x1f801070,0xff7f);b.sio0.tick(1);assertFalse(b.interruptPending());b.write16(BASE+10,0x1013);assertEquals(0,b.read32(BASE+4)&512);}
    }
    @Test public void serialResetClearsPendingTransfersAndReceiveFifo(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);select(b);b.write8(BASE,1);b.sio0.tick(1500);b.write16(BASE+10,64);assertEquals(5,b.read32(BASE+4));assertEquals(0,b.read16(BASE+8));assertEquals(0,b.read16(BASE+10));assertEquals(0,b.read16(BASE+14));}
    }
    @Test public void transmitHoldingRegisterQueuesSecondByte(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);select(b);b.write8(BASE,1);b.write8(BASE,0x42);assertEquals(0,b.read32(BASE+4)&1);b.sio0.tick(3000);assertEquals(255,b.read8(BASE));assertEquals(0x41,b.read8(BASE));assertEquals(2,b.sio0.bytesTransferred);}
    }
    @Test public void sixteenBitDataReadPreviewsButPopsOneByte(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);select(b);b.write8(BASE,1);b.write8(BASE,0x42);b.sio0.tick(3000);assertEquals(0x41ff,b.read16(BASE));assertEquals(0x41,b.read8(BASE));}
    }
    @Test public void wideRegisterAccessesCombineModeControlAndBaud(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(BASE+8,0x1003000d);assertEquals(0x1003000d,b.read32(BASE+8));b.write32(BASE+12,0x00880000);assertEquals(0x88,b.read16(BASE+14));}
    }
    @Test public void padStateClampsAnalogRangeAndClearsAllInput(){
        PadState p=new PadState();p.update(0x1ffff,-1,300,500,-20);assertEquals(0,p.get().rightX());assertEquals(255,p.get().rightY());p.clear();assertEquals(0,p.get().pressed());assertEquals(128,p.get().leftX());
    }
}
