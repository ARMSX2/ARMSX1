package dev.psxlab.core;
/** Our GTE engine, with native arithmetic and a portable reference. */
public final class Gte {
    private final NativeGte nativeCore;
    private final ReferenceGte reference;
    public Gte(){this(true);}
    public Gte(boolean nativeExecution){nativeCore=nativeExecution && NativeCore.AVAILABLE?new NativeGte():null;reference=nativeCore==null?new ReferenceGte():null;reset();}
    public boolean nativeExecution(){return nativeCore!=null;}
    int[] nativeState(){if(nativeCore==null)throw new IllegalStateException("Native CPU requires native geometry");return nativeCore.rawState();}
    private boolean widescreen;
    private int projectionScale=65536;
    public boolean widescreenEnabled(){return widescreen;}
    public int projectionScale(){return projectionScale;}
    /** Q16 keeps 16:9 bit-exact with the former 3/4 projection, without per-vertex division. */
    public static int projectionScale(boolean enabled,float aspect){return enabled&&Float.isFinite(aspect)&&aspect>0?Math.round((4f/3f)/Math.max(0.1f,Math.min(10f,aspect))*65536):65536;}
    public void setWidescreen(boolean enabled){setWidescreen(enabled,16f/9f);}
    public void setWidescreen(boolean enabled,float aspect){widescreen=enabled;projectionScale=projectionScale(enabled,aspect);if(nativeCore!=null)nativeCore.projectionScale=projectionScale;else reference.projectionScale=projectionScale;}
    public void reset(){if(nativeCore!=null)nativeCore.reset();else reference.reset();}
    public int readData(int r){return nativeCore!=null?nativeCore.readData(r):reference.readData(r);}
    public int readControl(int r){return nativeCore!=null?nativeCore.readControl(r):reference.readControl(r);}
    public void writeData(int r,int v){if(nativeCore!=null)nativeCore.writeData(r,v);else reference.writeData(r,v);}
    public void writeControl(int r,int v){if(nativeCore!=null)nativeCore.writeControl(r,v);else reference.writeControl(r,v);}
    public void execute(int instruction){if(nativeCore!=null)nativeCore.execute(instruction);else reference.execute(instruction);}
    public static String verifyNative(){
        if(!NativeCore.AVAILABLE)throw new AssertionError("Native GTE unavailable");
        Gte a=new Gte(false),b=new Gte(true);java.util.Random random=new java.util.Random(0x475445);
        int[] commands={1,0x30,6,0x2d,0x2e,0x12,0x28,0xc,0x3d,0x3e,0x10,0x11,0x29,0x2a,0x1e,0x20,0x1b,0x3f,0x13,0x16,0x1c,0x14};
        int count=0;
        for(int trial=0;trial<1200;trial++){
            float aspect=new float[]{4f/3f,3f/2f,16f/10f,16f/9f,2f,20f/9f,64f/27f}[trial%7];a.setWidescreen((trial&1)!=0,aspect);b.setWidescreen((trial&1)!=0,aspect);
            for(int i=0;i<32;i++){int v=random.nextInt();a.writeData(i,v);b.writeData(i,v);v=random.nextInt();a.writeControl(i,v);b.writeControl(i,v);}
            for(int op:commands){int instruction=op|(random.nextInt()&0x001fffc0);a.execute(instruction);b.execute(instruction);count++;
                for(int i=0;i<32;i++)if(a.readData(i)!=b.readData(i) || a.readControl(i)!=b.readControl(i))throw new AssertionError("GTE mismatch trial="+trial+" command="+Integer.toHexString(instruction)+" register="+i);
            }
        }
        return "Native GTE: "+count+" randomized command/state comparisons match the original Java engine.\n";
    }
}
