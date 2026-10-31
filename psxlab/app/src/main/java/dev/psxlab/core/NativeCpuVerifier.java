package dev.psxlab.core;

import java.nio.*;
import java.util.*;

/** On-device differential checks against our retained Java interpreter. */
public final class NativeCpuVerifier {
    public static String verifyRamReadTiming(){
        for(boolean jit:new boolean[]{false,true})for(boolean scratch:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);int[] code={scratch?0x3c081f80:0x3c088001,0x8d090200,0,0x01295021,0xad0a0204,0x08004000,0};
            for(int i=0;i<code.length;i++)bus.write32(0x80010000+i*4,code[i]);int data=scratch?0x1f800200:0x80010200;bus.write32(data,7);
            if(!cpu.enableNative(jit,true,true))throw new AssertionError("Native timing core unavailable");int clocks=scratch?7000:13000;cpu.run(clocks);cpu.closeNative();bus.flushClocks();bus.spu.flushAudio();
            if(cpu.instructions!=clocks || cpu.pc!=0x80010000 || bus.read32(data+4)!=14 || bus.spu.frames!=clocks/768)throw new AssertionError("RAM timing mismatch jit="+jit+" scratch="+scratch+" clocks="+cpu.instructions+" PC="+Integer.toHexString(cpu.pc)+" samples="+bus.spu.frames);
        }
        // A compiled RAM load followed by a scratchpad exit must charge only
        // the executed RAM prefix, then resume the interpreter without double billing.
        for(boolean jit:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
            int[] code={0x3c088001,0x8d090200,0,0x3c0a1f80,0x8d4b0200,0,0x012b6021,0xad0c0204,0x08004000,0};
            for(int i=0;i<code.length;i++)bus.write32(0x80010000+i*4,code[i]);
            bus.write32(0x80010200,7);bus.write32(0x1f800200,11);
            if(!cpu.enableNative(jit,true,true))throw new AssertionError("Native timing core unavailable");
            cpu.run(16000);cpu.closeNative();bus.flushClocks();bus.spu.flushAudio();
            if(cpu.instructions!=16000 || cpu.pc!=0x80010000 || bus.read32(0x80010204)!=18 || bus.spu.frames!=16000/768)
                throw new AssertionError("RAM/scratchpad compiled-prefix timing mismatch jit="+jit+" clocks="+cpu.instructions);
        }
        return "PASS: RAM reads add six guest clocks, scratchpad reads add none; interpreter/JIT and partial compiled-block exits agree, with exact SPU clock advancement.\n";
    }
    private static void same(Cpu a,Cpu b,Bus ba,Bus bb,String where){
        IntBuffer x=IntBuffer.allocate(78),y=IntBuffer.allocate(78);x.put(a.r).put(a.cop0);a.exportPipeline(x);y.put(b.r).put(b.cop0);b.exportPipeline(y);
        if(!Arrays.equals(x.array(),y.array())||a.instructions!=b.instructions||a.exceptions!=b.exceptions){int i=0;while(i<78&&x.get(i)==y.get(i))i++;throw new AssertionError(where+" CPU word="+i+(i<78?String.format(" ref=%08X native=%08X",x.get(i),y.get(i)):"")+"\n"+a.diagnostic()+"\n"+b.diagnostic());}
        for(int i=0;i<32;i++)if(a.gte.readData(i)!=b.gte.readData(i)||a.gte.readControl(i)!=b.gte.readControl(i))throw new AssertionError(where+" GTE register "+i);
        if(ba.gpu.vblanks!=bb.gpu.vblanks || ba.spu.frames!=bb.spu.frames)throw new AssertionError(where+" device clock mismatch");
    }
    public static String verify(){
        if(!NativeCore.AVAILABLE)throw new AssertionError("Native CPU unavailable");
        Random rng=new Random(0x435055);long compared=0,jitted=0,invalidated=0;
        int[] special={0,2,3,4,6,7,16,17,18,19,24,25,26,27,32,33,34,35,36,37,38,39,42,43};
        int[] primary={8,9,10,11,12,13,14,15,32,33,34,35,36,37,38,40,41,42,43,46};
        for(boolean jit:new boolean[]{false,true})for(int trial=0;trial<48;trial++)try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus ba=new Bus(ga),bb=new Bus(gb);Cpu a=new Cpu(ba),b=new Cpu(bb);
            for(int i=0;i<32;i++)a.r[i]=b.r[i]=rng.nextInt();a.r[0]=b.r[0]=0;a.r[20]=b.r[20]=0x80080000;
            a.cop0[12]=b.cop0[12]=0x40000000;
            // Exceptions return to their EPC+4; delay-slot exception details are compared too.
            int[] handler={0x401a7000,0,0x275a0004,0x03400008,0x42000010};for(int i=0;i<handler.length;i++){ba.write32(0x80+i*4,handler[i]);bb.write32(0x80+i*4,handler[i]);}
            for(int i=0;i<256;i++){int value=rng.nextInt();ba.write32(0x80000+i*4,value);bb.write32(0x80000+i*4,value);}
            for(int i=0;i<512;i++){
                int s=1+rng.nextInt(18),t=1+rng.nextInt(18),d=1+rng.nextInt(18),word;
                if(i%11==0)word=(4+rng.nextInt(4))<<26|s<<21|t<<16|(rng.nextInt(9)-4)&65535;
                else if(rng.nextBoolean()){int f=special[rng.nextInt(special.length)];word=s<<21|t<<16|d<<11|rng.nextInt(32)<<6|f;}
                else{int op=primary[rng.nextInt(primary.length)],imm=rng.nextInt(32768);if(op>=32){s=20;imm=rng.nextInt(1000);if(op==33||op==37||op==41)imm&=~1;if(op==35||op==43)imm&=~3;}word=op<<26|s<<21|t<<16|imm;}
                ba.write32(0x10000+i*4,word);bb.write32(0x10000+i*4,word);
            }
            a.jump(0x80010000);b.jump(0x80010000);b.enableNative(jit,false);
            try{
                for(int block=0;block<64;block++){int n=1+rng.nextInt(97);for(int i=0;i<n;i++)a.step();b.run(n);compared+=n;same(a,b,ba,bb,"trial="+trial+" jit="+jit+" block="+block);}
                if(!Arrays.equals(ba.snapshotRam(),bb.snapshotRam()))throw new AssertionError("Native RAM differs trial="+trial);
                jitted+=b.nativeStatistics()[1];
            }finally{b.closeNative();}
        }
        // Cached links must reject code changed through byte/half/word physical aliases.
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus ba=new Bus(ga),bb=new Bus(gb);Cpu a=new Cpu(ba),b=new Cpu(bb);
            for(Bus bus:new Bus[]{ba,bb}){bus.write32(0x10000,0x24210001);bus.write32(0x10004,0x08004000);bus.write32(0x10008,0);}
            a.jump(0x80010000);b.jump(0x80010000);b.enableNative(true,false);
            try{for(int mutation=0;mutation<48;mutation++){
                int n=100;for(int i=0;i<n;i++)a.step();b.run(n);same(a,b,ba,bb,"self modifying "+mutation);
                int value=1+(mutation&7);switch(mutation%3){case 0->{ba.write8(0xa0010000,value);bb.write8(0xa0010000,value);}case 1->{ba.write16(0x10000,value);bb.write16(0x10000,value);}default->{ba.write32(0x80010000,0x24210000|value);bb.write32(0x80010000,0x24210000|value);}}
                compared+=n;
            }invalidated=b.nativeStatistics()[5];jitted+=b.nativeStatistics()[1];}finally{b.closeNative();}
        }
        if(invalidated==0)throw new AssertionError("Native code invalidation was not exercised");
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus ba=new Bus(ga),bb=new Bus(gb);Cpu a=new Cpu(ba),b=new Cpu(bb);
            for(Bus bus:new Bus[]{ba,bb}){bus.write32(0x10040,0x24840001);bus.write32(0x10044,0x08004010);bus.write32(0x10048,0);bus.write32(0x10000,0xac430040);bus.write32(0x10004,0x08004010);bus.write32(0x10008,0);}
            a.r[2]=b.r[2]=0x80010000;a.r[3]=b.r[3]=0x24840009;a.jump(0x80010040);b.jump(0x80010040);b.enableNative(true,false);
            try{
                a.run(200);b.run(200);long before=b.nativeStatistics()[5];
                ba.write32(0x10f00,123);bb.write32(0x10f00,123);a.run(100);b.run(100);same(a,b,ba,bb,"data sharing code page");
                if(b.nativeStatistics()[5]!=before)throw new AssertionError("Unrelated data invalidated executable code");
                a.jump(0x80010000);b.jump(0x80010000);a.run(200);b.run(200);same(a,b,ba,bb,"translated self modifying store");
                if(b.nativeStatistics()[5]==before)throw new AssertionError("Translated store failed to invalidate code");
                jitted+=b.nativeStatistics()[1];compared+=500;
            }finally{b.closeNative();}
        }
        // Folded branches must capture operands before committing a pending load,
        // including JR/JALR where the link register aliases the jump register.
        for(int variant=0;variant<72;variant++)try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus ba=new Bus(ga),bb=new Bus(gb);Cpu a=new Cpu(ba),b=new Cpu(bb);
            int value=variant%3==0?-1:variant%3==1?0:1;a.r[1]=b.r[1]=value;a.r[2]=b.r[2]=0;a.r[3]=b.r[3]=0x80010040;
            int word=switch(variant%12){case 0->0x10220004;case 1->0x14220004;case 2->0x18200004;case 3->0x1c200004;case 4->0x04200004;case 5->0x04210004;case 6->0x04300004;case 7->0x04310004;case 8->0x08004010;case 9->0x0c004010;case 10->0x00600008;default->0x00601809;};
            for(Bus bus:new Bus[]{ba,bb}){bus.write32(0x10000,0x8c010200);bus.write32(0x10004,word);bus.write32(0x10008,0);bus.write32(0x200,1-value);}
            a.jump(0x80010000);b.jump(0x80010000);b.enableNative(true,false);
            try{a.step();b.run(1);a.run(2);b.run(2);same(a,b,ba,bb,"folded branch "+variant);jitted+=b.nativeStatistics()[1];compared+=3;}finally{b.closeNative();}
        }
        if(System.getProperty("os.arch","").contains("aarch64")&&jitted==0)throw new AssertionError("ARM64 translator did not execute");
        return "Native CPU: "+compared+" instructions match Java state/RAM, including traps, branch/load delays, native block interpreter, ARM64 fast RAM and "+invalidated+" code invalidations; "+jitted+" translated instructions executed.\n";
    }
}
