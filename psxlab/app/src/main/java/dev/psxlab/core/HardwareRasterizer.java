package dev.psxlab.core;

import android.view.Surface;
import java.util.*;

/** Original GLES integer rasterizer; all calls belong to the GPU worker thread. */
public final class HardwareRasterizer implements AutoCloseable {
    private long handle;
    private boolean profiling;
    private final NativeTiming timing=new NativeTiming();
    void configureProfiling(boolean enabled){if(handle!=0&&profiling!=enabled){setProfilingNative(handle,enabled);profiling=enabled;if(enabled)timing.reset();}}
    java.util.Map<String,Object> nativeTimings(){return timing.sample(timingsNative(handle));}
    private static native void setProfilingNative(long handle,boolean enabled);
    private static native long[] timingsNative(long handle);

    private final int scale;
    public HardwareRasterizer(){this(1);}
    public HardwareRasterizer(int scale){if(!NativeRasterizer.AVAILABLE)throw new IllegalStateException("Native GPU unavailable");if(scale<1||scale>5)throw new IllegalArgumentException("Internal scale must be 1–5");this.scale=scale;handle=create(scale);}
    void drawBatch(int[] words){batchNative(handle,words);}
    void readVram(int[] out){readNative(handle,out);}
    void setVram(int[] in){setNative(handle,in);}
    void setDisplayVram(int[] in,int[] state,long revision){setDisplayNative(handle,in,state,revision);}
    void fill(int color,int xy,int wh){fillNative(handle,color,xy,wh);}
    void copy(int from,int to,int wh,int mask){copyNative(handle,from,to,wh,mask);}
    void upload(int[] words,int start,int x,int y,int width,int total,int mask){uploadNative(handle,words,start,x,y,width,total,mask);}
    void textures(boolean enabled){texturesNative(handle,enabled);}
    void replacement(TexturePacks.Image image,int x,int y,int w,int h,int mask){replacementNative(handle,image.argb(),image.width(),image.height(),x,y,w,h,mask);}
    private static native void texturesNative(long handle,boolean enabled);
    private static native void replacementNative(long handle,int[] pixels,int iw,int ih,int x,int y,int w,int h,int mask);
    boolean present(Surface surface,int[] state){return presentNative(handle,surface,state);}
    boolean presentResolved(Surface surface,int[] state,long vulkan){return presentResolvedNative(handle,surface,state,vulkan);}
    Gpu.Frame capture(int[] state){int status=state[0],divider=(status&0x10000)!=0?7:switch((status>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};int w=Math.max(1,Math.min(1024,(state[4]-state[3])/divider))*scale,h=Math.max(1,Math.min(512,(state[6]-state[5])*((status&0x480000)==0x480000?2:1)))*scale;return new Gpu.Frame(w,h,captureNative(handle,state));}
    String statistics(){long[] s=new long[12];statsNative(handle,s);return "GPU draws="+s[0]+" feedback="+s[1]+" readbacks="+s[2]+" passes="+s[3]+" presented="+s[4]+" nativeCopies="+s[5]+" sharedFrames="+s[6]+" asyncFrames="+s[7]+" uploadBytes="+s[8]+" scanouts="+s[9]+" reused="+s[10]+" nativeMovies="+s[11];}
    private void draw(int[] packet,int[] state){int[] commands=new int[1+packet.length+7];commands[0]=packet.length;System.arraycopy(packet,0,commands,1,packet.length);System.arraycopy(state,0,commands,packet.length+1,7);drawBatch(commands);}
    void setVsync(boolean enabled){setVsyncNative(handle,enabled);}
    void detachPresentation(){detachPresentationNative(handle);}
    private static native void detachPresentationNative(long handle);
    boolean presentBlack(Surface surface){return presentBlackNative(handle,surface);}
    private static native void setVsyncNative(long handle,boolean enabled);
    private static native boolean presentBlackNative(long handle,Surface surface);
    void configureDisplay(float aspect,boolean stretch,boolean integerScale){configureDisplayNative(handle,aspect,stretch,integerScale);}
    private static native void configureDisplayNative(long handle,float aspect,boolean stretch,boolean integerScale);
    @Override public void close(){if(handle!=0){destroy(handle);handle=0;}}
    private static native long create(int scale);
    private static native void destroy(long handle);
    private static native void verifyComputePathNative(long handle);
    private static native void batchNative(long handle,int[] commands);
    private static native void readNative(long handle,int[] vram);
    private static native void setNative(long handle,int[] vram);
    private static native void setDisplayNative(long handle,int[] vram,int[] state,long revision);
    private static native void fillNative(long handle,int color,int xy,int wh);
    private static native void copyNative(long handle,int from,int to,int wh,int mask);
    private static native void uploadNative(long handle,int[] words,int start,int x,int y,int width,int total,int mask);
    private static native void countNative(long handle,boolean enabled);
    private static native long pixelsNative(long handle);
    private static native boolean presentNative(long handle,Surface surface,int[] state);
    private static native boolean presentResolvedNative(long handle,Surface surface,int[] state,long vulkan);
    private static native int[] captureNative(long handle,int[] state);
    private static native void statsNative(long handle,long[] out);

    public static String verify(){return verifyRaster(1);}
    public static String verifySoftwareDisplayUploads(){
        int[] vram=new int[1024*512];Random random=new Random(0x5343414e);
        for(int i=0;i<vram.length;i++)vram[i]=random.nextInt(65536);
        int cases=0;long revision=1;
        try(HardwareRasterizer reference=new HardwareRasterizer(1);HardwareRasterizer partial=new HardwareRasterizer(1)){
            for(int mode:new int[]{0,0x20000,0x60000,0x200000,0x220000,0x480000,0x680000,0x800000})
                for(int y:new int[]{0,250,510,511})for(int height:new int[]{1,17,240,512}){
                    int[] state={mode,1007,y,0,3200,0,height,1};
                    reference.setVram(vram);partial.setDisplayVram(vram,state,revision);
                    if(!Arrays.equals(reference.capture(state).pixels(),partial.capture(state).pixels()))throw new AssertionError("Partial display upload differs: mode="+Integer.toHexString(mode)+" y="+y+" height="+height);
                    // Same revision is reusable; a later visible write must upload again.
                    partial.setDisplayVram(vram,state,revision);vram[y*1024+1007]^=0x7fff;revision++;
                    partial.setDisplayVram(vram,state,revision);reference.setVram(vram);
                    if(!Arrays.equals(reference.capture(state).pixels(),partial.capture(state).pixels()))throw new AssertionError("Revised display upload stale");cases++;
                }
        }
        return "Software display uploads: "+cases+" 15/24-bit, interlaced, disabled and wrapped-row cases match full VRAM uploads.\n";
    }
    private static String verifyRaster(int scale){
        Random random=new Random(0x505358);int[] expected=new int[524288];for(int i=0;i<expected.length;i++)expected[i]=random.nextInt(65536);int[] actual=expected.clone();
        SoftwareRasterizer reference=new SoftwareRasterizer(expected,false);
        int[] ops={0x20,0x22,0x24,0x25,0x26,0x28,0x2c,0x30,0x34,0x38,0x3c,0x40,0x42,0x50,0x52,0x60,0x62,0x64,0x65,0x66,0x68,0x6c,0x70,0x74,0x78,0x7c};
        try(HardwareRasterizer gpu=new HardwareRasterizer(scale)){
            gpu.setVram(actual);if(scale==1)countNative(gpu.handle,true);int fixtures=0;
            for(int depth=0;depth<3;depth++)for(int mode=0;mode<4;mode++)for(int op:ops){
                int[] state={0,(depth<<7)|(mode<<5)|512,random.nextInt(1<<20),0,127|(127<<10),0,fixtures%3};
                int[] packet=new int[12];int at=1;packet[0]=(op<<24)|random.nextInt(1<<24);int[] xy={8|(12<<16),100|(18<<16),20|(104<<16),110|(112<<16)};
                if(op<0x40){int n=(op&8)!=0?4:3;for(int i=0;i<n;i++){if(i>0&&(op&16)!=0)packet[at++]=random.nextInt(1<<24);packet[at++]=xy[i];if((op&4)!=0)packet[at++]=random.nextInt();}}
                else if(op<0x60){packet[at++]=xy[0];if((op&16)!=0)packet[at++]=random.nextInt(1<<24);packet[at++]=xy[3];}
                else{packet[at++]=xy[0];if((op&4)!=0)packet[at++]=random.nextInt();if((op&24)==0)packet[at++]=64|(48<<16);}
                packet=Arrays.copyOf(packet,at);reference.draw(packet,state);gpu.draw(packet,state);gpu.readVram(actual);
                if(!Arrays.equals(expected,actual)||(scale==1&&reference.pixels!=pixelsNative(gpu.handle))){int mismatch=-1;for(int i=0;i<actual.length;i++)if(expected[i]!=actual[i]){mismatch=i;break;}throw new AssertionError("Hardware raster mismatch op="+Integer.toHexString(op)+" depth="+depth+" blend="+mode+" at="+mismatch+" expected="+(mismatch<0?0:expected[mismatch])+" actual="+(mismatch<0?0:actual[mismatch])+" pixels="+reference.pixels+"/"+pixelsNative(gpu.handle));}fixtures++;
            }
            // Exercise batched command ordering and cross-command texture hazards.
            for(int batch=0;batch<12;batch++){
                int[] words=new int[512];int at=0;
                for(int i=0;i<16;i++){int op=(i&1)==0?0x64:0x62;int[] p=(op&4)!=0?new int[]{op<<24|0x808080,8+i*5|((12+i*3)<<16),random.nextInt(),20|(17<<16)}:new int[]{op<<24|random.nextInt(1<<24),8+i*5|((12+i*3)<<16),20|(17<<16)};int[] s={0,(batch%3<<7)|(batch%4<<5)|512,random.nextInt(1<<20),0,127|(127<<10),0,i%3};reference.draw(p,s);words[at++]=p.length;System.arraycopy(p,0,words,at,p.length);at+=p.length;System.arraycopy(s,0,words,at,7);at+=7;}
                gpu.drawBatch(Arrays.copyOf(words,at));gpu.readVram(actual);if(!Arrays.equals(expected,actual)||(scale==1&&reference.pixels!=pixelsNative(gpu.handle)))throw new AssertionError("Hardware batched raster differs at batch "+batch);
            }
            for(int fixture=0;fixture<96;fixture++){
                int op=ops[fixture%ops.length],offset=(random.nextInt(33)-16)&2047;offset|=((random.nextInt(33)-16)&2047)<<11;
                int[] state={0,random.nextInt(1<<14),random.nextInt(1<<20),8|(7<<10),255|(127<<10),offset,fixture%4};
                int[] p=new int[12];p[0]=op<<24|random.nextInt(1<<24);int at=1,n=op<0x40?((op&8)!=0?4:3):op<0x60?2:1;
                for(int i=0;i<n;i++){if(i>0&&(op&16)!=0)p[at++]=random.nextInt(1<<24);p[at++]=((random.nextInt(384)-64)&65535)|((random.nextInt(256)-64)<<16);if(op<0x40&&(op&4)!=0 || op>=0x60&&(op&4)!=0)p[at++]=random.nextInt();}
                if(op>=0x60&&(op&24)==0)p[at++]=32|(24<<16);p=Arrays.copyOf(p,at);
                reference.draw(p,state);gpu.draw(p,state);gpu.readVram(actual);if(!Arrays.equals(expected,actual)||(scale==1&&reference.pixels!=pixelsNative(gpu.handle)))throw new AssertionError("Hardware clipped/offset raster differs fixture="+fixture+" op="+Integer.toHexString(op));
            }
            int[] words=new int[512];for(int i=0;i<words.length;i++)words[i]=random.nextInt();gpu.upload(words,3,1000,510,37,1050,1);
            for(int i=0;i<1024;i++){int index=3+i,address=((510+index/37)&511)*1024+((1000+index%37)&1023);expected[address]=((words[i/2]>>>((i&1)*16))&65535)|32768;}
            gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("Hardware wrapped upload differs");
            int source=1000|(510<<16),dest=1004|(510<<16),wh=32|(4<<16);gpu.copy(source,dest,wh,0);
            for(int y=0;y<4;y++)for(int x=0;x<32;x++)expected[((510+y)&511)*1024+((1004+x)&1023)]=expected[((510+y)&511)*1024+((1000+x)&1023)];
            gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("Hardware overlapping copy differs");
            for(int mask=0;mask<4;mask++){
                int sx=32,sy=500,dx=32,dy=506,w=40,h=20;gpu.copy(sx|(sy<<16),dx|(dy<<16),w|(h<<16),mask);
                for(int y=0;y<h;y++)for(int x=0;x<w;x++){int address=((dy+y)&511)*1024+((dx+x)&1023);if((mask&2)==0||(expected[address]&32768)==0)expected[address]=expected[((sy+y)&511)*1024+((sx+x)&1023)]|((mask&1)<<15);}
                gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("Hardware vertical overlapping copy differs mask="+mask);
            }
            gpu.copy(0,512|(256<<16),32|(8<<16),1);for(int y=0;y<8;y++)for(int x=0;x<32;x++)expected[(256+y)*1024+512+x]=expected[y*1024+x]|32768;
            gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("Hardware parallel copy differs");
            gpu.fill(0x0200ff00,1009|(510<<16),31|(4<<16));for(int y=0;y<4;y++)for(int x=0;x<32;x++)expected[((510+y)&511)*1024+((1008+x)&1023)]=31<<5;
            gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("Hardware fill differs");
            int scanouts=0;
            for(int status:new int[]{0,0x20000,0x40000,0x60000,0x10000,0x200000,0x220000,0x480000,0x680000,0x800000}){
                int[] state={status,1007,510,17,179,3,8,0};int[] image=captureNative(gpu.handle,state);
                int divider=(status&0x10000)!=0?7:switch((status>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};int w=(179-17)/divider,h=5*((status&0x480000)==0x480000?2:1);
                if(image.length!=w*h*scale*scale)throw new AssertionError("Texture scanout dimensions differ");
                for(int y=0;y<h;y++)for(int x=0;x<w;x++){
                    int color=0xff000000,row=((510+y)&511)*1024;
                    if((status&0x800000)==0){if((status&0x200000)==0){int p=expected[row+((1007+x)&1023)],r=p&31,g=(p>>>5)&31,b=(p>>>10)&31;color|=((r<<3|r>>>2)<<16)|((g<<3|g>>>2)<<8)|(b<<3|b>>>2);}
                        else{int bx=1007*2+x*3,r=(expected[row+((bx>>>1)&1023)]>>>((bx&1)*8))&255;bx++;int g=(expected[row+((bx>>>1)&1023)]>>>((bx&1)*8))&255;bx++;int b=(expected[row+((bx>>>1)&1023)]>>>((bx&1)*8))&255;color|=r<<16|g<<8|b;}}
                    if(image[(y*scale)*(w*scale)+x*scale]!=color)throw new AssertionError("Texture scanout pixel differs status="+Integer.toHexString(status)+" x="+x+" y="+y);
                }scanouts++;
            }
            return "Hardware GLES "+scale+"x: "+fixtures+" raster fixtures, 96 clipped/offset draws, 192 interleaved batched draws, wrapped uploads/fills, overlapping and parallel copies match Java VRAM"+(scale==1?"/pixel counts":"")+"; "+scanouts+" texture scanout modes match 15/24-bit reference pixels.\n";
        }
    }
    public static String verifyEnhanced(java.util.function.BiConsumer<String,Gpu.Frame> images){
        String canonical=verifyRaster(5);
        int[] expected=new int[1024*512],actual=new int[expected.length];
        int[] state={0,0,0,0,127|(127<<10),0,0},display={0,0,0,0,960,0,96,0};
        SoftwareRasterizer reference=new SoftwareRasterizer(expected,false);
        try(HardwareRasterizer gpu=new HardwareRasterizer(5);HardwareRasterizer compute=new HardwareRasterizer(5)){
            verifyComputePathNative(compute.handle);
            gpu.setVram(expected);
            int[] triangle={0x200000ff,8|(9<<16),81|(23<<16),17|(83<<16)};
            reference.draw(triangle,state);gpu.draw(triangle,state);gpu.readVram(actual);
            if(!Arrays.equals(expected,actual))throw new AssertionError("HD mode changed canonical VRAM");
            Gpu.Frame enhanced=gpu.capture(display);
            if(enhanced.width()!=480||enhanced.height()!=480||enhanced.pixels().length!=480*480)throw new AssertionError("HD scanout dimensions differ");
            int mixed=0,different=0;int[] nativePixels=new int[96*96];
            for(int y=0;y<96;y++)for(int x=0;x<96;x++){
                int p=expected[y*1024+x],r=p&31,g=(p>>>5)&31,b=(p>>>10)&31,color=0xff000000|((r<<3|r>>>2)<<16)|((g<<3|g>>>2)<<8)|(b<<3|b>>>2);nativePixels[y*96+x]=color;
                int first=enhanced.pixels()[(y*5)*480+x*5];boolean varied=false;
                for(int sy=0;sy<5;sy++)for(int sx=0;sx<5;sx++){int value=enhanced.pixels()[(y*5+sy)*480+x*5+sx];if(value!=color)different++;if(value!=first)varied=true;}
                if(varied)mixed++;
            }
            if(mixed<50||different<100)throw new AssertionError("HD drawing is only enlarging native pixels: mixed="+mixed+" differences="+different);
            if(images!=null){images.accept("native-1x",new Gpu.Frame(96,96,nativePixels));images.accept("hd-5x",enhanced);}
            // Independent native-reference checks for shading, blend modes and texture depths.
            for(int depth=0;depth<3;depth++)for(int blend=0;blend<4;blend++){
                Random random=new Random(350+depth*4+blend);for(int i=0;i<expected.length;i++)expected[i]=random.nextInt(65536);
                gpu.setVram(expected);compute.setVram(expected);int[] e={0,(depth<<7)|(blend<<5)|512,0,0,127|(127<<10),0,blend%3};
                int[] packet={0x360080ff,7|(11<<16),0x2020|(300<<22),0x00ff80,85|(19<<16),0x2070,0xff8000,21|(87<<16),0x7020};
                reference.draw(packet,e);gpu.draw(packet,e);gpu.readVram(actual);if(!Arrays.equals(expected,actual))throw new AssertionError("HD canonical textured draw differs at depth="+depth+" blend="+blend);
                compute.draw(packet,e);if(!Arrays.equals(gpu.capture(display).pixels(),compute.capture(display).pixels()))throw new AssertionError("HD graphics/compute pixels differ at depth="+depth+" blend="+blend);
            }
            Random coverage=new Random(0x48445053);
            for(int fixture=0;fixture<24;fixture++){
                Arrays.fill(expected,0);gpu.setVram(expected);compute.setVram(expected);
                int[] p={0x300000ff,coverage.nextInt(96)|(coverage.nextInt(96)<<16),0x0000ff00,coverage.nextInt(96)|(coverage.nextInt(96)<<16),0x00ff0000,coverage.nextInt(96)|(coverage.nextInt(96)<<16)};
                gpu.draw(p,state);compute.draw(p,state);
                int[] graphics=gpu.capture(display).pixels(),checked=compute.capture(display).pixels();if(!Arrays.equals(graphics,checked)){int at=Arrays.mismatch(graphics,checked),missed=0,extra=0;for(int i=0;i<graphics.length;i++)if(graphics[i]!=checked[i]){if(graphics[i]==0xff000000)missed++;else extra++;}throw new AssertionError("HD triangle coverage differs at fixture="+fixture+" packet="+Arrays.toString(p)+" at="+(at%480)+","+(at/480)+" pixels="+Integer.toHexString(graphics[at])+"/"+Integer.toHexString(checked[at])+" missed="+missed+" extra="+extra);}
            }
            for(int scale=2;scale<=4;scale++)try(HardwareRasterizer scaled=new HardwareRasterizer(scale);HardwareRasterizer checked=new HardwareRasterizer(scale)){
                verifyComputePathNative(checked.handle);Arrays.fill(expected,0);scaled.setVram(expected);checked.setVram(expected);
                for(int i=0;i<8;i++){
                    int[] p={0x300000ff,coverage.nextInt(96)|(coverage.nextInt(96)<<16),0x0000ff00,coverage.nextInt(96)|(coverage.nextInt(96)<<16),0x00ff0000,coverage.nextInt(96)|(coverage.nextInt(96)<<16)};
                    scaled.draw(p,state);checked.draw(p,state);
                }
                Gpu.Frame frame=scaled.capture(display);if(frame.width()!=96*scale||frame.height()!=96*scale||!Arrays.equals(frame.pixels(),checked.capture(display).pixels()))throw new AssertionError("HD geometry differs at scale="+scale);
            }
            return canonical+"2x-4x dimensions/coverage match compute; HD graphics and compute match in 36 textured/shaded/blended/coverage fixtures. HD 5×: "+mixed+" native-sized edge cells contain subpixel coverage; "+different+" samples differ from nearest-neighbour enlargement. Canonical VRAM matches the independent native renderer across textured/shaded/blended fixtures.\n";
        }
    }
}
