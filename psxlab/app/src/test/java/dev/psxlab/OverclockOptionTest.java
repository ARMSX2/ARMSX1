package dev.psxlab;

import static org.junit.Assert.assertEquals;

import java.util.Map;
import org.junit.Test;

public class OverclockOptionTest {
    @Test public void missingOrUnreadableValuesRunTheRealConsoleClock(){
        assertEquals(100,ArmSxBackend.overclockPercent(Map.of()));
        assertEquals(100,ArmSxBackend.overclockPercent(Map.of("cpu.overclock","fast")));
        assertEquals(200,ArmSxBackend.overclockPercent(Map.of("cpu.overclock"," 200 ")));
    }
    @Test public void handEditedValuesStayInTheSupportedRange(){
        assertEquals(1000,ArmSxBackend.overclockPercent(Map.of("cpu.overclock","4000")));
        assertEquals(25,ArmSxBackend.overclockPercent(Map.of("cpu.overclock","0")));
    }
}
