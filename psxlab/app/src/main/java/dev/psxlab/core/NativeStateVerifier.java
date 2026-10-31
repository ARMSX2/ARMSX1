package dev.psxlab.core;
import java.util.*;
import java.util.zip.CRC32;

/** Silent on-device state test: active native voices, JIT, device clocks and memory. */
public final class NativeStateVerifier {
    private static void voice(Bus b,int n){int address=0x1000+n*16;b.write16(0x1f801da6,address/8);b.spu.writeTransfer(7<<8);for(int i=0;i<7;i++)b.spu.writeTransfer(0x1234+n*0x1111);int r=0x1f801c00+n*16;b.write16(r,0x3fff);b.write16(r+2,0x3fff);b.write16(r+4,0x0800+n*17);b.write16(r+6,address/8);b.write16(r+8,0xff);b.write16(r+10,0x1fc0);b.write16(0x1f801d80,0x3fff);b.write16(0x1f801d82,0x3fff);b.write16(0x1f801daa,0xc000);b.write16(0x1f801d88,1<<n);}
    private static void record(Spu s,CRC32 crc){s.setAudioSink((pcm,count)->{for(int i=0;i<count;i++){crc.update(pcm[i]&255);crc.update(pcm[i]>>>8&255);}});}
    public static String verify()throws Exception{
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);Cpu ca=new Cpu(a),cb=new Cpu(b);int[] code={0x3c088001,0x8d090200,0,0x25290001,0xad090200,0x08004000,0};for(int i=0;i<code.length;i++)a.write32(0x10000+i*4,code[i]);for(int i=0;i<4;i++)voice(a,i);
            if(!ca.enableNative(true,true,true))throw new AssertionError("Native CPU unavailable");ca.run(260000);byte[] state=SaveState.capture(ca,a,ga,"native-state-fixture");CRC32 expected=new CRC32(),actual=new CRC32();record(a.spu,expected);ca.run(1300000);a.flushClocks();a.spu.flushAudio();
            SaveState.restore(state,cb,b,gb,"native-state-fixture");if(!cb.enableNative(true,true,true))throw new AssertionError("Restored native CPU unavailable");record(b.spu,actual);cb.run(1300000);b.flushClocks();b.spu.flushAudio();
            if(expected.getValue()!=actual.getValue() || a.spu.nonzeroFrames==0 || !Arrays.equals(a.snapshotRam(),b.snapshotRam()) || !Arrays.equals(ca.r,cb.r) || ca.pc!=cb.pc || ca.instructions!=cb.instructions || a.spu.frames!=b.spu.frames || ga.vblanks!=gb.vblanks || !Arrays.equals(a.spu.snapshotNative(),b.spu.snapshotNative()))throw new AssertionError("Native state continuation differs: PCM "+expected.getValue()+"/"+actual.getValue()+" clocks "+ca.instructions+"/"+cb.instructions);
            ca.closeNative();cb.closeNative();return "Native save state: restored JIT/RAM, four active SPU voices, native voice/reverb state and device clocks reproduce 1,300,000 guest clocks byte-for-byte; PCM CRC="+Long.toHexString(actual.getValue())+", state bytes="+state.length+".\n";
        }
    }
}
