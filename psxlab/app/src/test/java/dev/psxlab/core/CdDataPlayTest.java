package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;
import static dev.psxlab.core.CdRomTest.*;

public class CdDataPlayTest {
    @Test public void playAtSetlocAdvancesSilentlyWithoutDataInterrupts(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,2,0,2,7);ack(b);
            command(b,3);assertArrayEquals(new int[]{0x82},response(b));ack(b);
            b.cdrom.tick(SECTOR);assertEquals(0,irq(b));assertEquals(0,b.cdrom.audioBufferedFrames());
            assertEquals(0,b.cdrom.audioFrame());assertEquals(0,b.cdrom.availableBytes());assertEquals(0,b.cdrom.dmaBytes);
            command(b,0x11);assertArrayEquals(new int[]{1,1,0,0,7,0,2,7},response(b));ack(b);
            command(b,1);assertArrayEquals(new int[]{0x82},response(b));
        }
    }
    @Test public void pauseStopsPlaybackAndHeadMovement(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,3);ack(b);b.cdrom.tick(SECTOR*3);
            command(b,9);ack(b);CdRomTest.awaitIrq(b,2);assertEquals(2,irq(b));ack(b);
            command(b,0x11);int[] position=response(b);ack(b);b.cdrom.tick(SECTOR*2);
            command(b,0x11);assertArrayEquals(position,response(b));ack(b);command(b,1);assertArrayEquals(new int[]{2},response(b));
        }
    }
    @Test public void trackSelectionAndNoDiscOrBadParametersReturnControllerResponses(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);command(b,3);assertEquals(5,irq(b));assertEquals(0x80,response(b)[1]);ack(b);
            b.cdrom.insert(disc());command(b,3,0,0);assertEquals(5,irq(b));assertEquals(0x20,response(b)[1]);ack(b);
            command(b,3,0xfa);assertEquals(5,irq(b));assertEquals(0x10,response(b)[1]);ack(b);
            command(b,2,0,2,20);ack(b);command(b,3,1);ack(b);b.cdrom.tick(SECTOR);
            command(b,0x11);assertArrayEquals(new int[]{1,1,0,0,0,0,2,0},response(b));
        }
    }
    @Test public void playbackAtLeadoutStopsMotorAndDeliversEndInterrupt(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,2,0,3,0x24);ack(b);command(b,3);ack(b);
            b.cdrom.tick(SECTOR*2);b.cdrom.tick(1);assertEquals(4,irq(b));assertArrayEquals(new int[]{0},response(b));
            ack(b);b.cdrom.tick(SECTOR*3);assertEquals(0,irq(b));
        }
    }
    @Test public void reportModeReturnsPositionAndZeroDataTrackPeak(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x0e,4);ack(b);command(b,2,0,2,9);ack(b);command(b,3);ack(b);
            b.cdrom.tick(SECTOR*6);assertEquals(1,irq(b));ack(b);
            assertArrayEquals(new int[]{0x82,1,1,0,0x80,0x10,0,0},response(b));
        }
    }
    @Test public void scanRequiresPlayAndCanReturnToNormalPlayback(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,4);assertEquals(5,irq(b));assertEquals(0x80,response(b)[1]);ack(b);
            command(b,2,0,2,0x10);ack(b);command(b,3);ack(b);command(b,4);ack(b);b.cdrom.tick(SECTOR);
            command(b,0x11);assertEquals(0x14,response(b)[4]);ack(b);command(b,5);ack(b);b.cdrom.tick(SECTOR);
            command(b,3);ack(b);b.cdrom.tick(SECTOR);command(b,0x11);assertEquals(0x10,response(b)[4]);
        }
    }
}
