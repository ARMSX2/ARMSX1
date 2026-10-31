package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;
import java.util.*;

public class SpuTest {
    @Test public void tracingBothVoiceBanksPreservesGeneratedAudio(){
        try(Gpu g=new Gpu(false)){
            Bus a=new Bus(g,new PadState(),false),b=new Bus(g,new PadState(),false);
            List<String> events=new ArrayList<>();b.spu.setTraceSink(events::add);
            List<Integer> expected=collect(a.spu),actual=collect(b.spu);
            for(Bus bus:new Bus[]{a,b}){voice(bus,0,7);voice(bus,23,7);frames(bus.spu,200);bus.write16(0x1f801d8e,128);frames(bus.spu,20);}
            assertEquals(expected,actual);assertEquals(a.spu.frames,b.spu.frames);
            assertTrue(events.stream().anyMatch(e->e.contains("SPU_KEYON mask=800000") && e.contains("v23(start=01170 pitch=1000")));
            assertTrue(events.stream().anyMatch(e->e.contains("SPU_KEYOFF mask=800000")));
            int count=events.size();b.spu.setTraceSink(null);b.write16(0x1f801d88,1);assertEquals(count,events.size());
        }
    }
    private static void block(Bus b,int address,int flags,int packed){
        b.write16(0x1f801da6,address/8);b.spu.writeTransfer(flags<<8);
        for(int i=0;i<7;i++)b.spu.writeTransfer(packed);
    }
    private static void voice(Bus b,int n,int flags){
        block(b,0x1000+n*16,flags,0x1111);int r=0x1f801c00+n*16;
        b.write16(r,0x3fff);b.write16(r+2,0x3fff);b.write16(r+4,0x1000);b.write16(r+6,(0x1000+n*16)/8);
        b.write16(r+8,0x00ff);b.write16(r+10,0x1fc0);
        b.write16(0x1f801d80,0x3fff);b.write16(0x1f801d82,0x3fff);b.write16(0x1f801daa,0xc000);
        b.write16(n<16?0x1f801d88:0x1f801d8a,1<<(n%16));
    }
    private static List<Integer> collect(Spu s){List<Integer> out=new ArrayList<>();s.setAudioSink((pcm,n)->{for(int i=0;i<n;i++)out.add((int)pcm[i]);});return out;}
    private static void frames(Spu s,int n){s.tick(n*768);s.flushAudio();}
    @Test public void decoderSignExtendsLowNibbleFirstAndAppliesShift(){
        byte[] ram=new byte[0x80000];ram[0]=4;ram[2]=(byte)0x8f;int[] samples=new int[28],history=new int[2];
        Spu.decodeBlock(ram,0,samples,history);assertEquals(-256,samples[0]);assertEquals(-2048,samples[1]);assertEquals(0,samples[2]);
    }
    @Test public void predictionUsesPreviousBlockAndSaturates(){
        byte[] ram=new byte[0x80000];ram[0]=0x10;int[] samples=new int[28],history={1000,-2000};
        Spu.decodeBlock(ram,0,samples,history);assertEquals(937,samples[0]);assertEquals(878,samples[1]);int last=history[0];Spu.decodeBlock(ram,0,samples,history);assertEquals((last*60)>>6,samples[0]);
        ram[0]=0x40;Arrays.fill(ram,2,16,(byte)0x77);history[0]=history[1]=32767;Spu.decodeBlock(ram,0,samples,history);for(int sample:samples)assertEquals(32767,sample);
    }
    @Test public void reservedShiftUsesNineAndBlocksWrapRam(){
        byte[] ram=new byte[0x80000];ram[0x7fff8]=15;ram[0x7fffa]=0x11;ram[0]=0x22;int[] samples=new int[28];Spu.decodeBlock(ram,0x7fff8,samples,new int[2]);assertEquals(8,samples[0]);assertEquals(16,samples[12]);
    }
    @Test public void clockProducesExactly44100StereoFramesPerSecond(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);List<Integer> pcm=collect(b.spu);b.spu.tick(767);assertEquals(0,b.spu.frames);b.spu.tick(33868800-767);b.spu.flushAudio();assertEquals(44100,b.spu.frames);assertEquals(88200,pcm.size());assertEquals(0,b.spu.nonzeroFrames);}
    }
    @Test public void loopProducesAudibleStereoAndEndxAndKeyOnClearsIt(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);List<Integer> pcm=collect(b.spu);voice(b,0,7);frames(b.spu,100);assertTrue(b.spu.nonzeroFrames>80);assertEquals(1,b.read16(0x1f801d9c));assertEquals(0x200,b.read16(0x1f801c0e));assertTrue(pcm.get(80)>3900);assertEquals(pcm.get(80),pcm.get(81));b.write16(0x1f801d88,1);frames(b.spu,1);assertEquals(0,b.read16(0x1f801d9c));}
    }
    @Test public void oneShotEndsAndKeyOffReleasesEnvelope(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,1);frames(b.spu,35);assertEquals(1,b.read16(0x1f801d9c));assertEquals(0,b.read16(0x1f801c0c));voice(b,0,7);frames(b.spu,20);assertTrue(b.read16(0x1f801c0c)>30000);b.write16(0x1f801d8c,1);frames(b.spu,5);assertEquals(0,b.read16(0x1f801c0c));}
    }
    @Test public void highVoiceBankAndReadOnlyEndFlags(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,23,7);frames(b.spu,35);assertEquals(128,b.read16(0x1f801d9e));assertEquals(0,b.read16(0x1f801d9c));b.write16(0x1f801d9e,0);assertEquals(128,b.read16(0x1f801d9e));}
    }
    @Test public void muteSuppressesOutputWhileEnvelopeAdvances(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801daa,0x8000);frames(b.spu,100);assertEquals(0,b.spu.nonzeroFrames);assertTrue(b.read16(0x1f801c0c)>30000);b.write16(0x1f801daa,0xc000);frames(b.spu,10);assertTrue(b.spu.nonzeroFrames>0);}
    }
    @Test public void signedVolumeCanInvertOneStereoChannel(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);List<Integer> out=collect(b.spu);voice(b,0,7);b.write16(0x1f801c02,0x4000);frames(b.spu,50);assertTrue(out.get(80)>0);assertTrue(out.get(81)<0);assertEquals(0x8000,b.read16(0x1f801e02));}
    }
    @Test public void pitchChangesBlockConsumptionNotOutputClock(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801c04,0);frames(b.spu,100);long blocks=b.spu.decodedBlocks;b.write16(0x1f801c04,0x4000);frames(b.spu,100);assertEquals(200,b.spu.frames);assertTrue(b.spu.decodedBlocks-blocks>=14);}
    }
    @Test public void voiceFetchIrqAcknowledgesAndRetriggers(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801da4,0x201);b.write16(0x1f801daa,0xc040);b.write16(0x1f801074,512);frames(b.spu,5);assertTrue(b.interruptPending());assertEquals(64,b.read16(0x1f801dae)&64);b.write16(0x1f801daa,0xc000);b.write16(0x1f801070,0xfdff);assertFalse(b.interruptPending());b.write16(0x1f801daa,0xc040);frames(b.spu,28);assertTrue(b.interruptPending());}
    }
    @Test public void captureStoresVoiceAndRaisesIrq(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,1,7);b.write16(0x1f801dac,4);b.write16(0x1f801da4,0x100);b.write16(0x1f801daa,0xc040);frames(b.spu,30);assertEquals(64,b.read16(0x1f801dae)&64);b.write16(0x1f801da6,0x104);assertTrue(b.spu.readTransfer()>3900);frames(b.spu,230);assertEquals(2048,b.read16(0x1f801dae)&2048);}
    }
    @Test public void noiseCanPlayWithoutAdpcmPitch(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801c04,0);b.write16(0x1f801d94,1);b.write16(0x1f801daa,0xff00);frames(b.spu,100);assertTrue(b.spu.nonzeroFrames>50);}
    }
    @Test public void frozenSustainAndReleaseDoNotChangeLevel(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);frames(b.spu,50);int level=b.read16(0x1f801c0c);frames(b.spu,100);assertEquals(level,b.read16(0x1f801c0c));b.write16(0x1f801c0a,0x1fdf);b.write16(0x1f801d8c,1);frames(b.spu,100);assertEquals(level,b.read16(0x1f801c0c));}
    }
    @Test public void oddByteWritesAreIgnoredAndEvenByteWritesUseLowHalfword(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801c00,123);b.write8(0x1f801c01,0xff);assertEquals(123,b.read16(0x1f801c00));b.write8(0x1f801c00,0x12345678);assertEquals(0x5678,b.read16(0x1f801c00));}
    }
    @Test public void cpuStepsDriveSpu(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);for(int i=0;i<1536;i++)c.step();b.flushClocks();assertEquals(2,b.spu.frames);}
    }
    @Test public void attackDecayAndReleaseHaveDistinctRates(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801c08,0);frames(b.spu,4);assertEquals(0,b.read16(0x1f801c0c));frames(b.spu,1);assertEquals(14336,b.read16(0x1f801c0c));frames(b.spu,2);assertEquals(32767,b.read16(0x1f801c0c));frames(b.spu,1);assertEquals(16383,b.read16(0x1f801c0c));b.write16(0x1f801d8c,1);frames(b.spu,1);assertEquals(0,b.read16(0x1f801c0c));}
    }
    @Test public void volumeSweepRetainsLevelThenRampsDown(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);frames(b.spu,1);assertEquals(32766,b.read16(0x1f801e00));b.write16(0x1f801c00,0xa000);frames(b.spu,1);assertEquals(16382,b.read16(0x1f801e00));frames(b.spu,1);assertEquals(0,b.read16(0x1f801e00));}
    }
    @Test public void pitchModulationConsumesMoreBlocksWithPositivePreviousVoice(){
        try(Gpu g=new Gpu(false);Gpu g2=new Gpu(false)){Bus a=new Bus(g),b=new Bus(g2);voice(a,0,7);voice(a,1,7);voice(b,0,7);voice(b,1,7);b.write16(0x1f801d90,2);frames(a.spu,2000);frames(b.spu,2000);assertTrue(b.spu.decodedBlocks>a.spu.decodedBlocks);assertEquals(a.spu.frames,b.spu.frames);}
    }
    @Test public void disabledReverbStillReadsPcmButDoesNotWriteRam(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);List<Integer> pcm=collect(b.spu);b.write16(0x1f801da2,0xffff);b.write16(0x1f801da6,0xffff);for(int i=0;i<4;i++)b.spu.writeTransfer(16000);b.write16(0x1f801dac,4);b.write16(0x1f801d80,0x3fff);b.write16(0x1f801d82,0x3fff);b.write16(0x1f801d84,0x7fff);b.write16(0x1f801d86,0x7fff);b.write16(0x1f801daa,0xc000);frames(b.spu,100);assertTrue(pcm.get(160)>15000);assertEquals(pcm.get(160),pcm.get(161));b.write16(0x1f801da6,0xffff);for(int i=0;i<4;i++)assertEquals(16000,b.spu.readTransfer());}
    }
    @Test public void reverbWritesStayInsideWorkAreaAndCanRaiseIrq(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801dac,4);b.write16(0x1f801d98,1);b.write16(0x1f801da2,0xffff);b.write16(0x1f801da4,0xffff);b.write16(0x1f801dc4,0x7fff);b.write16(0x1f801dfc,0x7fff);b.write16(0x1f801dfe,0x7fff);b.write16(0x1f801daa,0xc0c0);frames(b.spu,40);assertEquals(64,b.read16(0x1f801dae)&64);b.write16(0x1f801da6,0xffff);int value=0;for(int i=0;i<4;i++)value|=b.spu.readTransfer();assertNotEquals(0,value);b.write16(0x1f801da6,0x200);assertEquals(0x700,b.spu.readTransfer());assertEquals(0x1111,b.spu.readTransfer());}
    }

    @Test public void manualRepeatSurvivesLaterLoopStartFlags(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);frames(b.spu,10);
            block(b,0x2000,7,0x2222);b.write16(0x1f801c0e,0x400);frames(b.spu,60);
            assertEquals(0x400,b.read16(0x1f801c0e));assertTrue(b.spu.protectedLoopStarts>0);
            b.write16(0x1f801d88,1);frames(b.spu,6);assertEquals(0x200,b.read16(0x1f801c0e));}
    }
    @Test public void keyOnDelayDoesNotConsumeTheBeginningOfSample(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,7);b.write16(0x1f801da4,0x200);b.write16(0x1f801daa,0xc040);
            frames(b.spu,4);assertEquals(0,b.read16(0x1f801dae)&64);
            frames(b.spu,1);assertTrue(b.spu.decodedBlocks>0);assertEquals(64,b.read16(0x1f801dae)&64);}
    }
    @Test public void releasedVoicesDoNotDecodeWithoutIrq(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);voice(b,0,1);frames(b.spu,100);long count=b.spu.decodedBlocks;
            frames(b.spu,100);assertEquals(count,b.spu.decodedBlocks);}
    }
    @Test public void adpcmPredictorsTruncateSeparatelyWithoutRoundingBias(){
        byte[] ram=new byte[0x80000];ram[0]=0x20;int[] samples=new int[28];
        Spu.decodeBlock(ram,0,samples,new int[]{1,1});assertEquals(0,samples[0]);
    }
    @Test public void reservedFilterDoesNotAliasAValidPredictor(){
        byte[] ram=new byte[0x80000];ram[0]=(byte)0x90;int[] samples=new int[28];
        Spu.decodeBlock(ram,0,samples,new int[]{20000,20000});assertEquals(0,samples[0]);
    }
}
