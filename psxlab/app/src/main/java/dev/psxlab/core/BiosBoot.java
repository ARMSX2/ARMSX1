package dev.psxlab.core;
import java.io.*;
import java.util.function.BooleanSupplier;

/** PS-X EXE fast-load handoff after the BIOS initializes its resident kernel. */
public final class BiosBoot {
    public static final int SHELL_ENTRY=0x80030000;
    /** Skip the shell, then let the firmware perform LoadExe/Exec and its thread setup. */
    public static Cpu discBoot(byte[] bios,Bus bus,BooleanSupplier cancelled)throws IOException{
        Cpu cpu=initialize(bios,bus,cancelled);int caller=cpu.r[31],physical=caller&0x1fffffff;
        // Unknown firmware keeps its normal shell rather than guessing a return address.
        if((caller&3)!=0||physical<0x1fc00000||physical>=0x1fc80000)return cpu;
        int[] returnToBootstrap={0x3c011f80,0x3c0a0300,0xac2a1814,0x03e00008,0};
        for(int i=0;i<returnToBootstrap.length;i++)bus.write32(SHELL_ENTRY+i*4,returnToBootstrap[i]);
        return cpu;
    }
    public static Cpu initialize(byte[] bios,Bus bus,BooleanSupplier cancelled)throws IOException{
        bus.loadBios(bios);Cpu cpu=new Cpu(bus);cpu.reset();
        while(cpu.pc!=SHELL_ENTRY){
            if((cpu.instructions&4095)==0 && (cancelled.getAsBoolean() || Thread.currentThread().isInterrupted()))throw new InterruptedIOException("BIOS initialization cancelled");
            if(cpu.instructions>=5000000)throw new IOException(String.format("BIOS did not reach the executable handoff; PC %08X",cpu.pc));
            cpu.step();
        }
        boolean handler=false;for(int i=0;i<16;i+=4)handler|=bus.read32(0x80000080+i)!=0;
        if(!handler || bus.cacheIsolated)throw new IOException("BIOS reached shell without a usable resident exception handler");
        return cpu;
    }
}
