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
    public boolean widescreenEnabled(){return widescreen;}
    public void setWidescreen(boolean enabled){widescreen=enabled;if(nativeCore!=null)nativeCore.widescreen=enabled;else reference.widescreen=enabled;}
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
            a.setWidescreen((trial&1)!=0);b.setWidescreen((trial&1)!=0);
            for(int i=0;i<32;i++){int v=random.nextInt();a.writeData(i,v);b.writeData(i,v);v=random.nextInt();a.writeControl(i,v);b.writeControl(i,v);}
            for(int op:commands){int instruction=op|(random.nextInt()&0x001fffc0);a.execute(instruction);b.execute(instruction);count++;
                for(int i=0;i<32;i++)if(a.readData(i)!=b.readData(i) || a.readControl(i)!=b.readControl(i))throw new AssertionError("GTE mismatch trial="+trial+" command="+Integer.toHexString(instruction)+" register="+i);
            }
        }
        return "Native GTE: "+count+" randomized command/state comparisons match the original Java engine.\n";
    }
}
