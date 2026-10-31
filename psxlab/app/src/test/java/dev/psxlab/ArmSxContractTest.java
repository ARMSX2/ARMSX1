package dev.psxlab;
import org.junit.Test;
import static org.junit.Assert.*;

public class ArmSxContractTest {
    @Test public void tallNtscBiosDisplayIsNotMistakenForPal(){
        assertEquals(60f,ArmSxBackend.videoRate(0),0);
        assertEquals(60f,ArmSxBackend.videoRate(1<<19),0);
        assertEquals(50f,ArmSxBackend.videoRate(1<<20),0);
        assertEquals(50f,ArmSxBackend.videoRate((1<<20)|(1<<19)),0);
    }
    @Test public void allSixteenPadButtonsHaveDistinctPs1BitsAndUnknownKeysAreIgnored(){
        int[] keys={109,106,107,108,19,22,20,21,104,105,102,103,100,97,96,99};
        int mask=0;for(int key:keys){int bit=ArmSxBackend.buttonBit(key);assertEquals(0,mask&(1<<bit));mask|=1<<bit;}
        assertEquals(65535,mask);assertEquals(-1,ArmSxBackend.buttonBit(0));
    }
}
