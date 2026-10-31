package dev.psxlab.core;

import java.util.ArrayList;

/** A tiny original MIPS program. Every rectangle goes through CPU stores to GP0. */
public final class Demo {
    public static Cpu load(Bus bus) {
        ArrayList<Integer> code = new ArrayList<>();
        code.add(0x3c081f80); code.add(0x35081810); // t0 = GP0
        rectangle(code,0x182438,0,0,320,240);
        rectangle(code,0xefbc66,32,32,256,32);
        rectangle(code,0xc97853,32,80,112,128);
        rectangle(code,0x76c4b2,160,80,128,64);
        rectangle(code,0xefbc66,160,160,128,48);
        int loop=0x80010000+code.size()*4;
        code.add(0x08000000|((loop>>>2)&0x3ffffff)); code.add(0);
        for(int i=0;i<code.size();i++) bus.write32(0x80010000+i*4,code.get(i));
        return new Cpu(bus);
    }
    private static void rectangle(ArrayList<Integer> c,int color,int x,int y,int w,int h) {
        word(c,0x02000000|color); word(c,y<<16|x); word(c,h<<16|w);
    }
    private static void word(ArrayList<Integer> c,int v) {
        c.add(0x3c090000|(v>>>16)); c.add(0x35290000|(v&65535)); c.add(0xad090000);
    }
}
