package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class PresentationPolicyTest {
    @Test public void bfiRequiresMatchingActualRefreshAndNeverNeedsAShader(){
        assertEquals(1,PresentationPolicy.evaluate(1,2,120,60,true,true,false,false));assertEquals(1,PresentationPolicy.evaluate(1,2,100,50,true,true,false,false));
        for(float hz:new float[]{60,90,144})assertEquals(4,PresentationPolicy.evaluate(1,2,hz,60,true,true,false,false));
        assertEquals(4,PresentationPolicy.evaluate(1,3,120,60,true,true,true,true));assertEquals(1,PresentationPolicy.evaluate(1,4,240,60,true,true,true,false));
    }
    @Test public void pausedFastForwardMissingSurfaceAndUnknownRateSuspendEffects(){assertEquals(0,PresentationPolicy.evaluate(0,2,0,60,false,false,false,false));assertEquals(5,PresentationPolicy.evaluate(1,2,120,60,false,true,false,false));assertEquals(6,PresentationPolicy.evaluate(1,2,120,60,true,false,false,false));assertEquals(3,PresentationPolicy.evaluate(1,2,0,60,true,true,false,false));}
}
