package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class CpuHardwareTest {
    @Test public void syscallHandlerCanReadEpcAndReturn(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,12);b.write32(0x10004,0x2401002a);
            b.write32(0x80,0x401a7000); // mfc0 k0,EPC
            b.write32(0x84,0);b.write32(0x88,0x275a0004);b.write32(0x8c,0x03400008);b.write32(0x90,0x42000010);
            Cpu c=new Cpu(b);c.cop0[12]=0x3f;for(int i=0;i<7;i++)c.step();
            assertEquals(42,c.r[1]);assertEquals(0x80010008,c.pc);assertEquals(0x3f,c.cop0[12]&63);
        }
    }
    @Test public void exceptionInTakenAndUntakenBranchDelaySetsBdAndBranchEpc(){
        for(int instruction:new int[]{0x10000002,0x14000002})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,instruction);b.write32(0x10004,12);Cpu c=new Cpu(b);c.step();c.step();
            assertEquals(0x80010000,c.cop0[14]);assertEquals(instruction==0x10000002?0xc0000020:0x80000020,c.cop0[13]);
            if(instruction==0x10000002)assertEquals(0x8001000c,c.cop0[6]);
        }
    }
    @Test public void bootExceptionVectorAndAddressErrors(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,0x8c010001);Cpu c=new Cpu(b);c.cop0[12]=0x400000;c.step();
            assertEquals(0xbfc00180,c.pc);assertEquals(1,c.cop0[8]);assertEquals(4,c.lastException);
            c.jump(0x80010000);b.write32(0x10000,0xac010003);c.step();assertEquals(5,c.lastException);assertEquals(3,c.cop0[8]);
            c.jump(0x80010001);c.step();assertEquals(4,c.lastException);assertEquals(0x80010001,c.cop0[8]);
        }
    }
    @Test public void halfwordLoadsSignExtendAndRespectDelay(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x102,0x8001);b.write32(0x10000,0x84010102);b.write32(0x10004,0x94020102);
            Cpu c=new Cpu(b);c.step();assertEquals(0,c.r[1]);c.step();assertEquals(0xffff8001,c.r[1]);c.step();assertEquals(0x8001,c.r[2]);
        }
    }
    @Test public void unalignedLoadPairsReconstructAtAllAlignments(){
        for(int offset=0;offset<4;offset++)for(boolean reverse:new boolean[]{false,true})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);for(int i=0;i<8;i++)b.write8(0x100+i,0x10+i);
            int left=0x88010103+offset,right=0x98010100+offset;
            b.write32(0x10000,reverse?right:left);b.write32(0x10004,reverse?left:right);Cpu c=new Cpu(b);c.step();c.step();c.step();
            assertEquals(0x13121110+offset*0x01010101,c.r[1]);
        }
    }
    @Test public void unalignedStoresPreserveSurroundingBytes(){
        for(int offset=0;offset<4;offset++)try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);java.util.Arrays.fill(b.ram,0x100,0x108,(byte)0x55);
            b.write32(0x10000,0xa8010103+offset);b.write32(0x10004,0xb8010100+offset);Cpu c=new Cpu(b);c.r[1]=0x44332211;c.step();c.step();
            for(int i=0;i<8;i++)assertEquals(i>=offset && i<offset+4?0x11*(i-offset+1):0x55,b.read8(0x100+i));
        }
    }
    @Test public void multiplicationAndDivisionHandleEdges(){
        for(int op:new int[]{24,25,26,27})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,0x00220000|op);Cpu c=new Cpu(b);c.r[1]=-2;c.r[2]=3;c.step();
            if(op==24){assertEquals(-6,c.lo);assertEquals(-1,c.hi);}
            if(op==25){assertEquals(-6,c.lo);assertEquals(2,c.hi);}
            if(op==26){assertEquals(0,c.lo);assertEquals(-2,c.hi);}
            if(op==27){assertEquals(1431655764,c.lo);assertEquals(2,c.hi);}
            if(op>=26){c.jump(0x80010000);c.r[1]=Integer.MIN_VALUE;c.r[2]=-1;c.step();if(op==26){assertEquals(Integer.MIN_VALUE,c.lo);assertEquals(0,c.hi);}
                c.jump(0x80010000);c.r[2]=0;c.step();assertEquals(op==26?1:-1,c.lo);assertEquals(Integer.MIN_VALUE,c.hi);}
        }
    }
    @Test public void cop0LoadDelayAndCacheIsolation(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,0x40816000);b.write32(0x10004,0x40026000);b.write32(0x10008,0xac010100);Cpu c=new Cpu(b);c.r[1]=0x10000;c.step();assertTrue(b.cacheIsolated);c.step();assertEquals(0,c.r[2]);c.step();assertEquals(0x10000,c.r[2]);assertEquals(0,b.read32(0x100));
        }
    }
    @Test public void irqIsMaskedThenDeliveredAndAcknowledged(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);Cpu c=new Cpu(b);b.raiseInterrupt(3);c.cop0[12]=0x401;c.step();assertEquals(0,c.exceptions);
            b.write16(0x1f801074,8);c.step();assertEquals(1,c.exceptions);assertEquals(0,c.lastException);
            b.write16(0x1f801070,0xfff7);assertFalse(b.interruptPending());
        }
    }
    @Test public void resetExecutesBiosAndRomIsReadOnly(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);byte[] bios=new byte[512*1024];BootTest.word(bios,0,0x2401002a);b.loadBios(bios);Cpu c=new Cpu(b);c.reset();c.step();assertEquals(42,c.r[1]);assertEquals(0xbfc00004,c.pc);
            b.write32(0xbfc00000,0);assertEquals(0x2401002a,b.read32(0xbfc00000));
        }
    }
    @Test public void gpuTransfersKeepRawPixelsAndOrdering(){
        for(boolean threaded:new boolean[]{false,true})try(Gpu g=new Gpu(threaded)){
            g.write(0xa0000000);g.write(0x01ff03ff);g.write(0x00020002);g.write(0x1234abcd);g.write(0x8000001f);
            g.write(0xc0000000);g.write(0x01ff03ff);g.write(0x00020002);assertEquals(0x1234abcd,g.read());assertEquals(0x8000001f,g.read());
            g.control(0x03000000);assertEquals(0,g.status()&0x800000);g.control(0x03000001);assertNotEquals(0,g.status()&0x800000);
        }
    }
}
