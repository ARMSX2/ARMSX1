package dev.psxlab.core;

import java.nio.*;
import java.util.*;

/** Original native CPU block runner; buffers stay owned by this Java object. */
final class NativeCpu implements AutoCloseable {
    private final ByteBuffer state=ByteBuffer.allocateDirect(584).order(ByteOrder.LITTLE_ENDIAN);
    private final IntBuffer words=state.asIntBuffer();
    private final ByteBuffer memory;
    private final Bus bus;
    private final boolean ramWaitStates;
    private long handle;
    private boolean profiling;
    private final NativeTiming timing=new NativeTiming();
    void configureProfiling(boolean enabled){if(handle!=0&&profiling!=enabled){setProfilingNative(handle,enabled);profiling=enabled;if(enabled)timing.reset();}}
    Map<String,Object> nativeTimings(){long[] s=statisticsNative(handle);return Map.of("timing",timing.sample(timingsNative(handle)),"cache",Map.of("compiled",s[0],"page_generation_misses",s[5],"unchanged_blocks_reused",s[8],"process_jit_mapped_bytes",s[9],"changed_block_pc",String.format(Locale.ROOT,"%08X",s[10]),"changed_instruction_address",String.format(Locale.ROOT,"%08X",s[11]),"previous_instruction",String.format(Locale.ROOT,"%08X",s[12]),"replacement_instruction",String.format(Locale.ROOT,"%08X",s[13]),"capacity_clears",s[14]));}
    private static native void setProfilingNative(long handle,boolean enabled);
    private static native long[] timingsNative(long handle);

    private int projectionScale=65536;
    NativeCpu(Bus bus,boolean recompile,boolean ramWaitStates){this.bus=bus;this.ramWaitStates=ramWaitStates;memory=bus.enableNativeMemory();handle=create(bus,state,memory,(recompile?1:0)|(ramWaitStates?2:0));if(handle==0)throw new IllegalStateException("Native CPU initialization failed");}
    void importState(Cpu cpu){
        int requested=cpu.gte.projectionScale();if(requested!=projectionScale){projectionScale=requested;setProjectionNative(handle,requested);}
        words.position(0);words.put(cpu.r).put(cpu.cop0);cpu.exportPipeline(words);
        state.putLong(312,cpu.instructions);state.putLong(320,cpu.exceptions);
        words.position(82);words.put(cpu.gte.nativeState());
    }
    void exportState(Cpu cpu){
        words.position(0);words.get(cpu.r).get(cpu.cop0);cpu.importPipeline(words);
        cpu.instructions=state.getLong(312);cpu.exceptions=state.getLong(320);
        words.position(82);words.get(cpu.gte.nativeState());
    }
    int run(Cpu cpu,int count){return run(cpu,count,false);}
    int run(Cpu cpu,int count,boolean stopAtFrame){importState(cpu);bus.beginNativeFrame();try{return runNative(handle,count,stopAtFrame);}finally{exportState(cpu);}}
    String diagnostic(){long[] s=statisticsNative(handle);return "CPU native blocks · RAM waits="+(ramWaitStates?"6":"off")+" · ARM64 blocks="+s[0]+" JIT instructions="+s[1]+" interpreted="+s[2]+" hits="+s[3]+" links="+s[4]+" invalidations="+s[5]+" device/MMIO calls="+s[6]+" compile failures="+s[7]+" unchanged blocks reused="+s[8]+" process JIT mapped bytes="+s[9];}
    long[] statistics(){return statisticsNative(handle);}
    @Override public void close(){if(handle!=0){destroy(handle);handle=0;}}
    private static native long create(Bus bus,ByteBuffer state,ByteBuffer memory,int options);
    private static native void setProjectionNative(long handle,int projectionScale);
    private static native int runNative(long handle,int count,boolean stopAtFrame);
    private static native long[] statisticsNative(long handle);
    private static native void destroy(long handle);
}
