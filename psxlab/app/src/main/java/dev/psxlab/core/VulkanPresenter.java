package dev.psxlab.core;
import android.view.Surface;
/** Vulkan upload/scaling/presentation; game primitives use our native software rasterizer. */
public final class VulkanPresenter implements AutoCloseable {
    private long handle;
    public VulkanPresenter(){this(1);}
    public VulkanPresenter(int scale){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native display unavailable");if(scale<1||scale>5)throw new IllegalArgumentException("Invalid HD scale");handle=create(1024*512*scale*scale);}
    boolean present(Surface surface,Gpu.Frame frame,boolean filter){return present(surface,frame,filter,0);}
    boolean present(Surface surface,Gpu.Frame frame,boolean filter,long token){return presentNative(handle,surface,frame.pixels(),frame.width(),frame.height(),filter,token);}
    synchronized long[] counters(){long[] out=new long[6];if(handle!=0)statisticsNative(handle,out);return out;}
    synchronized String statistics(){long[] s=counters();return "Vulkan presented="+s[0]+" reusedSurface="+s[1]+" uploadBytes="+s[2]+" swapchains="+s[3]+" suboptimal="+s[4]+" outdated="+s[5];}
    long nativeHandle(){return handle;}
    @Override public synchronized void close(){if(handle!=0){destroy(handle);handle=0;}}
    public static String verify(){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native display unavailable");return verifyNative();}
    private static native long create(int maxPixels);
    private static native void destroy(long handle);
    private static native boolean presentNative(long handle,Surface surface,int[] pixels,int width,int height,boolean filter,long token);
    private static native void statisticsNative(long handle,long[] out);
    private static native String verifyNative();
}
