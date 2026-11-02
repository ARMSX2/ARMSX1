package dev.psxlab.core;
import org.junit.Test;
import java.util.*;
import static org.junit.Assert.*;
public class HardwareRegisterReportTest {
    @Test public void reportsPollingAndWritesWithoutCallingThemMeasuredStalls(){
        var report=HardwareRegisterReport.format(new long[]{1,200,3,2,1,0x1f801814,200,0,4,0x80012340,0x1f8010a8,0,3,4<<8,0x80045670});
        assertEquals(200L,report.get("reads"));var rows=(List<?>)report.get("hot_registers");var gpu=(Map<?,?>)rows.get(0);assertEquals("GPU GP1/GPUSTAT",gpu.get("register"));assertEquals("80012340",gpu.get("last_pc"));assertEquals("DMA2 CHCR",((Map<?,?>)rows.get(1)).get("register"));
        assertTrue(report.get("interpretation").toString().contains("do not measure"));
    }
    @Test public void absentDisabledAndMalformedSnapshotsAreUnavailable(){for(long[] value:new long[][]{null,new long[0],new long[]{0,0,0,0,0},new long[6]})assertEquals(false,HardwareRegisterReport.format(value).get("available"));}
}
