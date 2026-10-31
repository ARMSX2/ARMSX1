package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class ScanoutCacheTest {
    private static int[] display(){return new int[]{0,0,0,0,3200,0,240,1};}
    @Test public void blankFieldAndFilterChangesCanReuseTheSameConvertedPixels(){
        ScanoutCache cache=new ScanoutCache();int[] state=display();assertFalse(cache.matches(0,state));
        cache.update(0,state,new Gpu.Frame(1,1,new int[]{0xff123456}));
        state[0]^=0x80002000;state[7]=0;assertTrue(cache.matches(0,state));assertEquals(1,cache.token());
    }
    @Test public void vramAndEveryRelevantDisplayChangeInvalidateConvertedPixels(){
        ScanoutCache cache=new ScanoutCache();int[] state=display();Gpu.Frame image=new Gpu.Frame(1,1,new int[1]);cache.update(7,state,image);
        assertFalse(cache.matches(8,state));
        for(int bit=16;bit<=23;bit++){int[] changed=state.clone();changed[0]^=1<<bit;assertFalse(cache.matches(7,changed));}
        for(int i=1;i<7;i++){int[] changed=state.clone();changed[i]++;assertFalse(cache.matches(7,changed));}
        assertTrue(cache.matches(7,state));assertSame(image,cache.frame());
    }
    @Test public void captureKeyDoesNotAliasTheCallersArrayAndTokensAdvance(){
        ScanoutCache cache=new ScanoutCache();int[] state=display();cache.update(2,state,new Gpu.Frame(1,1,new int[1]));
        state[1]=9;assertFalse(cache.matches(2,state));assertTrue(cache.matches(2,display()));
        cache.update(2,state,new Gpu.Frame(1,1,new int[1]));assertEquals(2,cache.token());assertTrue(cache.matches(2,state));
    }
}
