package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class AudioEventTraceTest {
    @Test public void wrappingKeepsAbsoluteClocksAndAccountsForDiscardedEvents(){
        AudioEventTrace t=new AudioEventTrace(2);t.record(100,200,300,400,500,"old");t.record(101,201,301,401,501,"middle");t.record(102,202,302,402,502,"last");
        String s=t.snapshot();assertTrue(s.contains("total_events=3 retained=2 discarded=1"));assertFalse(s.contains("\told\n"));assertTrue(s.contains("3\t102\t202\t302\t402\t502\tlast\n"));
    }
    @Test public void snapshotRemainsStableAndEventsCannotInjectRows(){
        AudioEventTrace t=new AudioEventTrace(1);t.record(1,2,3,4,5,"CD\tline\nnext\r");String s=t.snapshot();t.record(6,7,8,9,10,"later");
        assertTrue(s.endsWith("CD line next \n"));assertFalse(s.contains("later"));
    }
}
