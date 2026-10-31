package dev.psxlab.core;

/** Silent native fixtures: host block caching must preserve uncached ROM waits. */
public final class NativeMemoryTimingVerifier {
    private static void check(boolean jit,int pc,int[] code,long expected)throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);byte[] bios=new byte[512*1024];
            for(int i=0;i<code.length;i++)for(int j=0;j<4;j++)bios[i*4+j]=(byte)(code[i]>>>(j*8));
            bus.loadBios(bios);Cpu cpu=new Cpu(bus);cpu.jump(pc);
            if(!cpu.enableNative(jit,true,true))throw new AssertionError("Native CPU unavailable");
            for(int i=0;i<code.length;i++)cpu.run(1);
            if(cpu.instructions!=expected || cpu.pc!=pc+code.length*4)throw new AssertionError("ROM clocks "+cpu.instructions+" != "+expected+" PC="+Integer.toHexString(cpu.pc));
            cpu.closeNative();
        }
    }
    public static String verify()throws Exception{
        for(boolean jit:new boolean[]{false,true}){
            check(jit,0xbfc00000,new int[]{0,0,0,0},100);
            check(jit,0x9fc00000,new int[]{0,0,0,0},4);
            check(jit,0xbfc00000,new int[]{0x3c08bfc0,0x91090000,0x950a0000,0x8d0b0000},142);
            check(jit,0x9fc00000,new int[]{0x3c08bfc0,0x91090000,0x950a0000,0x8d0b0000},46);
            // Change COM_DELAY in running MIPS code; the following fetch uses 32 waits.
            check(jit,0xbfc00000,new int[]{0x3c081f80,0x34091325,0xad091020,0,0},141);
        }
        long[] clocks=new long[2];
        for(int mode=0;mode<2;mode++)try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);bus.loadBios(new byte[512*1024]);Cpu cpu=new Cpu(bus);cpu.jump(0xbfc00000);cpu.enableNative(mode==1,true,true);
            cpu.run(20000);clocks[mode]=cpu.instructions;cpu.closeNative();
        }
        if(clocks[0]!=20000 || clocks[1]!=clocks[0])throw new AssertionError("JIT ROM fetch timing differs");
        for(boolean jit:new boolean[]{false,true})for(boolean recovery:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(recovery)bus.write32(0x1f801018,0x00020943);int[] code={0x3c081f80,0x91091800,0x910a1803,0};for(int i=0;i<code.length;i++)bus.write32(0x10000+i*4,code[i]);Cpu cpu=new Cpu(bus);cpu.enableNative(jit,true,true);
            for(int i=0;i<code.length;i++)cpu.run(1);long expected=recovery?24:16;
            if(cpu.instructions!=expected || cpu.r[9]!=0x18 || cpu.r[10]!=0xe0)throw new AssertionError("CD register read wait clocks: "+cpu.instructions+" != "+expected);cpu.closeNative();
        }
        for(boolean jit:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);int[] code={0x3c081f80,0x24090001,0xa1091800,0x240a001f,0xa10a1802,0xa1001800,0xa1091801,0xa1091800,0x91021803,0,0x30420007,0x1040fffc,0,0x91031801,0,0x0800400f,0};
            for(int i=0;i<code.length;i++)bus.write32(0x10000+i*4,code[i]);Cpu cpu=new Cpu(bus);cpu.enableNative(jit,true,true);cpu.run(40000);
            if(cpu.r[2]!=3 || cpu.r[3]!=16 || bus.cdrom.commands!=1 || (bus.cdrom.statusRegister()&32)!=0)throw new AssertionError("Native CD polling did not observe ACK/result/bank changes");cpu.closeNative();
        }
        return "Native ROM/CD timing: uncached/cached fetches, live COM_DELAY updates, CD read waits and status/bank/result polling match in JIT/interpreter modes; no host audio created.\n";
    }
}
