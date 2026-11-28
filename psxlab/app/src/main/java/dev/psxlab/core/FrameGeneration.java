package dev.psxlab.core;

import java.util.Arrays;

/**
 * Frame generation schedule, decided once per emulated field on the emulation thread. The pictures
 * themselves come from psxlab/app/src/main/cpp/frame_generation.h.
 *
 * A PS1 game shows a new picture by moving the display start to the buffer it just finished
 * (double buffering), so a 30 fps game moves it every second field. The fields between two moves
 * are where in-between pictures go: after a new frame arrives the newest picture is held back by
 * up to one game frame and the gap since the previous frame is filled with interpolated ones.
 * On a display running at a whole multiple of the game rate (120 Hz for 60 fps) every field also
 * gets multiple-1 extra pictures. A game that never moves its display start is shown unchanged.
 *
 * The cadence comes from the last few gaps between moves; the shortest is used so that a varying
 * frame rate repeats a picture rather than running out of in-between pictures late.
 */
public final class FrameGeneration {
    /** Longest gap, in fields, still filled: 15 fps on a 60 Hz game. Longer gaps are pauses, not a frame rate. */
    public static final int MAX_CADENCE=4;
    private static final float[] NONE=new float[0];
    private static final long NO_KEY=Long.MIN_VALUE;

    /**
     * What one field shows. capture: this field's picture is a new game frame and goes into the
     * history; estimate: search motion against the previous one too; reset: forget older history first.
     * generated: in-between times (0 = previous frame, 1 = newest) shown in order; real: show the
     * newest picture after them.
     */
    public record Plan(boolean capture,boolean estimate,boolean reset,float[] generated,boolean real){
        /** Nothing frame generation does is needed: present the field as usual. */
        public boolean ordinary(){return !capture&&generated.length==0;}
        @Override public String toString(){return "Plan[capture="+capture+", estimate="+estimate+", reset="+reset+", generated="+Arrays.toString(generated)+", real="+real+"]";}
    }

    private long lastKey=NO_KEY;
    private int fieldsSinceMove;
    private final int[] gaps=new int[3];
    private int recordedGaps,nextGap;
    private int slot,total=1;
    private boolean pendingReset=true,haveFrame,interpolating;

    /** Start again, e.g. after a pause, fast-forward, a state load or the setting being switched on. */
    public void reset(){lastKey=NO_KEY;fieldsSinceMove=0;recordedGaps=0;nextGap=0;slot=0;total=1;pendingReset=true;haveFrame=false;interpolating=false;}

    /** Fields per game frame, from the gaps seen so far; 1 until two moves have been seen. */
    public int cadence(){
        int best=Integer.MAX_VALUE;
        for(int i=0;i<recordedGaps;i++)if(gaps[i]<=MAX_CADENCE)best=Math.min(best,gaps[i]);
        return best==Integer.MAX_VALUE?1:best;
    }

    /**
     * @param status GPUSTAT for this field: resolution, video mode, colour depth and blanking bits are part of the picture's identity
     * @param multiple display refreshes per emulated field (1, or 2..4 on a matching high-refresh display)
     */
    public Plan next(int status,int displayX,int displayY,int multiple){
        multiple=Math.max(1,Math.min(4,multiple));
        long mode=(status>>>16)&0xff;
        long key=(mode<<20)|((displayY&511L)<<10)|(displayX&1023L);
        boolean modeChanged=lastKey!=NO_KEY&&(lastKey>>>20)!=mode;
        if(modeChanged){recordedGaps=0;nextGap=0;haveFrame=false;pendingReset=true;}
        if(key!=lastKey){
            if(lastKey!=NO_KEY&&!modeChanged)record(fieldsSinceMove+1);
            lastKey=key;fieldsSinceMove=0;
            boolean previous=haveFrame;haveFrame=true;
            boolean reset=pendingReset;pendingReset=false;
            total=cadence()*multiple;slot=0;
            interpolating=previous&&!reset&&total>1;
            return schedule(true,reset,multiple);
        }
        fieldsSinceMove++;
        if(!interpolating||slot>=total)return new Plan(false,false,false,NONE,true);
        return schedule(false,false,multiple);
    }

    private void record(int gap){gaps[nextGap]=gap;nextGap=(nextGap+1)%gaps.length;recordedGaps=Math.min(gaps.length,recordedGaps+1);}

    private Plan schedule(boolean capture,boolean reset,int multiple){
        if(!interpolating){slot=total;return new Plan(capture,false,reset,NONE,true);}
        float[] times=new float[multiple];
        int count=0;boolean real=false;
        for(int i=0;i<multiple&&!real;i++){int k=++slot;if(k<total)times[count++]=(float)k/total;else real=true;}
        return new Plan(capture,capture,reset,count==times.length?times:Arrays.copyOf(times,count),real);
    }

    /** Display refreshes per field: the whole multiple the display runs at, or 1 when it is not one. */
    public static int displayMultiple(float displayHz,boolean pal){
        float game=pal?50f:59.94f;
        if(!Float.isFinite(displayHz)||displayHz<=0)return 1;
        int multiple=Math.round(displayHz/game);
        return multiple>=2&&multiple<=4&&Math.abs(displayHz-game*multiple)<=1.5f?multiple:1;
    }
}
