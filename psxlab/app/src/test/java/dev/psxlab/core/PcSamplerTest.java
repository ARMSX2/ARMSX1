package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class PcSamplerTest {
    @Test public void waitLoopDominatesOneWindow(){
        PcSampler s=new PcSampler();
        for(int i=0;i<300;i++){s.add(0x8007EBF0+(i%3)*4);s.add(PcSampler.EXCEPTION_VECTOR);}
        long hot=s.hotWindow();
        assertEquals(0x8007EBC0,(int)(hot>>>32));assertEquals(100,(int)hot);
        assertTrue(s.top(2),s.top(2).startsWith("80000080 50%"));
    }
    @Test public void normalPlayIsSpreadOut(){
        PcSampler s=new PcSampler();
        for(int i=0;i<300;i++)s.add(0x80010000+i*256);
        assertTrue((int)s.hotWindow()<10);
    }
    @Test public void tooFewSamplesAreNotJudged(){
        PcSampler s=new PcSampler();for(int i=0;i<10;i++)s.add(0x80020000);
        assertEquals(0,s.hotWindow());assertEquals("80020000 100%",s.top(3));
    }
    @Test public void clearStartsANewRow(){
        PcSampler s=new PcSampler();s.add(1);s.clear();assertEquals(0,s.samples());assertEquals("none",s.top(3));
    }
}
