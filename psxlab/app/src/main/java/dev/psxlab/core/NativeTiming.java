package dev.psxlab.core;

import java.util.*;

/** Cumulative native scopes plus deltas since this owner's previous sample. */
public final class NativeTiming {
    private static final String[] PATHS={"cpu.compile","cpu.cache.check","cpu.cache.clear","gpu.raster.dispatch","gpu.vram.transfer","gpu.vram.readback","gpu.scanout.native","gpu.present.gles.api","gpu.sync.producer","gpu.vulkan.fence_wait","gpu.vulkan.acquire","gpu.vulkan.submit","gpu.vulkan.present","gpu.memory.staging_allocate","cpu.block.build","gpu.vram.resolve"};
    private long[] previous;
    private long previousAt;
    public void reset(){previous=null;previousAt=0;}
    public Map<String,Object> sample(long[] values){
        if(values==null||values.length!=PATHS.length*8)return Map.of("available",false);
        long now=System.nanoTime(),interval=previousAt==0?0:now-previousAt,largest=0;String largestPath="";
        ArrayList<Map<String,Object>> stages=new ArrayList<>();
        for(int i=0;i<PATHS.length;i++){
            int at=i*8;if(values[at]==0)continue;
            long calls=previous==null?values[at]:Math.max(0,values[at]-previous[at]);
            long wall=previous==null?values[at+1]:Math.max(0,values[at+1]-previous[at+1]);
            long cpu=previous==null?values[at+2]:Math.max(0,values[at+2]-previous[at+2]);
            LinkedHashMap<String,Object> row=new LinkedHashMap<>();row.put("path",PATHS[i]);row.put("source",EngineLog.source(PATHS[i]));row.put("tid",values[at+6]);
            row.put("calls",values[at]);row.put("wall_ns",values[at+1]);row.put("cpu_ns",values[at+2]);row.put("max_wall_ns",values[at+3]);row.put("max_cpu_ns",values[at+4]);row.put("max_started_ns",values[at+5]);if(PATHS[i].startsWith("cpu."))row.put("context_pc",String.format(Locale.ROOT,"%08X",values[at+7]));
            row.put("window_calls",calls);row.put("window_wall_ns",wall);row.put("window_cpu_ns",cpu);row.put("window_non_cpu_ns",Math.max(0,wall-cpu));stages.add(row);
            if(wall>largest){largest=wall;largestPath=PATHS[i];}
        }
        previous=values.clone();previousAt=now;
        return Map.of("available",true,"sampled_ns",now,"window_ns",interval,"first_sample",interval==0,"stages",stages,"largest_measured_path",largestPath,"largest_measured_ns",largest,
                "interpretation","host/API scopes; nested timings are not additive; non-CPU time includes waiting and descheduling; hardware GPU time unavailable; largest path is evidence, not proof of root cause");
    }
}
