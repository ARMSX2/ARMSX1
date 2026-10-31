package dev.psxlab.core;
import java.nio.*;

/** Java owns the native state memory; MMIO and sample operations allocate nothing. */
final class NativeSpu {
    private final ByteBuffer state=ByteBuffer.allocateDirect(stateSize()).order(ByteOrder.nativeOrder());
    private final LongBuffer counters=state.asLongBuffer();
    private final Bus bus;
    NativeSpu(Bus bus){this.bus=bus;initialize(state);}
    int op(int op,int a,int b){long result=operation(state,op,a,b);if((result>>>32)!=0)bus.raiseInterrupt(9);return (int)result;}
    long counter(int n){return counters.get(n);}
    byte[] snapshot(){byte[] bytes=new byte[state.capacity()];state.duplicate().get(bytes);return bytes;}
    void restore(byte[] bytes){if(bytes.length!=state.capacity())throw new IllegalArgumentException("Native SPU state size differs");state.duplicate().put(bytes);}
    private static native int stateSize();
    private static native void initialize(ByteBuffer state);
    private static native long operation(ByteBuffer state,int op,int a,int b);
}
