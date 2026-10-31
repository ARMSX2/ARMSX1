package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class MdecTest {
    private static final int DATA=0x1f801820,STATUS=0x1f801824;
    private static void tables(Bus b){
        b.write32(DATA,0x40000001);
        for(int i=0;i<32;i++)b.write32(DATA,0x01010101);
        b.write32(DATA,0x60000000);
        // A constant DC row suffices for analytical constant-block fixtures.
        for(int i=0;i<32;i++)b.write32(DATA,i<4?0x5a825a82:0);
    }
    private static void block(Bus b,int dc){b.write32(DATA,0xfe000000|0x400|(dc&1023));}
    private static void dma(Bus b,int channel,int address,int count,int direction){
        int base=0x1f801080+channel*16;
        b.write32(base,address);b.write32(base+4,0x10000|count);b.write32(base+8,0x1000200|direction);
    }
    @Test public void resetIsMappedThroughPhysicalAliasesAndAbortsInput(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0xbf801824,0x80000000);assertEquals(0x80040000,b.read32(STATUS));
            b.write32(DATA,0x40000001);assertEquals(0xa004001f,b.read32(STATUS));
            b.write32(DATA,0x01020304);assertEquals(30,b.read32(STATUS)&65535);
            b.write32(STATUS,0x80000000);assertEquals(0x80040000,b.read32(STATUS));
            assertEquals(0,b.mdec.availableWords());
        }
    }
    @Test public void invalidCommandsLatchFlagsWithoutInventingParameters(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(DATA,0xfe001234);
            assertEquals(0x87841234,b.read32(STATUS));assertEquals(0,b.read32(STATUS)&0x20000000);
            b.write32(DATA,0x40000000);assertEquals(15,b.read32(STATUS)&65535);
        }
    }
    @Test public void grayscaleUsesQuantAndProgrammedScaleAndPacksFourOrEightPixels(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);
            b.write32(DATA,0x28000001);block(b,64); // DC 64 / 8 = 8; unsigned = 136.
            for(int i=0;i<16;i++)assertEquals(0x88888888,b.read32(DATA));
            assertEquals(0x8204ffff,b.read32(STATUS));
            b.write32(DATA,0x20000001);block(b,64);
            for(int i=0;i<8;i++)assertEquals(0x88888888,b.read32(DATA));
            b.write32(DATA,0x2c000001);block(b,-64);
            for(int i=0;i<16;i++)assertEquals(0xf8f8f8f8,b.read32(DATA));
        }
    }
    @Test public void splitRleSkipsPaddingAndWaitsForEndOfBlock(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(DATA,0x28000002);
            b.write32(DATA,0x0440fe00);assertEquals(0,b.mdec.availableWords());
            assertEquals(0,b.read32(STATUS)&65535);
            b.write32(DATA,0xfe00fe00);assertEquals(16,b.mdec.availableWords());
            assertEquals(0x88888888,b.read32(DATA));
        }
    }
    @Test public void resetDiscardsDecodedOutputButPreservesTables(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(DATA,0x28000001);block(b,64);
            b.write32(STATUS,0x80000000);assertEquals(0,b.mdec.availableWords());assertEquals(0x80040000,b.read32(STATUS));
            b.write32(DATA,0x28000001);block(b,64);assertEquals(0x88888888,b.read32(DATA));
        }
    }
    @Test public void inputDmaWaitsForRequestThenCompletesTableUploadAndIrq(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010f0,0x076543a9);b.write32(0x1f8010f4,0x00810000);b.write16(0x1f801074,8);
            for(int i=0;i<16;i++)b.write32(0x100+i*4,0x01010101);
            b.write32(DATA,0x40000000);dma(b,0,0x100,16,1);
            assertNotEquals(0,b.read32(0x1f801088)&0x1000000);
            b.write32(STATUS,0x40000000);
            assertEquals(0,b.read32(0x1f801088)&0x1000000);assertEquals(0x140,b.read32(0x1f801080));
            assertEquals(0xffff,b.read32(STATUS)&65535);assertTrue(b.interruptPending());
        }
    }
    @Test public void outputDmaWaitsForDecodeAndInterleavesAllFourColorBlocks(){
        for(int depth:new int[]{2,3})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(STATUS,0x60000000);b.write32(0x1f8010f0,0x076543a9);
            int count=depth==3?128:192;
            dma(b,1,0x1000,count,0);assertNotEquals(0,b.read32(0x1f801098)&0x1000000);
            b.write32(DATA,0x20000006|(depth<<27)|(depth==3?0x02000000:0));
            block(b,0);block(b,0);block(b,0);block(b,64);block(b,-64);block(b,128);
            b.dma.tick(count);
            assertEquals(0,b.read32(0x1f801098)&0x1000000);
            assertEquals(0,b.mdec.availableWords());assertEquals(0x1000+count*4,b.read32(0x1f801090));
            for(int y=0;y<16;y++)for(int x=0;x<16;x++){
                int shade=128+(y<8?(x<8?0:8):(x<8?-8:16));
                if(depth==3){int c=shade>>>3;assertEquals(0x8000|c|(c<<5)|(c<<10),b.read16(0x1000+(y*16+x)*2));}
                else for(int c=0;c<3;c++)assertEquals(shade,b.read8(0x1000+(y*16+x)*3+c));
            }
        }
    }
    @Test public void cpuReadoutKeepsBlockOrderWhileDmaReorders(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(DATA,0x38000006);
            block(b,0);block(b,0);block(b,0);block(b,64);block(b,-64);block(b,128);
            for(int q=0;q<4;q++){
                int c=(new int[]{128,136,120,144}[q])>>>3,p=c|(c<<5)|(c<<10);
                assertEquals(q,(b.read32(STATUS)>>>16)&7);
                for(int i=0;i<32;i++)assertEquals(p|(p<<16),b.read32(DATA));
            }
        }
    }
    @Test public void outputDmaDoesNotInterruptBeforeCallerPublishesFrameState(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(STATUS,0x60000000);
            b.write32(0x1f8010f0,0x076543a9);b.write32(0x1f8010f4,0x00820000);b.write16(0x1f801074,8);
            b.write32(DATA,0x38000006);for(int i=0;i<6;i++)block(b,0);
            dma(b,1,0x1000,128,0);
            assertFalse(b.interruptPending());assertNotEquals(0,b.read32(0x1f801098)&0x1000000);
            // A movie player stores this state after launching DMA, before its
            // completion callback advances the frame. Synchronous IRQs lose it.
            for(int i=0;i<32;i++)b.tick();b.flushClocks();b.write32(0x100,1);
            assertFalse(b.interruptPending());assertEquals(0,b.read32(0x1000));
            for(int i=0;i<96;i++)b.tick();b.flushClocks();
            assertTrue(b.interruptPending());assertEquals(1,b.read32(0x100));
            assertEquals(0x42104210,b.read32(0x1000));assertEquals(0,b.read32(0x1f801098)&0x1000000);
            b.write32(0x100,0);b.write32(0x1f8010f4,0x02820000);b.write16(0x1f801070,0);
            b.dma.tick(10000);assertFalse(b.interruptPending());assertEquals(0,b.read32(0x100));
        }
    }
    @Test public void outputDmaPausesWhenDisabledAndCanBeCancelledAndRestarted(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(STATUS,0x60000000);b.write32(0x1f8010f0,0x076543a9);
            b.write32(DATA,0x38000006);for(int i=0;i<6;i++)block(b,0);
            dma(b,1,0x1000,128,0);b.dma.tick(64);
            b.write32(0x1f8010f0,0x07654329);b.dma.tick(10000);
            assertEquals(128,b.mdec.availableWords());assertEquals(0,b.read32(0x1000));
            b.write32(0x1f8010f0,0x076543a9);b.dma.tick(63);assertEquals(0,b.read32(0x1000));
            b.write32(0x1f801098,0);b.dma.tick(10000);assertEquals(128,b.mdec.availableWords());
            dma(b,1,0x2000,128,0);b.dma.tick(128);
            assertEquals(0,b.read32(0x1000));assertEquals(0x42104210,b.read32(0x2000));assertEquals(0,b.mdec.availableWords());
        }
    }
    @Test public void outputDmaPreservesMacroblockLayoutAcrossSmallTransfers(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(STATUS,0x60000000);b.write32(0x1f8010f0,0x076543a9);
            b.write32(DATA,0x38000006);block(b,0);block(b,0);block(b,0);block(b,64);block(b,-64);block(b,128);
            for(int i=0;i<16;i++){dma(b,1,0x1000+i*32,8,0);b.dma.tick(8);}
            assertEquals(0x4631,b.read16(0x1000+16));assertEquals(0x3def,b.read16(0x1000+256));assertEquals(0x4a52,b.read16(0x1000+272));
        }
    }
    @Test public void zeroQuantScaleBypassesQuantTableAndSignSaturates(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);
            b.write32(DATA,0x28000001);b.write32(DATA,0xfe000100);
            for(int i=0;i<16;i++)assertEquals(0xc0c0c0c0,b.read32(DATA));
            b.write32(DATA,0x40000000);for(int i=0;i<16;i++)b.write32(DATA,-1);
            b.write32(DATA,0x2c000001);block(b,-512);
            for(int i=0;i<16;i++)assertEquals(0x80808080,b.read32(DATA));
        }
    }
    @Test public void chromaTableActuallyChangesColorAndRgbSaturates(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(DATA,0x40000001);
            for(int i=0;i<16;i++)b.write32(DATA,0x01010101);
            for(int i=0;i<16;i++)b.write32(DATA,0x08080808);
            b.write32(DATA,0x30000006);block(b,64);block(b,0);
            for(int i=0;i<4;i++)block(b,0);
            // Cr = (64 * 8) / 8 = 64. R = 128+90, G = 128-46, B = 128.
            assertEquals(0xda,b.read32(DATA)&255);
            assertEquals(0x52da8052,b.read32(DATA));
            b.write32(STATUS,0x80000000);b.write32(DATA,0x30000006);
            block(b,511);block(b,0);for(int i=0;i<4;i++)block(b,511);
            assertEquals(255,b.read32(DATA)&255);
        }
    }
    @Test public void acCoefficientProducesExpectedHorizontalCosine(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);tables(b);b.write32(DATA,0x60000000);
            int[] matrix=new int[64];
            for(int u=0;u<8;u++)for(int x=0;x<8;x++)matrix[u*8+x]=(int)Math.round(u==0?Math.sqrt(0.5)*32768:Math.cos(Math.PI*u*(2*x+1)/16)*32768);
            for(int i=0;i<64;i+=2)b.write32(DATA,(matrix[i]&65535)|(matrix[i+1]<<16));
            b.write32(DATA,0x28000002);b.write32(DATA,0x01000440);b.write32(DATA,0xfe00fe00);
            for(int y=0;y<8;y++)for(int group=0;group<2;group++){
                int word=b.read32(DATA);
                for(int i=0;i<4;i++){
                    int x=group*4+i,expected=128+8+(int)Math.round(Math.sqrt(2)*4*Math.cos(Math.PI*(2*x+1)/16));
                    assertEquals(expected,word>>>(i*8)&255,1);
                }
            }
        }
    }
}
