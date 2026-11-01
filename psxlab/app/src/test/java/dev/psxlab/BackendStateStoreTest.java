package dev.psxlab;

import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import static org.junit.Assert.*;

public class BackendStateStoreTest {
    @Test public void permanentIdentityAndAllSlotsSurviveNewLaunchDirectories()throws Exception{
        File root=Files.createTempDirectory("states-reopen").toFile();String source="content://games/tree/primary%3Agames/document/primary%3Agames%2FCrash.cue";
        String name="game-"+"a".repeat(64)+".cue",first="/data/user/0/com.nanodata.armsx/cache/saf-launch/111/"+name,next="/data/user/0/com.nanodata.armsx/cache/saf-launch/222/"+name;
        for(int slot=-1;slot<10;slot++){
            File file=BackendStateStore.file(root,source,slot);BackendStateStore.write(file,source,"",new byte[]{1,2,3});
            assertEquals(file,BackendStateStore.find(new File(root.getAbsolutePath()),source,next,slot));
            BackendStateStore.Metadata meta=BackendStateStore.metadata(file);assertEquals(source,meta.game());assertEquals("",meta.disc());assertEquals(slot,meta.slot());
        }
        assertTrue(BackendStateStore.sameGame(first,source,next));
        assertFalse(BackendStateStore.sameGame(first,source,next.replace("a".repeat(64),"b".repeat(64))));
        assertFalse(BackendStateStore.sameGame("/sdcard/CUE/game.cue",source,"/other/game.cue"));
    }
    @Test public void legacyStagedStatesRemainReadableAndAreNotOverwrittenByNewSaves()throws Exception{
        File root=Files.createTempDirectory("states-legacy").toFile();String source="content://games/Crash.pbp",name="game-"+"a".repeat(64)+".pbp";
        String old="/data/user/0/com.nanodata.armsx/cache/saf-launch/1/"+name,next="/data/user/0/com.nanodata.armsx/cache/saf-launch/2/"+name;
        byte[] state;
        try(dev.psxlab.core.Gpu gpu=new dev.psxlab.core.Gpu(false)){
            dev.psxlab.core.Bus bus=new dev.psxlab.core.Bus(gpu,new dev.psxlab.core.PadState(),false);
            state=dev.psxlab.core.SaveState.capture(new dev.psxlab.core.Cpu(bus),bus,gpu,old);
        }
        File file=BackendStateStore.file(root,old,2);file.getParentFile().mkdirs();
        try(DataOutputStream out=new DataOutputStream(new FileOutputStream(file))){out.writeInt(0x50534c41);out.writeInt(1);out.writeUTF(old);out.writeInt(state.length);out.write(state);}
        byte[] before=Files.readAllBytes(file.toPath());assertEquals(old,BackendStateStore.metadata(file).game());
        assertEquals(file,BackendStateStore.find(root,source,next,2));
        assertFalse(BackendStateStore.find(root,"another game",next.replace("a".repeat(64),"b".repeat(64)),2).exists());
        File canonical=BackendStateStore.file(root,source,2);BackendStateStore.write(canonical,source,"",state);
        assertEquals(canonical,BackendStateStore.find(root,source,next,2));assertArrayEquals(before,Files.readAllBytes(file.toPath()));
    }
    @Test public void catalogRejectsTruncationAndIgnoresCompanions()throws Exception{
        File root=Files.createTempDirectory("states-catalog").toFile(),file=BackendStateStore.file(root,"game",0);
        BackendStateStore.write(file,"game","disc-2.chd",new byte[]{1,2,3});
        assertEquals("disc-2.chd",BackendStateStore.metadata(file).disc());assertNull(BackendStateStore.slot(file.getName()+".png"));
        byte[] bytes=Files.readAllBytes(file.toPath());Files.write(file.toPath(),java.util.Arrays.copyOf(bytes,bytes.length-1));
        try{BackendStateStore.metadata(file);fail();}catch(IOException expected){}
    }
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
