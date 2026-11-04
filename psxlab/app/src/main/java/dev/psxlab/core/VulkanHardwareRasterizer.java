package dev.psxlab.core;

import android.view.Surface;
import java.util.Map;

/** Native Vulkan drawing on the presentation device; no EGL context or shared-buffer handoff. */
final class VulkanHardwareRasterizer extends HardwareRasterizer {
    private final VulkanPresenter output;
    private long nativeGpu;
    private volatile HardwareRasterizer compatibility;
    private float aspect=4f/3f;
    private boolean stretch,integerScale,vsync,profiling;
    private float[] window={0,0,1,1};

    VulkanHardwareRasterizer(int scale,VulkanPresenter output){super(scale,true);this.output=output;nativeGpu=createNative(output.nativeHandle(),scale);}
    @Override boolean nativeVulkan(){return compatibility==null;}
    @Override void drawBatch(int[] words){if(compatibility!=null)compatibility.drawBatch(words);else batchNative(nativeGpu,words);}
    @Override void readVram(int[] out){if(compatibility!=null)compatibility.readVram(out);else readNative(nativeGpu,out);}
    @Override void setVram(int[] in){if(compatibility!=null)compatibility.setVram(in);else setNative(nativeGpu,in);}
    @Override void setDisplayVram(int[] in,int[] state,long revision){if(compatibility!=null)compatibility.setDisplayVram(in,state,revision);else setNative(nativeGpu,in);}
    @Override void fill(int color,int xy,int wh){if(compatibility!=null)compatibility.fill(color,xy,wh);else fillNative(nativeGpu,color,xy,wh);}
    @Override void copy(int from,int to,int wh,int mask){if(compatibility!=null)compatibility.copy(from,to,wh,mask);else copyNative(nativeGpu,from,to,wh,mask);}
    @Override void upload(int[] words,int start,int x,int y,int width,int total,int mask){if(compatibility!=null)compatibility.upload(words,start,x,y,width,total,mask);else uploadNative(nativeGpu,words,start,x,y,width,total,mask);}
    @Override boolean present(Surface surface,int[] state){return presentResolved(surface,state,output.nativeHandle());}
    @Override boolean presentResolved(Surface surface,int[] state,long vulkan){if(compatibility!=null)return compatibility.presentResolved(surface,state,vulkan);return presentNative(nativeGpu,surface,state);}
    @Override Gpu.Frame capture(int[] state){if(compatibility!=null)return compatibility.capture(state);int[] pixels=captureNative(nativeGpu,state);int status=state[0],divider=(status&0x10000)!=0?7:switch((status>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};int width=Math.max(1,Math.min(1024,(state[4]-state[3])/divider))*scale;return new Gpu.Frame(width,pixels.length/width,pixels);}
    @Override void configureDisplay(float aspect,boolean stretch,boolean integerScale){this.aspect=aspect;this.stretch=stretch;this.integerScale=integerScale;output.configureDisplay(aspect,stretch,integerScale);if(compatibility!=null)compatibility.configureDisplay(aspect,stretch,integerScale);}
    @Override void configureWindow(float[] value){window=value.clone();output.configureWindow(value);if(compatibility!=null)compatibility.configureWindow(value);}
    @Override void setVsync(boolean enabled){vsync=enabled;if(compatibility!=null)compatibility.setVsync(enabled);}
    @Override void detachPresentation(){if(compatibility!=null)compatibility.detachPresentation();}
    @Override boolean presentBlack(Surface surface){return output.presentBlack(surface);}
    @Override void configureProfiling(boolean enabled){profiling=enabled;if(compatibility!=null)compatibility.configureProfiling(enabled);}
    @Override synchronized Map<String,Object> nativeTimings(){return compatibility!=null?compatibility.nativeTimings():Map.of("available",false,"backend","native Vulkan");}
    @Override synchronized String describe(){return compatibility!=null?compatibility.describe()+" (compatibility rendering)":nativeGpu==0?"":describeNative(nativeGpu);}
    @Override synchronized long[] gpuTimes(){if(compatibility!=null)return compatibility.gpuTimes();long[] values=new long[21];if(nativeGpu!=0)countsNative(nativeGpu,values);return values;}
    @Override synchronized String statistics(){if(compatibility!=null)return compatibility.statistics();if(nativeGpu==0)return "GPU closed";long[] values=new long[15];statsNative(nativeGpu,values);return "Native Vulkan GPU draws="+values[0]+" feedback="+values[1]+" readbacks="+values[2]+" passes="+values[3]+" nativeCopies="+values[5]+" uploadBytes="+values[8]+" scanouts="+values[9];}
    private synchronized void useCompatibility(){
        if(compatibility!=null)return;
        int[] ram=new int[1024*512];readNative(nativeGpu,ram);
        HardwareRasterizer replacement=new HardwareRasterizer(scale);
        try{replacement.setVram(ram);replacement.configureDisplay(aspect,stretch,integerScale);replacement.configureWindow(window);replacement.setVsync(vsync);replacement.configureProfiling(profiling);}
        catch(RuntimeException error){replacement.close();throw error;}
        try{destroyNative(nativeGpu);}finally{nativeGpu=0;compatibility=replacement;}
    }
    // Texture replacements retain the established HD replacement implementation.
    // Migration preserves canonical VRAM and needs no new user setting.
    @Override void textures(boolean enabled){if(enabled)useCompatibility();if(compatibility!=null)compatibility.textures(enabled);}
    @Override void replacement(TexturePacks.Image image,int x,int y,int w,int h,int mask){useCompatibility();compatibility.replacement(image,x,y,w,h,mask);}
    @Override public synchronized void close(){try{if(compatibility!=null)try{compatibility.close();}finally{compatibility=null;}}finally{if(nativeGpu!=0)try{destroyNative(nativeGpu);}finally{nativeGpu=0;}}}

    private static native long createNative(long output,int scale);
    private static native void destroyNative(long gpu);
    private static native void batchNative(long gpu,int[] words);
    private static native void readNative(long gpu,int[] pixels);
    private static native void setNative(long gpu,int[] pixels);
    private static native void fillNative(long gpu,int color,int xy,int wh);
    private static native void copyNative(long gpu,int from,int to,int wh,int mask);
    private static native void uploadNative(long gpu,int[] words,int start,int x,int y,int width,int total,int mask);
    private static native boolean presentNative(long gpu,Surface surface,int[] state);
    private static native int[] captureNative(long gpu,int[] state);
    private static native String describeNative(long gpu);
    private static native void statsNative(long gpu,long[] values);
    private static native void countsNative(long gpu,long[] values);
}
