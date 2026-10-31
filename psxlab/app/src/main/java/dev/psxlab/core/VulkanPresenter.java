package dev.psxlab.core;
import android.view.Surface;
/** Vulkan upload/scaling/presentation; game primitives use our native software rasterizer. */
public final class VulkanPresenter implements AutoCloseable {
    private long handle;
    public VulkanPresenter(){this(1);}
    public VulkanPresenter(int scale){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native display unavailable");if(scale<1||scale>5)throw new IllegalArgumentException("Invalid HD scale");handle=create(1024*512*scale*scale);}
    boolean present(Surface surface,Gpu.Frame frame,boolean filter){return presentNative(handle,surface,frame.pixels(),frame.width(),frame.height(),filter);}
    long nativeHandle(){return handle;}
    @Override public void close(){if(handle!=0){destroy(handle);handle=0;}}
    public static String verify(){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native display unavailable");return verifyNative();}
    private static native long create(int maxPixels);
    private static native void destroy(long handle);
    private static native boolean presentNative(long handle,Surface surface,int[] pixels,int width,int height,boolean filter);
    private static native String verifyNative();
}
