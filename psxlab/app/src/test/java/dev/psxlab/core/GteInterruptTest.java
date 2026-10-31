package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class GteInterruptTest {
    private static void interrupt(Bus b,Cpu c){b.write16(0x1f801074,1);b.raiseInterrupt(0);c.cop0[12]=0x40000401;}
    @Test public void avszCommandCompletesBeforeIrqWithEpcStillAtCommand(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write32(0x10000,0x4b68002e);c.gte.writeData(24,0xff000000);for(int r=16;r<=19;r++)c.gte.writeData(r,2000);c.gte.writeControl(30,85);interrupt(b,c);c.step();assertEquals(680000,c.gte.readData(24));assertEquals(166,c.gte.readData(7));assertEquals(0x80010000,c.cop0[14]);assertEquals(0x80000080,c.pc);assertEquals(0,c.lastException);assertEquals(1,c.exceptions);}
    }
    @Test public void kernelSkipLeavesDepthLookupInsideScratchpad(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);Cpu c=new Cpu(b);
            int[] code={0x4b68002e,0x481fc000,0,0x001ffbc2,0x001ff982,0x03e3f821,0x93ff00d0,0};
            for(int i=0;i<code.length;i++)b.write32(0x10000+i*4,code[i]);
            c.r[3]=0x1f800000;b.write8(0x1f8000d0,0x5a);c.gte.writeData(24,0xff000000);for(int r=16;r<=19;r++)c.gte.writeData(r,2000);c.gte.writeControl(30,85);
            interrupt(b,c);c.step();int epc=c.cop0[14];assertEquals(0x4a000000,b.read32(epc)&0xfe000000);
            b.write16(0x1f801070,0xfffe);c.jump(epc+4);for(int i=0;i<7;i++)c.step();assertEquals(0x5a,c.r[31]);
        }
    }
    @Test public void nonIdempotentCommandRunsOnceWhenKernelSkipsIt(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write32(0x10000,0x4a08003e);c.gte.writeData(8,4096);c.gte.writeData(9,3);c.gte.writeData(25,2);interrupt(b,c);c.step();assertEquals(5,c.gte.readData(25));b.write16(0x1f801070,0xfffe);c.jump(c.cop0[14]+4);c.step();assertEquals(5,c.gte.readData(25));}
    }
    @Test public void registerMovesDoNotExecuteOnInterrupt(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write32(0x10000,0x48814800);c.r[1]=123;c.gte.writeData(9,45);interrupt(b,c);c.step();assertEquals(45,c.gte.readData(9));assertEquals(0x80010000,c.cop0[14]);}
    }
    @Test public void disabledGteDoesNotIssueCommandOnInterrupt(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write32(0x10000,0x4b68002e);c.gte.writeData(24,123);interrupt(b,c);c.cop0[12]&=~0x40000000;c.step();assertEquals(123,c.gte.readData(24));assertEquals(0,c.lastException);}
    }
    @Test public void interruptOnBranchDelayCommandKeepsBdAndBranchEpc(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);c.cop0[12]=0x40000000;b.write32(0x10000,0x10000003);b.write32(0x10004,0x4b68002e);c.gte.writeControl(30,1);for(int r=16;r<=19;r++)c.gte.writeData(r,100);c.step();interrupt(b,c);c.step();assertEquals(400,c.gte.readData(24));assertEquals(0x80010000,c.cop0[14]);assertNotEquals(0,c.cop0[13]&0x80000000);}
    }
}
