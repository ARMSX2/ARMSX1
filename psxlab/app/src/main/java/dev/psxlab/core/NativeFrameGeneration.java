package dev.psxlab.core;

/** CPU frame generation for the software display (native psxfg::CpuFrameGen). GPU worker only. */
final class NativeFrameGeneration implements AutoCloseable {
    private long handle;
    private int width,height;
    private int[] scratch;
    NativeFrameGeneration(){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native frame generation unavailable");handle=createNative();}
    /** Make this the newest frame; true when an in-between picture can now be made. */
    boolean capture(Gpu.Frame frame,boolean estimate,boolean reset){width=frame.width();height=frame.height();return captureNative(handle,frame.pixels(),width,height,estimate,reset);}
    /** The picture at time t, or null when there is no motion to interpolate along. */
    Gpu.Frame interpolate(float t,boolean nearest){
        if(width<1||height<1)return null;
        int[] out=new int[width*height];
        return interpolateNative(handle,t,nearest,out)?new Gpu.Frame(width,height,out):null;
    }
    @Override public void close(){if(handle!=0){destroyNative(handle);handle=0;}}
    private static native long createNative();
    private static native void destroyNative(long handle);
    private static native boolean captureNative(long handle,int[] pixels,int width,int height,boolean estimate,boolean reset);
    private static native boolean interpolateNative(long handle,float t,boolean nearest,int[] out);
}
