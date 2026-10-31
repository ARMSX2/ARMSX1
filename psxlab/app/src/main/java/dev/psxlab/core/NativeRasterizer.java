package dev.psxlab.core;

/** Native execution of our own software raster rules; Java remains the portable reference. */
public final class NativeRasterizer {
    static final boolean AVAILABLE;
    static{boolean available;try{System.loadLibrary("psxgpu");available=true;}catch(UnsatisfiedLinkError e){available=false;}AVAILABLE=available;}
    private NativeRasterizer(){}
    static native long draw(int[] vram,int[] packet,int[] state);
    static native long drawBatch(int[] vram,int[] commands);
    public static String verifyBatch(){
        if(!AVAILABLE)throw new AssertionError("Native renderer unavailable");
        int[] expected=new int[1024*512];java.util.Random random=new java.util.Random(717);
        for(int i=0;i<expected.length;i++)expected[i]=random.nextInt(65536);int[] actual=expected.clone();
        SoftwareRasterizer javaRenderer=new SoftwareRasterizer(expected,false),nativeRenderer=new SoftwareRasterizer(actual,true);
        int draws=0;for(int batch=0;batch<12;batch++){
            int[] commands=new int[512];int at=0;
            for(int i=0;i<16;i++){
                int op=(i&1)==0?0x64:0x62;int[] p=(op&4)!=0?new int[]{op<<24|0x808080,8+i*5|((12+i*3)<<16),random.nextInt(),20|(17<<16)}:new int[]{op<<24|random.nextInt(1<<24),8+i*5|((12+i*3)<<16),20|(17<<16)};
                int[] state={0,(batch%3<<7)|(batch%4<<5)|512,random.nextInt(1<<20),0,127|(127<<10),0,i%3};
                javaRenderer.draw(p,state);commands[at++]=p.length;System.arraycopy(p,0,commands,at,p.length);at+=p.length;System.arraycopy(state,0,commands,at,7);at+=7;draws++;
            }
            nativeRenderer.drawBatch(java.util.Arrays.copyOf(commands,at),16);
            if(!java.util.Arrays.equals(expected,actual) || javaRenderer.pixels!=nativeRenderer.pixels || javaRenderer.primitives!=nativeRenderer.primitives)throw new AssertionError("Native batched rendering differs at batch "+batch);
        }
        try{drawBatch(actual,new int[]{12,0x20000000});throw new AssertionError("Malformed batch accepted");}catch(IllegalArgumentException expectedError){}
        return "Native GPU batches: "+draws+" interleaved-state primitives match Java; malformed input rejected.\n";
    }
    public static String verify(){
        if(!AVAILABLE)throw new AssertionError("Original native software renderer unavailable");
        java.util.Random random=new java.util.Random(0x505358);int[] a=new int[1024*512];
        for(int i=0;i<a.length;i++)a[i]=random.nextInt(65536);int[] b=a.clone();
        SoftwareRasterizer reference=new SoftwareRasterizer(a,false),nativeRenderer=new SoftwareRasterizer(b,true);
        int[] ops={0x20,0x22,0x24,0x25,0x26,0x28,0x2c,0x30,0x34,0x38,0x3c,0x40,0x42,0x50,0x52,0x60,0x62,0x64,0x65,0x66,0x68,0x6c,0x70,0x74,0x78,0x7c};
        int fixtures=0;long javaTime=0,nativeTime=0;
        for(int depth=0;depth<3;depth++)for(int mode=0;mode<4;mode++)for(int op:ops){
            int[] e={0,(depth<<7)|(mode<<5)|512,random.nextInt(1<<20),0,127|(127<<10),0,fixtures%3};
            int[] p=new int[12];int at=1,color=random.nextInt(1<<24);p[0]=(op<<24)|color;
            int[] xy={8|(12<<16),100|(18<<16),20|(104<<16),110|(112<<16)};
            if(op<0x40){int n=(op&8)!=0?4:3;for(int i=0;i<n;i++){if(i>0 && (op&16)!=0)p[at++]=random.nextInt(1<<24);p[at++]=xy[i];if((op&4)!=0)p[at++]=random.nextInt();}}
            else if(op<0x60){p[at++]=xy[0];if((op&16)!=0)p[at++]=random.nextInt(1<<24);p[at++]=xy[3];}
            else{p[at++]=xy[0];if((op&4)!=0)p[at++]=random.nextInt();if((op&24)==0)p[at++]=64|(48<<16);}
            p=java.util.Arrays.copyOf(p,at);long time=System.nanoTime();reference.draw(p,e);javaTime+=System.nanoTime()-time;
            time=System.nanoTime();nativeRenderer.draw(p,e);nativeTime+=System.nanoTime()-time;
            if(!java.util.Arrays.equals(a,b) || reference.pixels!=nativeRenderer.pixels)throw new AssertionError("Native raster differs: op="+Integer.toHexString(op)+" depth="+depth+" blend="+mode);
            fixtures++;
        }
        return "Native raster: "+fixtures+" draw fixtures match Java VRAM/pixel counts; Java "+javaTime/1000000+" ms, native "+nativeTime/1000000+" ms.\n";
    }
}
