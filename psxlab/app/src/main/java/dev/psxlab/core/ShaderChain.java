package dev.psxlab.core;

/** Optional shader postprocessing; CPU/GPU emulation remains in PSX Lab. */
public final class ShaderChain {
    private static volatile boolean requested;
    private ShaderChain(){}
    public static boolean requested(){return requested;}
    public static void request(boolean enabled,String preset){requested=enabled&&preset!=null&&!preset.isBlank();if(NativeRasterizer.AVAILABLE)requestNative(requested,preset==null?"":preset);}
    public static boolean active(){return NativeRasterizer.AVAILABLE&&activeNative();}
    public static String status(){return NativeRasterizer.AVAILABLE?statusNative():"Native shader runtime unavailable";}
    public static String parameters(String preset){return NativeRasterizer.AVAILABLE?parametersNative(preset):"[]";}
    public static void parameters(String preset,String[] names,float[] values){if(NativeRasterizer.AVAILABLE)parametersNative(preset,names,values);}
    public static void backend(int mode){if(NativeRasterizer.AVAILABLE)backendNative(mode);}
    static void phase(int total,int current,float fps,float aspect){if(requested&&NativeRasterizer.AVAILABLE)phaseNative(total,current,fps,aspect);}
    static void prepare(){if(requested&&NativeRasterizer.AVAILABLE)prepareNative();}
    private static native void requestNative(boolean enabled,String preset);
    private static native boolean activeNative();
    private static native String statusNative();
    private static native String parametersNative(String preset);
    private static native void parametersNative(String preset,String[] names,float[] values);
    private static native void backendNative(int mode);
    private static native void phaseNative(int total,int current,float fps,float aspect);
    private static native void prepareNative();
}
