package dev.psxlab.core;

import java.lang.reflect.Field;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.junit.Test;
import static org.junit.Assert.*;

public class GpuReadTransferTest {
    private static void send(Gpu gpu,int... words){for(int word:words)gpu.write(word);}
    @Test public void readHeaderCanOverlapDrawingButDataWaitsForCompletion()throws Exception{
        try(Gpu gpu=new Gpu(true)){
            send(gpu,0xa0000000,0,0x00010002,0x12345678);
            Field field=Gpu.class.getDeclaredField("worker");field.setAccessible(true);
            ThreadPoolExecutor worker=(ThreadPoolExecutor)field.get(gpu);
            CountDownLatch busy=new CountDownLatch(1),release=new CountDownLatch(1),reading=new CountDownLatch(1);
            worker.execute(()->{busy.countDown();try{release.await();}catch(InterruptedException e){Thread.currentThread().interrupt();}});
            assertTrue(busy.await(5,TimeUnit.SECONDS));
            FutureTask<Void> header=new FutureTask<>(()->{send(gpu,0xc0000000,0,0x00010002);return null;});
            FutureTask<Integer> read=new FutureTask<>(()->{reading.countDown();return gpu.read();});
            try{
                new Thread(header,"read-header-test").start();header.get(5,TimeUnit.SECONDS);
                new Thread(read,"read-data-test").start();assertTrue(reading.await(5,TimeUnit.SECONDS));
                try{read.get(100,TimeUnit.MILLISECONDS);fail("VRAM data must wait for queued GPU work");}catch(TimeoutException expected){}
            }finally{release.countDown();}
            assertEquals(0x12345678,(int)read.get(5,TimeUnit.SECONDS));
        }
    }
    @Test public void largeThreadedReadKeepsPixelsAndDoesNotFenceEveryWord()throws Exception{
        try(Gpu gpu=new Gpu(true)){
            send(gpu,0xa0000000,0,0x00040400);
            for(int i=0;i<2048;i++)gpu.write(i|((4095-i)<<16));
            send(gpu,0xc0000000,0,0x00040400);gpu.sync();
            Field field=Gpu.class.getDeclaredField("worker");field.setAccessible(true);
            ThreadPoolExecutor worker=(ThreadPoolExecutor)field.get(gpu);long before=worker.getCompletedTaskCount();
            for(int i=0;i<2048;i++)assertEquals(i|((4095-i)<<16),gpu.read());
            assertEquals(0,gpu.status()&0x08000000);gpu.sync();
            assertTrue("One transfer must not submit a GPU fence for every word",worker.getCompletedTaskCount()-before<8);
        }
    }
    @Test public void interveningUploadIsVisibleToRemainingReadWords(){
        try(Gpu gpu=new Gpu(true)){
            send(gpu,0xa0000000,0,0x00010004,0x00020001,0x00040003);
            send(gpu,0xc0000000,0,0x00010004);assertEquals(0x00020001,gpu.read());
            send(gpu,0xa0000000,2,0x00010002,0x00080007);assertEquals(0x00080007,gpu.read());
        }
    }
    @Test public void stateRestoreDuringReadResumesAtCorrectPixel()throws Exception{
        try(Gpu gpu=new Gpu(true)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);
            send(gpu,0xa0000000,0,0x00010004,0x00020001,0x00040003);
            send(gpu,0xc0000000,0,0x00010004);assertEquals(0x00020001,gpu.read());
            byte[] state=SaveState.capture(cpu,bus,gpu,"read-transfer");assertEquals(0x00040003,gpu.read());
            SaveState.restore(state,cpu,bus,gpu,"read-transfer");assertEquals(0x00040003,gpu.read());
        }
    }
}
