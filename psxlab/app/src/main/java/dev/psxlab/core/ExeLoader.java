package dev.psxlab.core;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class ExeLoader {
    public static final int BIOS_EXE_HEADER=0x8000b070;
    public static int le(byte[] b,int p){return (b[p]&255)|(b[p+1]&255)<<8|(b[p+2]&255)<<16|b[p+3]<<24;}
    public static Cpu load(byte[] exe,Bus bus)throws IOException{
        return loadInto(exe,bus,new Cpu(bus));
    }
    /** Preserve the initialized kernel's COP0 and device state when loading a game. */
    public static Cpu loadInto(byte[] exe,Bus bus,Cpu cpu)throws IOException{
        if(exe.length<2048 || !new String(exe,0,8,StandardCharsets.US_ASCII).equals("PS-X EXE"))throw new IOException("Not a PS-X EXE executable");
        int pc=le(exe,16),gp=le(exe,20),dest=le(exe,24),size=le(exe,28);
        int bss=le(exe,40),bssSize=le(exe,44),stack=le(exe,48),stackOffset=le(exe,52);
        range(dest,size);range(pc,4);
        if((pc&3)!=0 || (dest&3)!=0 || size<4 || size>exe.length-2048)throw new IOException("Invalid or truncated executable payload");
        if(bssSize!=0)range(bss,bssSize);
        // LoadExe leaves the complete first sector in its resident header buffer.
        // Translated executables can call code in the otherwise unused header bytes.
        // Copy before the payload/BSS, preserving the BIOS load order if they overlap.
        for(int i=0;i<2048;i++)bus.write8(BIOS_EXE_HEADER+i,exe[i]);
        for(int i=0;i<size;i++)bus.write8(dest+i,exe[2048+i]);
        for(int i=0;i<bssSize;i++)bus.write8(bss+i,0);
        cpu.jump(pc);cpu.r[28]=gp;
        if(stack!=0)cpu.r[29]=cpu.r[30]=stack+stackOffset;
        else if(cpu.r[29]==0)cpu.r[29]=cpu.r[30]=0x801ffff0;
        cpu.r[4]=1;cpu.r[5]=0;
        return cpu;
    }
    private static void range(int address,int size)throws IOException{
        int p=address&0x1fffffff;
        if(size<0 || p>=0x200000 || (long)p+size>0x200000)throw new IOException("Executable range exceeds PS1 RAM");
    }
}
