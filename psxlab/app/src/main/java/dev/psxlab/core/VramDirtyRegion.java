package dev.psxlab.core;

/** VRAM the hardware renderer may have written since the CPU copy was last downloaded, as one bounding
 * rectangle. A VRAM-to-CPU transfer outside it reads the CPU copy without waiting for the GPU to finish. */
public final class VramDirtyRegion {
    private int x0,y0,x1=1023,y1=511; // A new renderer's VRAM is not known to match the CPU copy.
    public void all(){x0=0;y0=0;x1=1023;y1=511;}
    public void clean(){x0=1024;y0=512;x1=-1;y1=-1;}
    public boolean isClean(){return x1<x0||y1<y0;}
    /** Inclusive corners, already inside VRAM; an empty area (x1<x0 or y1<y0) changes nothing. */
    public void add(int ax0,int ay0,int ax1,int ay1){if(ax1<ax0||ay1<ay0)return;x0=Math.min(x0,ax0);y0=Math.min(y0,ay0);x1=Math.max(x1,ax1);y1=Math.max(y1,ay1);}
    /** A transfer-shaped area that may wrap at the right or bottom edge of VRAM. */
    public void addWrapped(int x,int y,int w,int h){
        if(w<=0||h<=0)return;
        boolean wrapsX=x+w>1024,wrapsY=y+h>512;
        add(wrapsX?0:x,wrapsY?0:y,wrapsX?1023:x+w-1,wrapsY?511:y+h-1);
    }
    /** The drawing area from GP0 E3/E4: primitives never write outside it. */
    public void addDrawingArea(int e3,int e4){add(e3&1023,(e3>>>10)&511,e4&1023,(e4>>>10)&511);}
    public boolean intersectsWrapped(int x,int y,int w,int h){
        if(isClean()||w<=0||h<=0)return false;
        // Split a wrapping transfer into at most four plain rectangles.
        int wa=Math.min(w,1024-x),ha=Math.min(h,512-y);
        return overlaps(x,y,wa,ha)||w>wa&&overlaps(0,y,w-wa,ha)||h>ha&&overlaps(x,0,wa,h-ha)||w>wa&&h>ha&&overlaps(0,0,w-wa,h-ha);
    }
    private boolean overlaps(int x,int y,int w,int h){return x<=x1&&x+w-1>=x0&&y<=y1&&y+h-1>=y0;}
    public String describe(){return isClean()?"none":x0+","+y0+"-"+x1+","+y1;}
}
