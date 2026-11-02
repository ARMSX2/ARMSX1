package dev.psxlab.core;
import java.util.*;

/** Exact CPU-visible MMIO counts; hot polling is evidence, not proof of a bottleneck. */
public final class HardwareRegisterReport {
    public static Map<String,Object> format(long[] values){
        if(values==null||values.length<5||(values.length-5)%5!=0||values[0]==0)return Map.of("available",false);
        List<Map<String,Object>> rows=new ArrayList<>();
        for(int at=5;at<values.length;at+=5){int address=(int)values[at];rows.add(Map.of("address",String.format(Locale.ROOT,"%08X",address),"register",name(address),"reads",values[at+1],"writes",values[at+2],"read_width_mask",values[at+3]&255,"write_width_mask",values[at+3]>>>8,"last_pc",String.format(Locale.ROOT,"%08X",(int)values[at+4]),"source","psxlab/app/src/main/cpp/cpu_jni.cpp#CpuCore::read/write"));}
        return Map.of("available",true,"reads",values[1],"writes",values[2],"other_mmio_reads",values[3],"other_mmio_writes",values[4],"hot_registers",rows,"interpretation","Cumulative since the game started; widths 1/2/4 mean 8/16/32-bit. Includes native CD status latches. Counts do not measure register wait time.");
    }
    public static String name(int address){
        if(address>=0x1f801080&&address<0x1f8010f0)return "DMA"+((address-0x1f801080)/16)+" "+switch(address&15){case 0->"MADR";case 4->"BCR";case 8->"CHCR";default->"register";};
        if(address>=0x1f801100&&address<0x1f801130)return "Timer "+((address-0x1f801100)/16);
        if(address>=0x1f801c00&&address<0x1f802000)return "SPU";
        return switch(address){case 0x1f801040->"Controller data";case 0x1f801044->"Controller status";case 0x1f801070->"I_STAT";case 0x1f801074->"I_MASK";case 0x1f8010f0->"DPCR";case 0x1f8010f4->"DICR";case 0x1f801800->"CD index/status";case 0x1f801801->"CD response";case 0x1f801802->"CD data";case 0x1f801803->"CD interrupt";case 0x1f801810->"GPU GP0/GPUREAD";case 0x1f801814->"GPU GP1/GPUSTAT";case 0x1f801820->"MDEC data";case 0x1f801824->"MDEC status";default->"Hardware register";};
    }
}
