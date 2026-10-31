package dev.psxlab.core;

import java.nio.*;

/** Original native CPU block runner; buffers stay owned by this Java object. */
final class NativeCpu implements AutoCloseable {
    private final ByteBuffer state=ByteBuffer.allocateDirect(584).order(ByteOrder.LITTLE_ENDIAN);
    private final IntBuffer words=state.asIntBuffer();
    private final ByteBuffer memory;
    private final Bus bus;
    private final boolean ramWaitStates;
    private long handle;
    private boolean widescreen;
    NativeCpu(Bus bus,boolean recompile,boolean ramWaitStates){this.bus=bus;this.ramWaitStates=ramWaitStates;memory=bus.enableNativeMemory();handle=create(bus,state,memory,(recompile?1:0)|(ramWaitStates?2:0));if(handle==0)throw new IllegalStateException("Native CPU initialization failed");}
    void importState(Cpu cpu){
        boolean requested=cpu.gte.widescreenEnabled();if(requested!=widescreen){widescreen=requested;setProjectionNative(handle,requested);}
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
    private static native void setProjectionNative(long handle,boolean widescreen);
    private static native int runNative(long handle,int count,boolean stopAtFrame);
    private static native long[] statisticsNative(long handle);
    private static native void destroy(long handle);
}
