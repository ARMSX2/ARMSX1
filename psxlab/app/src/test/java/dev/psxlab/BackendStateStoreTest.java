package dev.psxlab;

import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import static org.junit.Assert.*;

public class BackendStateStoreTest {
    @Test public void autosaveDoesNotShareAnyNumberedSlot(){File root=new File("test-root");File auto=BackendStateStore.file(root,"game",-1);for(int slot=0;slot<10;slot++)assertNotEquals(auto,BackendStateStore.file(root,"game",slot));}
    @Test public void discAndPayloadSurviveReplacementTogether()throws Exception{
        File file=Files.createTempDirectory("psx-backend-state").resolve("game.psxlabstate").toFile();
        BackendStateStore.write(file,"disc-1.chd",new byte[]{1,2,3});
        BackendStateStore.write(file,"disc-2.chd",new byte[]{4,5});
        BackendStateStore.Stored stored=BackendStateStore.read(file);assertEquals("disc-2.chd",stored.disc());assertArrayEquals(new byte[]{4,5},stored.payload());
        byte[] before=Files.readAllBytes(file.toPath());
        try{BackendStateStore.write(file,"bad-disc",new byte[0]);fail();}catch(IOException expected){}
        assertArrayEquals(before,Files.readAllBytes(file.toPath()));
    }
    @Test public void otherBackendAndTruncatedStatesAreRejectedWithoutMutation()throws Exception{
        File file=Files.createTempFile("psx-backend-state",".psxlabstate").toFile();
        byte[] foreign={0x53,0x57,0x41,0x4e,0,0,0,1};Files.write(file.toPath(),foreign);
        try{BackendStateStore.read(file);fail();}catch(IOException expected){assertTrue(expected.getMessage().contains("SwanStation"));}
        assertArrayEquals(foreign,Files.readAllBytes(file.toPath()));
        BackendStateStore.write(file,"disc.chd",new byte[]{1,2,3,4});byte[] full=Files.readAllBytes(file.toPath());
        Files.write(file.toPath(),java.util.Arrays.copyOf(full,full.length-1));
        try{BackendStateStore.read(file);fail();}catch(EOFException expected){}
    }
}
