package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class CoreTest {
    @Test public void syscallEntersKernelExceptionVector() {
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,12);Cpu c=new Cpu(b);
            c.step();assertEquals(0x80000080,c.pc);assertEquals(0x80010000,c.cop0[14]);assertEquals(8,c.lastException);assertEquals(32,c.cop0[13]&0x7c);assertEquals(1,c.instructions);
        }
    }
    @Test public void signedBranchesCoverBothOutcomesAndLink() {
        for(int kind:new int[]{0,1,16,17})for(int value:new int[]{Integer.MIN_VALUE,-1,0,1,Integer.MAX_VALUE}){
            try(Gpu g=new Gpu(false)){
                Bus b=new Bus(g);b.write32(0x10000,0x04200002|(kind<<16));b.write32(0x10004,0x24020007);
                Cpu c=new Cpu(b);c.r[1]=value;c.r[31]=123;
                c.step();assertEquals(0x80010004,c.pc);c.step();assertEquals(7,c.r[2]);
                boolean taken=(kind&1)==0?value<0:value>=0;
                assertEquals(taken?0x8001000c:0x80010008,c.pc);
                assertEquals((kind&16)!=0?0x80010008:123,c.r[31]);
            }
        }
    }
    @Test public void bgezNegativeOffsetAndDelayedLoadUseOldOperand() {
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,0x0461fffd);Cpu c=new Cpu(b);c.r[3]=0;c.step();c.step();assertEquals(0x8000fff8,c.pc);
            b.write32(0x10000,0x8c030100);b.write32(0x10004,0x04610002);b.write32(0x100,0);c=new Cpu(b);c.r[3]=-1;
            c.step();c.step();c.step();assertEquals(0x8001000c,c.pc);assertEquals(0,c.r[3]);
        }
    }
    @Test public void addiSignExtendsAndDetectsOverflow() {
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x10000,0x2042fff8);Cpu c=new Cpu(b);c.r[2]=32;c.step();assertEquals(24,c.r[2]);
            c.jump(0x80010000);c.r[2]=Integer.MIN_VALUE;c.step();assertEquals(12,c.lastException);assertEquals(Integer.MIN_VALUE,c.r[2]);
        }
    }
    @Test public void threadedAndInlineFramesMatch() {
        int[][] frames=new int[2][];
        for(int mode=0;mode<2;mode++)try(Gpu g=new Gpu(mode==1)) {
            Cpu c=Demo.load(new Bus(g));for(int i=0;i<10000;i++)c.step();frames[mode]=g.snapshot();
        }
        assertArrayEquals(frames[0],frames[1]);
        assertNotEquals(0xff000000,frames[0][40*1024+40]);
    }
    @Test public void loadDelayAndZeroRegister() {
        try(Gpu g=new Gpu(false)) {
            Bus b=new Bus(g); b.write32(0x100,123);
            b.write32(0x10000,0x8c010100); // lw r1,100(zero)
            b.write32(0x10004,0x00201021); // addu r2,r1,zero: old value
            b.write32(0x10008,0x00201821); // addu r3,r1,zero: loaded value
            b.write32(0x1000c,0x24000063);
            Cpu c=new Cpu(b); for(int i=0;i<4;i++)c.step();
            assertEquals(0,c.r[2]); assertEquals(123,c.r[3]);assertEquals(0,c.r[0]);
        }
    }
    @Test public void branchExecutesDelaySlot() {
        try(Gpu g=new Gpu(false)) {
            Bus b=new Bus(g);b.write32(0x10000,0x10000002);b.write32(0x10004,0x24010007);b.write32(0x10008,0x24010063);b.write32(0x1000c,0x24020009);
            Cpu c=new Cpu(b);for(int i=0;i<3;i++)c.step();assertEquals(7,c.r[1]);assertEquals(9,c.r[2]);
        }
    }
    @Test public void queueStaysOrderedUnderPressure() {
        try(Gpu g=new Gpu(true)) {
            for(int i=0;i<4000;i++){g.write(0x02000000|(i&255));g.write(0);g.write(0x00010010);}
            int last=3999&255, quant=last>>>3;assertEquals(0xff000000|((quant<<3|quant>>>2)<<16),g.snapshot()[0]);
        }
    }
    @Test public void ramMirrorsAcrossSegments() {try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x80000000,0x12345678);assertEquals(0x12345678,b.read32(0xa0200000));}}
}
