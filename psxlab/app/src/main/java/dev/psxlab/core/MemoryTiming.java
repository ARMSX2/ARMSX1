package dev.psxlab.core;

/** Read bus clocks from the documented PS1 DEVx/COM_DELAY registers. */
final class MemoryTiming {
    static int readWait(int device,int common,int bytes){
        int recovery=(device&0x100)!=0?(common&15)-1:0;
        int floating=(device&0x400)!=0?(common>>>8)&15:0;
        int minimum=(device&0x800)!=0?(common>>>12)&15:0;
        int sequential=recovery+floating;
        int first=sequential+(sequential<6?1:0);
        int access=((device>>>4)&15)+2;
        first=Math.max(first+access,minimum+6);
        sequential=Math.max(sequential+access,minimum+2);
        int transfers=(device&0x1000)!=0?(bytes+1)/2:bytes;
        // The CPU instruction already accounts for the first clock.
        return Math.max(0,first+(transfers-1)*sequential-1);
    }
    private MemoryTiming(){}
}
