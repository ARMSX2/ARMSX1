package dev.psxlab.core;

import org.junit.Test;

import static org.junit.Assert.*;

public class PerformanceStatisticsTest {
    private static PerformanceStatistics.Sample sample(long n){return new PerformanceStatistics.Sample(n*564480,n*2,n*3,n, n*6,n,new long[]{n,n,n*10,0,n*4,0,n*2},7,320,240,3);}
    @Test public void publishesRealDeltasAndKeepsUnsupportedReadingsUnavailable(){
        PerformanceStatistics p=new PerformanceStatistics();double[] out=new double[33];assertEquals(0,p.copy(out,1,8,20,100));
        p.enabled(true);long ticket=p.ticket();
        for(int n=0;n<=30;n++)p.completed(ticket,1_000_000_000L+n*16_666_667L,5_000_000,9_000_000,2_000_000,sample(n));
        assertEquals(33,p.copy(out,3,8,123,456));assertEquals(16.666667,out[0],0.000001);
        assertEquals(5,out[1],0);assertEquals(2,out[2],0);assertEquals(9,out[3],0);
        assertEquals(-1,out[5],0);assertEquals(564480,out[6],0);assertEquals(-1,out[7],0);
        assertEquals(2,out[8],0);assertEquals(3,out[9],0);assertEquals(1,out[10],0);assertEquals(7,out[13],0);
        assertEquals(6,out[15],0);assertEquals(1,out[16],0);assertEquals(18,out[17],0);assertEquals(10,out[18],0);
        assertEquals(3,out[25],0);assertEquals(-1,out[29],0);assertEquals(320,out[23],0);assertEquals(960,out[31],0);
    }
    @Test public void disabledAndStaleQueuedFramesCannotPublish(){
        PerformanceStatistics p=new PerformanceStatistics();p.enabled(true);long old=p.ticket();p.enabled(false);p.enabled(true);
        for(int n=0;n<=60;n++)p.completed(old,1_000_000_000L+n*16_666_667L,1,1,1,sample(n));
        assertEquals(0,p.copy(new double[33],0,8,0,0));
        assertNotEquals(old,p.ticket());p.reset();assertEquals(0,p.copy(new double[33],0,8,0,0));
    }
    @Test public void shortFrontendBuffersAreSafeAndEnableReassertionKeepsTheWindow(){
        PerformanceStatistics p=new PerformanceStatistics();p.enabled(true);long ticket=p.ticket();
        p.completed(ticket,1_000_000_000L,1,1,1,sample(0));p.enabled(true);assertEquals(ticket,p.ticket());
        p.completed(ticket,1_500_000_000L,1,1,1,sample(1));assertEquals(2,p.copy(new double[2],0,8,0,0));
        assertEquals(0,p.copy(null,0,8,0,0));
    }
}
