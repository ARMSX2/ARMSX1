package dev.psxlab.core;

import java.util.zip.CRC32;
import org.junit.Test;
import static org.junit.Assert.*;

/** Exercise the real JNI SPU as well as the Java reference with a streaming refill. */
public class SpuStreamingIntegrationTest {
    @Test public void multiBlockSpuDmaCompletionSeesThePublishedRequestState(){
        for(int mode=0;mode<(NativeCore.AVAILABLE?3:1);mode++)try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
            int[] handler={0x3c1a0001,0x8f5b0000,0,0xaf5b0004,0x241b0001,0xaf5b0008,
                    0x3c1a1f80,0x241bfff7,0xaf5b1070,0x3c1b1090,0xaf5b10f4,0x401a7000,0,0x03400008,0x42000010};
            for(int i=0;i<handler.length;i++)bus.write32(0x80+i*4,handler[i]);
            int[] program={0xad090000,0xad4b0000,0x1000ffff,0};
            for(int i=0;i<program.length;i++)bus.write32(0x20000+i*4,program[i]);
            for(int i=0;i<16;i++)bus.write32(0x18000+i*4,0x12345678+i);
            bus.write32(0x1f8010f0,0x076d4321);bus.write32(0x1f8010f4,0x00900000);
            bus.write16(0x1f801da6,0x200);bus.write16(0x1f801daa,0x8020);
            bus.write32(0x1f8010c0,0x18000);bus.write32(0x1f8010c4,0x00800010);
            bus.write32(0x1f801074,8);cpu.cop0[12]=0x401;
            cpu.r[8]=0x1f8010c8;cpu.r[9]=0x01000201;cpu.r[10]=0x10000;cpu.r[11]=1;
            if(mode>0)assertTrue(cpu.enableNative(mode==2,true,true));cpu.jump(0x80020000);
            try{cpu.run(100000);bus.flushClocks();assertEquals("DMA callback was never reached, mode="+mode,1,bus.read32(0x10008));
                assertEquals("Completion ran before the following request-state store, mode="+mode,1,bus.read32(0x10004));}
            finally{cpu.closeNative();}
        }
    }

    @Test public void nativeAndJavaCheckTransferAndCachedBlockIrqsAtRegisterBoundaries(){
        for(boolean nativeSpu:new boolean[]{false,true}){
            try(Gpu gpu=new Gpu(false)){
                Bus b=new Bus(gpu,new PadState(),nativeSpu);b.write16(0x1f801074,512);
                b.spu.write(0x1f801da4,0x201);b.spu.write(0x1f801daa,0xc040);b.spu.write(0x1f801da6,0x200);
                for(int i=0;i<3;i++)b.spu.writeTransfer(0);assertFalse(b.interruptPending());
                b.spu.writeTransfer(0);assertTrue("Transfer end, native="+nativeSpu,b.interruptPending());
            }
            try(Gpu gpu=new Gpu(false)){
                Bus b=new Bus(gpu,new PadState(),nativeSpu);block(b,0x1000,7,0x1111);voice(b,0x1000);frames(b.spu,5);
                b.write16(0x1f801074,512);b.spu.write(0x1f801da4,0x300);b.spu.write(0x1f801daa,0xc040);assertFalse(b.interruptPending());
                b.spu.write(0x1f801da4,0x201);assertTrue("Cached block, native="+nativeSpu,b.interruptPending());
            }
            try(Gpu gpu=new Gpu(false)){
                Bus b=new Bus(gpu,new PadState(),nativeSpu);b.write16(0x1f801074,512);
                b.spu.write(0x1f801da4,0x200);b.spu.write(0x1f801da6,0x200);assertFalse(b.interruptPending());
                b.spu.write(0x1f801daa,0xc040);assertTrue("IRQ enable, native="+nativeSpu,b.interruptPending());
            }
        }
    }
    @Test public void nativeAndJavaLatchedIrqsEmitOnlyOneEdge(){
        for(boolean nativeSpu:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),nativeSpu);b.write16(0x1f801074,512);
            b.spu.write(0x1f801da4,0x200);b.spu.write(0x1f801daa,0xc040);b.spu.write(0x1f801da6,0x200);
            b.spu.writeTransfer(0);assertTrue(b.interruptPending());b.write16(0x1f801070,0xfdff);
            b.spu.writeTransfer(0);assertFalse("Repeated SPU edge, native="+nativeSpu,b.interruptPending());
            assertEquals(64,b.spu.read(0x1f801dae)&64);
            b.spu.write(0x1f801daa,0xc000);b.spu.write(0x1f801da6,0x200);b.spu.write(0x1f801daa,0xc040);b.spu.writeTransfer(0);
            assertTrue(b.interruptPending());
        }
    }
    private static void block(Bus b,int address,int flags,int packed){
        b.spu.write(0x1f801da6,address/8);b.spu.writeTransfer(flags<<8);
        for(int i=0;i<7;i++)b.spu.writeTransfer(packed);
    }
    private static void voice(Bus b,int pitch){
        b.spu.write(0x1f801c00,0x3fff);b.spu.write(0x1f801c02,0x3fff);b.spu.write(0x1f801c04,pitch);
        b.spu.write(0x1f801c06,0x200);b.spu.write(0x1f801c08,0x00ff);b.spu.write(0x1f801c0a,0x1fc0);
        b.spu.write(0x1f801c0e,0x200);b.spu.write(0x1f801d80,0x3fff);b.spu.write(0x1f801d82,0x3fff);
        b.spu.write(0x1f801daa,0xc000);b.spu.write(0x1f801d88,1);
    }
    private static void frames(Spu s,int count){for(int i=0;i<count;i++)s.tick(768);s.flushAudio();}
    @Test public void nativeAndJavaRefillsDoNotReadStaleStopFlags(){
        assertTrue(NativeCore.AVAILABLE);
        for(boolean nativeSpu:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),nativeSpu);assertEquals(nativeSpu,b.spu.nativeExecution());
            block(b,0x1000,0,0x1111);block(b,0x1010,1,0x2222);voice(b,0x1000);frames(b.spu,32);
            assertEquals(1,b.spu.decodedBlocks);block(b,0x1010,3,0x3333);frames(b.spu,100);
            assertTrue("Refill stream stopped; native="+nativeSpu,b.spu.read(0x1f801c0c)>0);
        }
    }
    @Test public void nativeAndJavaReadIrqsOccurOnTheFrameWhichReadsTheBlock(){
        for(boolean nativeSpu:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),nativeSpu);block(b,0x1000,0,0x1111);block(b,0x1010,3,0x2222);voice(b,0x1000);
            b.spu.write(0x1f801da4,0x202);b.spu.write(0x1f801daa,0xc040);frames(b.spu,32);
            assertEquals(0,b.spu.read(0x1f801dae)&64);frames(b.spu,1);assertEquals(64,b.spu.read(0x1f801dae)&64);
        }
    }
    @Test public void nativeAndJavaStaticStreamPcmRemainUnchangedAcrossAllPitchCases(){
        long[] expected={1194320044L,2213971421L,329282818L,743243591L,3987942357L,162771275L};
        int[] pitches={0x400,0x800,0x1000,0x1234,0x3fff,0x4000};
        for(boolean nativeSpu:new boolean[]{false,true})for(int n=0;n<pitches.length;n++)try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),nativeSpu);block(b,0x1000,4,0x1234);block(b,0x1010,3,0x8f76);voice(b,pitches[n]);
            CRC32 crc=new CRC32();b.spu.setAudioSink((pcm,count)->{for(int i=0;i<count;i++){crc.update(pcm[i]&255);crc.update((pcm[i]>>>8)&255);}});
            frames(b.spu,2048);assertEquals("Pitch="+pitches[n]+" native="+nativeSpu,expected[n],crc.getValue());
        }
    }
    @Test public void nativeRegisterRamIrqAndPcmParityStillPass(){assertTrue(Spu.verifyNative().contains("match the original Java engine"));}
    @Test public void savedDeferredBlocksAndPitchCarriesResumeWithMatchingPcm()throws Exception{
        for(boolean nativeSpu:new boolean[]{false,true})try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga,new PadState(),nativeSpu),b=new Bus(gb,new PadState(),nativeSpu);Cpu ca=new Cpu(a),cb=new Cpu(b);
            block(a,0x1000,4,0x1234);block(a,0x1010,3,0x8f76);voice(a,0x3fff);frames(a.spu,12);
            byte[] state=SaveState.capture(ca,a,ga,"stream-boundary");CRC32 expected=new CRC32(),actual=new CRC32();
            a.spu.setAudioSink((pcm,n)->{for(int i=0;i<n;i++){expected.update(pcm[i]&255);expected.update((pcm[i]>>>8)&255);}});frames(a.spu,512);
            SaveState.restore(state,cb,b,gb,"stream-boundary");b.spu.setAudioSink((pcm,n)->{for(int i=0;i<n;i++){actual.update(pcm[i]&255);actual.update((pcm[i]>>>8)&255);}});frames(b.spu,512);
            assertEquals(expected.getValue(),actual.getValue());assertEquals(a.spu.frames,b.spu.frames);
        }
    }
}
