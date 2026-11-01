package dev.psxlab.core;

/** Presentation-only TV-border crop, in native pixels before any HD scaling. */
final class DisplayCrop {
    static int[] apply(int[] state,int mode){
        if(mode<1||mode>2)return state;
        int status=state[0],divider=(status&0x10000)!=0?7:switch((status>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};
        int width=Math.max(1,Math.min(1024,(state[4]-state[3])/divider));
        int lines=state[6]-state[5],div=mode==2?16:32;
        if(width<=16||lines<=16)return state;
        int x=width/div,y=lines/div;
        // Packed RGB24 starts on a VRAM halfword. An even pixel inset keeps
        // the first red byte aligned without changing the movie's byte phase.
        boolean rgb24=(status&0x200000)!=0;if(rgb24)x&=~1;
        state[1]=(state[1]+(rgb24?x*3/2:x))&1023;
        state[2]=(state[2]+y*((status&0x480000)==0x480000?2:1))&511;
        state[3]+=x*divider;state[4]-=x*divider;
        state[5]+=y;state[6]-=y;
        return state;
    }
}
