package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class GteTest {
    private void identity(Gte g,int matrix){int base=matrix*8;g.writeControl(base,4096);g.writeControl(base+1,0);g.writeControl(base+2,4096);g.writeControl(base+3,0);g.writeControl(base+4,4096);}
    private void vector(Gte g,int index,int x,int y,int z){g.writeData(index*2,(x&65535)|(y<<16));g.writeData(index*2+1,z);}
    private void ir(Gte g,int a,int b,int c){g.writeData(9,a);g.writeData(10,b);g.writeData(11,c);}
    @Test public void registerWidthsAndControlSignExtension(){
        Gte g=new Gte();g.writeControl(29,0x1234ffff);assertEquals(-1,g.readControl(29));g.writeControl(26,0xffff);assertEquals(-1,g.readControl(26));
        g.writeData(1,0x12348000);assertEquals(-32768,g.readData(1));g.writeData(19,-1);assertEquals(65535,g.readData(19));
        g.writeControl(5,0x12345678);assertEquals(0x12345678,g.readControl(5));
    }
    @Test public void screenFifoMirrorPushesOnlyWhenWritingSxyp(){
        Gte g=new Gte();g.writeData(12,1);g.writeData(13,2);g.writeData(14,3);g.writeData(15,4);assertEquals(2,g.readData(12));assertEquals(3,g.readData(13));assertEquals(4,g.readData(14));assertEquals(4,g.readData(15));
        g.writeData(14,99);assertEquals(2,g.readData(12));assertEquals(99,g.readData(15));
    }
    @Test public void colorPackingClampsWithoutChangingFlags(){
        Gte g=new Gte();g.writeData(28,0x7fff);assertEquals(3968,g.readData(9));assertEquals(0x7fff,g.readData(29));
        ir(g,-128,4096,128);assertEquals((31<<5)|(1<<10),g.readData(28));assertEquals(0,g.readControl(31));g.writeData(29,0);assertNotEquals(0,g.readData(29));
    }
    @Test public void leadingSignBitCountAndReadOnlyResult(){
        Gte g=new Gte();for(int[] pair:new int[][]{{0,32},{-1,32},{1,31},{0x80000000,1},{0x00ffffff,8},{0xff000000,8}}){g.writeData(30,pair[0]);assertEquals(pair[1],g.readData(31));g.writeData(31,0);assertEquals(pair[1],g.readData(31));}
    }
    @Test public void flagSummaryIgnoresColorIr3AndIr0Saturation(){
        Gte g=new Gte();g.writeControl(31,0xffffffff);assertEquals(0xfffff000,g.readControl(31));g.writeControl(31,0x00781000);assertEquals(0x00781000,g.readControl(31));g.writeControl(31,1<<17);assertEquals(0x80020000,g.readControl(31));
    }
    @Test public void mvmvaIdentityAndTranslation(){
        Gte g=new Gte();identity(g,0);vector(g,0,10,-20,30);g.writeControl(5,5);g.writeControl(6,7);g.writeControl(7,-3);g.execute(0x80012);
        assertEquals(15,g.readData(9));assertEquals(-13,g.readData(10));assertEquals(27,g.readData(11));assertEquals(0,g.readControl(31));
    }
    @Test public void mvmvaSelectsLightMatrixIrVectorAndBackgroundTranslation(){
        Gte g=new Gte();identity(g,1);ir(g,12,34,56);g.writeControl(13,1);g.writeControl(14,2);g.writeControl(15,3);g.execute(0x80012|(1<<17)|(3<<15)|(1<<13));
        assertEquals(13,g.readData(9));assertEquals(36,g.readData(10));assertEquals(59,g.readData(11));
    }
    @Test public void mvmvaFarColorHardwareBugDropsFirstColumnAndTranslation(){
        Gte g=new Gte();identity(g,0);vector(g,0,10,20,30);g.writeControl(21,100);g.execute(0x80012|(2<<13));assertEquals(0,g.readData(9));assertEquals(20,g.readData(10));assertEquals(30,g.readData(11));
    }
    @Test public void squareHonorsShiftAndSaturation(){
        Gte g=new Gte();ir(g,-256,128,64);g.execute(0x80028);assertEquals(16,g.readData(9));assertEquals(4,g.readData(10));assertEquals(1,g.readData(11));
        ir(g,-256,128,64);g.execute(0x28);assertEquals(65536,g.readData(25));assertEquals(32767,g.readData(9));assertEquals(0x81000000,g.readControl(31));
    }
    @Test public void crossProductUsesMatrixDiagonal(){
        Gte g=new Gte();g.writeControl(0,2);g.writeControl(2,3);g.writeControl(4,5);ir(g,7,11,13);g.execute(0x0c);assertEquals(-16,g.readData(9));assertEquals(9,g.readData(10));assertEquals(1,g.readData(11));
    }
    @Test public void nclipComputesSignedAreaAndMac0Overflow(){
        Gte g=new Gte();g.writeData(12,0);g.writeData(13,10);g.writeData(14,10<<16);g.execute(6);assertEquals(100,g.readData(24));
        g.writeData(12,0x80008000);g.writeData(13,0x80007fff);g.writeData(14,0x7fff8000);g.execute(6);assertNotEquals(0,g.readControl(31)&(1<<16));
    }
    @Test public void averageDepthAndOtzSaturation(){
        Gte g=new Gte();g.writeControl(29,1024);g.writeControl(30,1024);for(int i=16;i<20;i++)g.writeData(i,100);g.execute(0x2d);assertEquals(75,g.readData(7));g.execute(0x2e);assertEquals(100,g.readData(7));
        g.writeControl(29,-1);g.execute(0x2d);assertEquals(0,g.readData(7));assertEquals(0x80040000,g.readControl(31));
    }
    @Test public void perspectiveIdentityProjectsAndUpdatesDepthFifo(){
        Gte g=new Gte();identity(g,0);vector(g,0,100,-200,1000);g.writeControl(26,1000);g.execute(0x80001);
        assertEquals(100,(short)g.readData(14));assertEquals(-200,(short)(g.readData(14)>>>16));assertEquals(1000,g.readData(19));assertEquals(0,g.readControl(31));
    }
    @Test public void triplePerspectivePreservesVertexOrder(){
        Gte g=new Gte();identity(g,0);for(int i=0;i<3;i++)vector(g,i,(i+1)*10,(i+1)*20,1000);g.writeControl(26,1000);g.execute(0x80030);
        for(int i=0;i<3;i++){assertEquals((i+1)*10,(short)g.readData(12+i));assertEquals((i+1)*20,(short)(g.readData(12+i)>>>16));assertEquals(1000,g.readData(17+i));}
    }
    @Test public void perspectiveNearPlaneSaturatesAndSetsFlags(){
        Gte g=new Gte();identity(g,0);vector(g,0,1000,1000,0);g.writeControl(26,100);g.execute(0x80001);assertEquals(1023,(short)g.readData(14));assertNotEquals(0,g.readControl(31)&0x80020000);assertEquals(0,g.readData(19));
    }
    @Test public void ir3OnlySaturationDoesNotSetSummary(){
        Gte g=new Gte();identity(g,0);vector(g,0,0,0,1);g.writeControl(7,40000);g.execute(0x80001);assertEquals(32767,g.readData(11));assertEquals(40001,g.readData(19));assertEquals(1<<22,g.readControl(31));
    }
    @Test public void triplePerspectiveRetainsFlagsFromFirstVertex(){
        Gte g=new Gte();identity(g,0);g.writeControl(7,1);vector(g,0,0,0,32767);vector(g,1,0,0,100);vector(g,2,0,0,100);g.execute(0x80030);assertEquals(101,g.readData(11));assertEquals(1<<22,g.readControl(31));
    }
    @Test public void matrixAccumulatorOverflowSetsPositiveFlagAndWraps44Bits(){
        Gte g=new Gte();g.writeControl(0,32767);g.writeControl(5,Integer.MAX_VALUE);vector(g,0,32767,0,0);g.execute(0x80012);assertTrue(g.readData(25)<0);assertNotEquals(0,g.readControl(31)&(1<<30));
    }
    @Test public void interpolationAndColorFifoCarryCodeByte(){
        Gte g=new Gte();g.writeData(6,0x34030201);g.writeData(8,0);g.execute(0x80010);assertEquals(0x34030201,g.readData(22));
        g.writeControl(21,16*10);g.writeControl(22,16*20);g.writeControl(23,16*30);g.writeData(8,4096);g.execute(0x80010);assertEquals(0x341e140a,g.readData(22));assertEquals(0x34030201,g.readData(21));
    }
    @Test public void dpctProcessesAllThreeOriginalColors(){
        Gte g=new Gte();g.writeData(6,0x20000000);g.writeData(20,0x01030201);g.writeData(21,0x02060504);g.writeData(22,0x03090807);g.writeData(8,0);g.execute(0x8002a);assertEquals(0x20030201,g.readData(20));assertEquals(0x20060504,g.readData(21));assertEquals(0x20090807,g.readData(22));
    }
    @Test public void generalInterpolationAccumulatesWithPreviousMac(){
        Gte g=new Gte();g.writeData(6,0x20000000);g.writeData(8,2048);ir(g,160,320,480);g.execute(0x8003d);assertEquals(80,g.readData(9));assertEquals(0x200f0a05,g.readData(22));
        ir(g,160,320,480);g.execute(0x8003e);assertEquals(160,g.readData(9));assertEquals(0x201e140a,g.readData(22));
    }
    @Test public void lightingIdentityAndModulationHaveExpectedScale(){
        Gte g=new Gte();identity(g,1);identity(g,2);vector(g,0,160,320,480);g.writeData(6,0x34808080);g.execute(0x8041e);assertEquals(0x341e140a,g.readData(22));
        g.execute(0x8041b);assertEquals(0x340f0a05,g.readData(22));
    }
    @Test public void cpuControlTransferFixesReportedInstructionAndLoadDelay(){
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);bus.write32(0x10000,0x48c8e800);bus.write32(0x10004,0x4849e800);Cpu cpu=new Cpu(bus);cpu.cop0[12]=0x40000000;cpu.r[8]=0x1234ffff;
            cpu.step();assertEquals(-1,cpu.gte.readControl(29));cpu.step();assertEquals(0,cpu.r[9]);cpu.step();assertEquals(-1,cpu.r[9]);assertEquals(0,cpu.exceptions);
        }
    }
    @Test public void cpuDataTransfersAndLwcSwcUseGteRegisterRules(){
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);bus.write32(0x100,0x12348000);bus.write32(0x10000,0xc8010100);bus.write32(0x10004,0xe8010104);Cpu cpu=new Cpu(bus);cpu.cop0[12]=0x40000000;cpu.step();cpu.step();assertEquals(0xffff8000,bus.read32(0x104));
        }
    }
    @Test public void disabledCop2RaisesCoprocessorUnusable(){
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);bus.write32(0x10000,0x48c8e800);Cpu cpu=new Cpu(bus);cpu.step();assertEquals(11,cpu.lastException);assertEquals(2,(cpu.cop0[13]>>>28)&3);}
    }
}
