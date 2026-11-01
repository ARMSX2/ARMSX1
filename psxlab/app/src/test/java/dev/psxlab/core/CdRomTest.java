package dev.psxlab.core;
import org.junit.Test;
import java.io.*;
import java.util.*;
import static org.junit.Assert.*;

public class CdRomTest {
    @Test public void openingTrayStopsDmaAndReadAndSignalsShellOpenUntilReplacement(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,7,0);b.write8(BASE+3,0x80);assertEquals(2048,b.cdrom.availableBytes());
            long before=b.cdrom.sectorsRead;b.cdrom.eject();assertEquals(0,b.cdrom.availableBytes());b.cdrom.tick(1);
            assertEquals(5,irq(b));assertArrayEquals(new int[]{0x10,8},response(b));ack(b);
            b.cdrom.tick(33868800);assertEquals(before,b.cdrom.sectorsRead);
            command(b,1);assertArrayEquals(new int[]{0x10},response(b));ack(b);
            command(b,6);assertEquals(5,irq(b));ack(b);response(b);
            b.cdrom.insert(disc());command(b,1);assertArrayEquals(new int[]{2},response(b));
        }
    }
    @Test public void rawScexTestAtDataPositionReturnsNoStringsWithoutChangingDiscId(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);SectorSource data=disc();b.cdrom.insert(new SectorSource(){
                public byte[] readSector(int at)throws IOException{return data.readSector(at);}
                public int sectorCount(){return data.sectorCount();}public char region(){return 'A';}
            });
            command(b,0x19,5);ack(b);assertArrayEquals(new int[]{1,1},response(b));
            command(b,0x19,4);assertEquals(3,irq(b));ack(b);assertArrayEquals(new int[]{2},response(b));
            b.cdrom.tick(33868800);command(b,0x19,5);ack(b);assertArrayEquals(new int[]{0,0},response(b));
            command(b,0x1a);ack(b);awaitIrq(b,2);assertEquals(2,irq(b));
            ack(b);assertArrayEquals(new int[]{2,0,32,0,'S','C','E','A'},response(b));
        }
    }
    @Test public void scexTestWithoutDiscDoesNotThrowOrInventStrings(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);command(b,0x19,4);assertEquals(3,irq(b));ack(b);assertArrayEquals(new int[]{18},response(b));
            command(b,0x19,5);ack(b);assertArrayEquals(new int[]{0,0},response(b));
        }
    }
    @Test public void acknowledgeBeforeReadingKeepsUnreadResponseBytes(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x13);assertEquals(3,irq(b));
            ack(b);assertEquals(0,irq(b));assertNotEquals(0,b.read8(BASE)&32);
            assertArrayEquals(new int[]{2,1,1},response(b));assertEquals(0,b.read8(BASE)&32);
            command(b,0x0f);assertArrayEquals(new int[]{2,0,0,0,0},response(b));
        }
    }
    @Test public void partialInterruptAckAndParameterResetPreserveResponseCursor(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x13);assertEquals(2,b.read8(BASE+1));
            b.write8(BASE+2,99);b.write8(BASE,1);b.write8(BASE+3,0x41);b.write8(BASE,0);
            assertEquals(2,irq(b));assertEquals(0x18,b.read8(BASE)&0x18);
            ack(b);assertArrayEquals(new int[]{1,1},response(b));
        }
    }
    @Test public void busyCommandIsReplacedAndOnlyLatestParametersAreExecuted(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());b.write8(BASE+2,0xa0);b.write8(BASE+1,0x0e);
            b.cdrom.tick(CdRom.COMMAND_TICKS-1);
            b.write8(BASE+2,3);b.write8(BASE+2,7);b.write8(BASE+1,0x0d);
            b.cdrom.tick(1);assertEquals(0,irq(b));assertNotEquals(0,b.read8(BASE)&128);
            b.cdrom.tick(CdRom.COMMAND_TICKS-1);assertEquals(3,irq(b));assertArrayEquals(new int[]{2},response(b));
            assertEquals(0,b.read8(BASE)&128);assertEquals(1,b.cdrom.commandOverwrites);
            ack(b);command(b,0x0f);assertArrayEquals(new int[]{2,0,0,3,7},response(b));
            ack(b);b.cdrom.tick(CdRom.COMMAND_TICKS*2);assertEquals(0,irq(b));
        }
    }
    @Test public void replacementWaitsForExistingInterruptWithoutLosingItsResponse(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);command(b,1); // Unread Nop response is still pending.
            b.write8(BASE+1,0xff);b.write8(BASE+1,0x0f);b.cdrom.tick(CdRom.COMMAND_TICKS*2);
            assertEquals(3,irq(b));assertArrayEquals(new int[]{16},response(b));assertNotEquals(0,b.read8(BASE)&128);
            ack(b);b.cdrom.tick(1);assertEquals(3,irq(b));assertArrayEquals(new int[]{16,0,0,0,0},response(b));
            ack(b);b.cdrom.tick(CdRom.COMMAND_TICKS*2);assertEquals(0,irq(b));assertEquals(1,b.cdrom.commandOverwrites);
        }
    }
    @Test public void repeatedBusyWritesUseOneCommandLatchInsteadOfAQueue(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);for(int i=0;i<40;i++)b.write8(BASE+1,0xff);b.write8(BASE+1,1);
            b.cdrom.tick(CdRom.COMMAND_TICKS);assertEquals(3,irq(b));assertArrayEquals(new int[]{16},response(b));
            assertEquals(40,b.cdrom.commandOverwrites);ack(b);b.cdrom.tick(CdRom.COMMAND_TICKS*2);assertEquals(0,irq(b));
        }
    }
    @Test public void repeatedDataRequestDoesNotRewindPartialSector(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);readFirst(b,7,32);b.write8(BASE+3,0x80);for(int i=0;i<12;i++)b.read8(BASE+2);assertEquals(2328,b.cdrom.availableBytes());b.write8(BASE+3,0x80);assertEquals(2328,b.cdrom.availableBytes());assertEquals(7,b.read8(BASE+2));}
    }
    @Test public void responsePaddingWrapsAndAckDrainsIt(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.cdrom.insert(disc());command(b,1);assertEquals(2,b.read8(BASE+1));for(int i=1;i<16;i++)assertEquals(0,b.read8(BASE+1));assertEquals(2,b.read8(BASE+1));assertEquals(0,b.read8(BASE)&32);ack(b);assertEquals(0,b.read8(BASE+1));}
    }
    @Test public void dataOverreadReturnsHardwarePaddingByte(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);readFirst(b,7,0);b.write8(BASE+3,0x80);for(int i=0;i<2048;i++)b.read8(BASE+2);assertEquals((7+2040)&255,b.read8(BASE+2));assertEquals((7+2040)&255,b.read8(BASE+2));}
    }
    @Test public void fastLoadInitializesCdIrqMaskLikeDiscBoot(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.cdrom.insert(disc());b.cdrom.prepareBootHandoff();assertEquals(0,b.read8(BASE)&3);assertEquals(31,b.read8(BASE+3)&31);}
    }
    static final int BASE=0x1f801800,SECTOR=33868800/75;
    static void command(Bus b,int command,int... p){b.write8(BASE,0);for(int v:p)b.write8(BASE+2,v);b.write8(BASE+1,command);b.cdrom.tick(CdRom.COMMAND_TICKS);}
    static int irq(Bus b){b.write8(BASE,1);int value=b.read8(BASE+3)&7;b.write8(BASE,0);return value;}
    static void ack(Bus b){b.write8(BASE,1);b.write8(BASE+3,31);b.write8(BASE,0);}
    static int[] response(Bus b){ArrayList<Integer> list=new ArrayList<>();while((b.read8(BASE)&32)!=0)list.add(b.read8(BASE+1));return list.stream().mapToInt(Integer::intValue).toArray();}
    static SectorSource disc(){return new SectorSource(){
        public int sectorCount(){return 100;}
        public char region(){return 'A';}
        public byte[] readSector(int lba)throws IOException{
            if(lba<0 || lba>=100)throw new EOFException("Outside test image");
            byte[] raw=new byte[2352];Arrays.fill(raw,1,11,(byte)255);int at=lba+150;raw[12]=(byte)CdRom.bcd(at/4500);raw[13]=(byte)CdRom.bcd(at/75%60);raw[14]=(byte)CdRom.bcd(at%75);raw[15]=2;
            for(int i=24;i<2072;i++)raw[i]=(byte)(lba+i-24);return raw;
        }
    };}
    static void awaitIrq(Bus b,int expected){for(int i=0;i<64 && irq(b)==0;i++)b.cdrom.tick(b.cdrom.clocksToEvent());assertEquals(expected,irq(b));}
    static void firstSector(Bus b){long before=b.cdrom.sectorsRead;for(int i=0;i<64 && b.cdrom.sectorsRead==before;i++)b.cdrom.tick(b.cdrom.clocksToEvent());assertEquals(before+1,b.cdrom.sectorsRead);}
    static void readFirst(Bus b,int lba,int mode){
        b.cdrom.insert(disc());command(b,0x0e,mode);ack(b);int frames=lba+150;command(b,2,CdRom.bcd(frames/4500),CdRom.bcd(frames/75%60),CdRom.bcd(frames%75));ack(b);command(b,6);assertEquals(3,irq(b));ack(b);firstSector(b);assertEquals(1,irq(b));
    }
    @Test public void registerBanksBusyAndScheduledResponse(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);assertEquals(0x18,b.read8(BASE));b.write8(BASE,3);assertEquals(3,b.read8(BASE)&3);b.write8(BASE,0);b.write8(BASE+1,1);assertNotEquals(0,b.read8(BASE)&128);b.cdrom.tick(CdRom.COMMAND_TICKS-1);assertEquals(0,irq(b));b.cdrom.tick(1);assertEquals(3,irq(b));assertArrayEquals(new int[]{16},response(b));assertEquals(0,b.read8(BASE)&128);
        }
    }
    @Test public void parameterFifoCapacityAndReset(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);for(int i=0;i<17;i++)b.write8(BASE+2,i);assertEquals(0,b.read8(BASE)&0x18);b.write8(BASE,1);b.write8(BASE+3,0x40);b.write8(BASE,0);assertEquals(0x18,b.read8(BASE)&0x18);
        }
    }
    @Test public void initHasSeparateAckAndCompleteInterrupts(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x0a);assertEquals(3,irq(b));assertArrayEquals(new int[]{2},response(b));b.cdrom.tick(100000);assertEquals(3,irq(b));ack(b);awaitIrq(b,2);assertEquals(2,irq(b));assertArrayEquals(new int[]{2},response(b));
        }
    }
    @Test public void interruptEnableAndAcknowledgmentDriveCpuLine(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801074,4);command(b,1);assertFalse(b.interruptPending());b.write8(BASE,1);b.write8(BASE+2,31);assertTrue(b.interruptPending());b.write16(0x1f801070,0xfffb);assertFalse(b.interruptPending());b.cdrom.tick(1);assertFalse("A CD IRQ already high must not produce another edge",b.interruptPending());b.write8(BASE+2,0);b.write8(BASE+2,31);assertTrue(b.interruptPending());ack(b);b.write16(0x1f801070,0xfffb);b.cdrom.tick(1);assertFalse(b.interruptPending());
        }
    }
    @Test public void invalidParametersAndUnknownCommandsReturnControllerErrors(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);command(b,2,0,0x2a,0);assertEquals(5,irq(b));assertEquals(0x10,response(b)[1]);ack(b);command(b,1,4);assertEquals(0x20,response(b)[1]);ack(b);command(b,0xff);assertEquals(0x40,response(b)[1]);
        }
    }
    @Test public void modeAndFilterRoundTripThroughGetParam(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x0d,3,7);ack(b);command(b,0x0e,0xa0);ack(b);command(b,0x0f);assertArrayEquals(new int[]{2,0xa0,0,3,7},response(b));
        }
    }
    @Test public void readsExact2048BytesAndRequestControlsDataFifo(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,7,0);assertEquals(0,b.read8(BASE)&64);b.write8(BASE+3,0x80);assertEquals(2048,b.cdrom.availableBytes());for(int i=0;i<2048;i++)assertEquals((7+i)&255,b.read8(BASE+2));assertEquals(0,b.read8(BASE)&64);b.write8(BASE+3,0);assertEquals(0,b.cdrom.availableBytes());
        }
    }
    @Test public void rawModeIncludesHeaderAndHas2340Bytes(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,0,32);b.write8(BASE+3,0x80);assertEquals(2340,b.cdrom.availableBytes());assertEquals(0,b.read8(BASE+2));assertEquals(2,b.read8(BASE+2));assertEquals(0,b.read8(BASE+2));assertEquals(2,b.read8(BASE+2));
        }
    }
    @Test public void cdromDmaCopiesActualSectorIntoMainRam(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,9,0);b.write8(BASE+3,0x80);b.write32(0x1f8010f0,0x0765c321);b.write32(0x1f8010b0,0x10000);b.write32(0x1f8010b4,512);b.write32(0x1f8010b8,0x11000000);
            for(int i=0;i<2048;i++)assertEquals((9+i)&255,b.read8(0x10000+i));assertEquals(2048,b.cdrom.dmaBytes);assertEquals(0,b.read32(0x1f8010b8)&0x1000000);
        }
    }
    @Test public void dmaWaitsForRequestInsteadOfCompletingWithZeroes(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);readFirst(b,3,0);b.write32(0x1f8010f0,0x0765c321);b.write32(0x1f8010b0,0x10000);b.write32(0x1f8010b4,512);b.write32(0x1f8010b8,0x11000000);assertNotEquals(0,b.read32(0x1f8010b8)&0x1000000);assertEquals(0,b.cdrom.dmaBytes);b.write8(BASE+3,0x80);assertEquals(0,b.read32(0x1f8010b8)&0x1000000);assertEquals(3,b.read8(0x10000));
        }
    }
    @Test public void getIdHasAckThenRegionResponseAndNoDiscError(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.cdrom.insert(disc());command(b,0x1a);assertEquals(3,irq(b));ack(b);awaitIrq(b,2);assertEquals(2,irq(b));assertArrayEquals(new int[]{2,0,32,0,'S','C','E','A'},response(b));
        }
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);command(b,0x1a);ack(b);awaitIrq(b,5);assertEquals(5,irq(b));assertEquals(0x40,response(b)[1]);}
    }
    @Test public void getLocLReturnsLastSectorHeader(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);readFirst(b,12,0);ack(b);command(b,0x10);int[] header=response(b);assertEquals(8,header.length);assertEquals(0x12,header[2]);assertEquals(2,header[3]);}
    }
    @Test public void pauseStopsSectorDeliveryAndHasCompleteResponse(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);readFirst(b,0,0);ack(b);command(b,9);assertEquals(3,irq(b));ack(b);awaitIrq(b,2);assertEquals(2,irq(b));assertEquals(0,response(b)[0]&32);long read=b.cdrom.sectorsRead;ack(b);b.cdrom.tick(SECTOR*2);assertEquals(read,b.cdrom.sectorsRead);}
    }
    @Test public void mediaFailureBecomesInt5WithDiagnostic(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);readFirst(b,99,0);ack(b);b.cdrom.tick(SECTOR);b.cdrom.tick(1);assertEquals(5,irq(b));assertTrue(b.cdrom.lastMediaError.contains("Outside test image"));}
    }
    @Test public void cookedIsoSectorAdapterPreservesUserBytes()throws Exception{
        byte[] bytes=BootTest.iso();IsoDisc disc=new IsoDisc(BootTest.source(bytes));byte[] raw=disc.readSector(22);assertEquals(1,raw[15]);assertArrayEquals(Arrays.copyOfRange(bytes,22*2048,23*2048),Arrays.copyOfRange(raw,16,2064));
    }
}
