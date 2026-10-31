package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class ReverbResamplerTest {
    @Test public void downsamplingPreservesDcAndRejectsNyquist(){
        ReverbResampler dc=new ReverbResampler(),high=new ReverbResampler();
        for(int i=0;i<200;i++){
            dc.input(12000,-6000);high.input((i&1)==0?12000:-12000,0);
            if(i>64){assertEquals(12000,dc.down(0),3);assertEquals(-6000,dc.down(1),3);assertEquals(0,high.down(0),3);}
            dc.advance();high.advance();
        }
    }
    @Test public void reconstructionPreservesBothChannelsWithoutSampleHold(){
        ReverbResampler r=new ReverbResampler();
        for(int i=0;i<200;i++){
            r.output((i&1)==0?12000:0,(i&1)==0?-6000:0);
            if(i>64){assertEquals(12000,r.up(0),3);assertEquals(-6000,r.up(1),3);}
            r.advance();
        }
    }
    @Test public void firstReverbSampleDoesNotAppearAsAnImmediateFullScaleStep(){
        ReverbResampler r=new ReverbResampler();r.output(32767,0);assertTrue(Math.abs(r.up(0))<5);assertEquals(0,r.up(1));
    }
}
