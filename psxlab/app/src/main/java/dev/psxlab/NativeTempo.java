package dev.psxlab;

/** SoundTouch DSP bridge. All processing and close are serialized by AudioOutput. */
final class NativeTempo implements AutoCloseable {
    static{System.loadLibrary("psxtempo");}
    private long handle=create();
    void feed(short[] pcm,int count,float tempo){put(handle,pcm,count,tempo);}
    int receive(short[] output){return read(handle,output);}
    void finish(){flush(handle);}
    @Override public void close(){if(handle!=0){destroy(handle);handle=0;}}
    private static native long create();
    private static native void put(long handle,short[] pcm,int count,float tempo);
    private static native int read(long handle,short[] output);
    private static native void flush(long handle);
    private static native void destroy(long handle);
}
