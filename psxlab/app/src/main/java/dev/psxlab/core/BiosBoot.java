package dev.psxlab.core;
import java.io.*;
import java.util.function.BooleanSupplier;

/** PS-X EXE fast-load handoff after the BIOS initializes its resident kernel. */
public final class BiosBoot {
    public static final int SHELL_ENTRY=0x80030000;
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
