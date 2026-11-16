package dev.psxlab;

import dev.psxlab.core.*;
import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import static org.junit.Assert.*;

public class CardStorageTest {
    private static final class Recorder implements CardStorage.Events {
        final List<String> events=Collections.synchronizedList(new ArrayList<>());
        public void card(int slot,int state,String detail){events.add((slot+1)+":"+(state==CardStorage.SAVING?"saving":state==CardStorage.SAVED?"saved":"failed"));}
        boolean await(String event,long millis)throws InterruptedException{long end=System.currentTimeMillis()+millis;while(System.currentTimeMillis()<end){if(events.contains(event))return true;Thread.sleep(20);}return events.contains(event);}
    }
    @Test public void oneSaveIsAnnouncedOnceAndOnlyAfterItIsOnDisk()throws Exception{
        File folder=Files.createTempDirectory("cards").toFile();Recorder recorder=new Recorder();
        try(Gpu gpu=new Gpu(false);CardStorage storage=new CardStorage(folder,new Bus(gpu),recorder,300)){
            // A save is many frame writes in a burst: one "saving", no "saved" while it continues.
            for(int i=0;i<10;i++){storage.mark(0);Thread.sleep(50);}
            assertEquals(List.of("1:saving"),List.copyOf(recorder.events));
            assertTrue(recorder.await("1:saved",3000));
            assertEquals(List.of("1:saving","1:saved"),List.copyOf(recorder.events));
            assertTrue(new File(folder,"card1.mcd").isFile());
            // A later save is a new one.
            storage.mark(1);assertTrue(recorder.await("2:saved",3000));
            assertEquals(List.of("1:saving","1:saved","2:saving","2:saved"),List.copyOf(recorder.events));
        }
    }
    @Test public void failureIsReportedOnceAndARecoveredRetryStillEndsSaved()throws Exception{
        File folder=Files.createTempDirectory("cards-fail").toFile();Recorder recorder=new Recorder();
        try(Gpu gpu=new Gpu(false);CardStorage storage=new CardStorage(folder,new Bus(gpu),recorder,200)){
            // Replace the card folder with a file so every write fails.
            for(File file:folder.listFiles())assertTrue(file.delete());assertTrue(folder.delete());assertTrue(folder.createNewFile());
            storage.mark(0);assertTrue(recorder.await("1:failed",3000));Thread.sleep(700);
            assertEquals("retries must not repeat the failure",List.of("1:saving","1:failed"),List.copyOf(recorder.events));
            assertFalse(storage.error().isEmpty());
            assertTrue(folder.delete());assertTrue(folder.mkdirs());
            assertTrue(recorder.await("1:saved",3000));assertEquals(List.of("1:saving","1:failed","1:saved"),List.copyOf(recorder.events));
        }
    }
    @Test public void closingDuringASaveFinishesIt()throws Exception{
        File folder=Files.createTempDirectory("cards-close").toFile();Recorder recorder=new Recorder();
        try(Gpu gpu=new Gpu(false)){
            CardStorage storage=new CardStorage(folder,new Bus(gpu),recorder,60_000);
            storage.mark(0);storage.close();
            assertEquals(List.of("1:saving","1:saved"),List.copyOf(recorder.events));
        }
    }
}
