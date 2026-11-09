package dev.psxlab;

import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import static org.junit.Assert.*;

public class BackendStateStoreTest {
    private static String id(int value){return BackendStateStore.identity("SLUS_005.94;1",new byte[]{(byte)value});}
    @Test public void discIdentityIgnoresBootNameSpellingAndKeepsDifferentRevisionsSeparate(){
        assertEquals(id(1),BackendStateStore.normalize(id(1)));
        assertEquals(MigrationBackup.digest(id(1).getBytes(java.nio.charset.StandardCharsets.UTF_8))+"-0.psxlabstate",BackendStateStore.file(new File("root"),id(1),0).getName());
        assertEquals(id(1),BackendStateStore.identity("cdrom:\\slus-00594;1",new byte[]{1}));
        assertEquals("SLUS-00594",BackendStateStore.serial(id(1)));assertNotEquals(id(1),id(2));
        assertNotEquals(id(1),BackendStateStore.identity("SLES_005.94",new byte[]{1}));
        assertEquals(BackendStateStore.identity("A.EXE",new byte[]{4}),BackendStateStore.identity("renamed.exe",new byte[]{4}));
        assertNotEquals(BackendStateStore.identity("A.EXE",new byte[]{4}),BackendStateStore.identity("A.EXE",new byte[]{5}));
    }
    @Test public void everySlotIsSharedAcrossFrontendLibraryAndRenamedSources()throws Exception{
        File root=Files.createTempDirectory("shared-game-id").toFile();String library="content://library/tree/Games/document/Crash.chd",frontend="/storage/ABCD/Games/renamed.pbp";
        for(int slot=-1;slot<=9;slot++){
            File canonical=BackendStateStore.file(root,id(1),slot);BackendStateStore.write(canonical,library,"disc-two.chd",id(1),new byte[]{1,2,3});
            assertEquals(canonical,BackendStateStore.find(root,id(1),frontend,"/cache/launch-2.chd",slot));
            assertEquals(canonical,BackendStateStore.find(root,id(1),"file:///another/new-name.cue","",slot));
            assertFalse(BackendStateStore.find(root,id(2),library,"",slot).exists());
            BackendStateStore.Stored saved=BackendStateStore.read(canonical);assertEquals(library,saved.game());assertEquals("disc-two.chd",saved.disc());assertEquals(id(1),saved.identity());assertArrayEquals(new byte[]{1,2,3},saved.payload());
        }
    }
    @Test public void saveStateRecordsTheSelectedPbpDiscAndReadsLegacyStates()throws Exception{
        File root=Files.createTempDirectory("embedded-disc-state").toFile();
        String source="/games/collection.pbp";
        File legacy=BackendStateStore.file(root,id(1),0),current=BackendStateStore.file(root,id(1),1);
        BackendStateStore.write(legacy,source,"",id(1),new byte[]{1});
        BackendStateStore.write(current,source,"",id(1),2,new byte[]{2});
        assertEquals(0,BackendStateStore.read(legacy).discIndex());
        BackendStateStore.Stored state=BackendStateStore.read(current);
        assertEquals(2,state.discIndex());assertEquals(id(1),state.identity());assertArrayEquals(new byte[]{2},state.payload());
        assertEquals(id(1),BackendStateStore.metadata(current).identity());
        assertThrows(IOException.class,()->BackendStateStore.write(current,source,"",id(1),5,new byte[]{3}));
        assertEquals(2,BackendStateStore.read(current).discIndex());
    }
    @Test public void migrationKeepsNewestSlotAndArchivesEveryOriginalAndPreview()throws Exception{
        File root=Files.createTempDirectory("legacy-aliases").toFile();String library="content://library/Crash.chd",frontend="/games/Renamed.pbp";
        File older=BackendStateStore.file(root,library,2),newer=BackendStateStore.file(root,frontend,2);
        BackendStateStore.write(older,library,"",new byte[]{1});BackendStateStore.write(newer,frontend,"disc-2.chd",new byte[]{2});
        older.setLastModified(1000);newer.setLastModified(2000);byte[] oldBytes=Files.readAllBytes(older.toPath()),newBytes=Files.readAllBytes(newer.toPath());
        Files.write(new File(newer+".png").toPath(),new byte[]{3});
        assertEquals(1,BackendStateStore.adoptLegacy(root,id(1),"/third-address/game.bin","",java.util.Set.of(),source->id(1)));
        File target=BackendStateStore.file(root,id(1),2);assertArrayEquals(new byte[]{2},BackendStateStore.read(target).payload());assertEquals("disc-2.chd",BackendStateStore.read(target).disc());
        assertEquals(2000,target.lastModified());assertArrayEquals(new byte[]{3},Files.readAllBytes(new File(target+".png").toPath()));assertFalse(older.exists());assertFalse(newer.exists());
        try(var paths=Files.walk(new File(root,"states/legacy").toPath())){var all=paths.filter(Files::isRegularFile).toList();assertEquals(3,all.size());for(var p:all){if(p.getFileName().toString().equals(older.getName()))assertArrayEquals(oldBytes,Files.readAllBytes(p));if(p.getFileName().toString().equals(newer.getName()))assertArrayEquals(newBytes,Files.readAllBytes(p));}}
        assertTrue(target.delete());assertEquals(0,BackendStateStore.adoptLegacy(root,id(1),frontend,"",java.util.Set.of(),source->id(1)));assertFalse(BackendStateStore.find(root,id(1),frontend,"",2).exists());
    }
    @Test public void migrationNeverOverwritesExistingCanonicalProgress()throws Exception{
        File root=Files.createTempDirectory("canonical-progress").toFile(),target=BackendStateStore.file(root,id(1),0),legacy=BackendStateStore.file(root,"older-launch",0);
        BackendStateStore.write(target,"new-launch","",id(1),new byte[]{9});BackendStateStore.write(legacy,"older-launch","",new byte[]{1});legacy.setLastModified(System.currentTimeMillis()+10000);byte[] before=Files.readAllBytes(target.toPath());
        assertEquals(0,BackendStateStore.adoptLegacy(root,id(1),"new-launch","",java.util.Set.of(),source->id(1)));assertArrayEquals(before,Files.readAllBytes(target.toPath()));assertFalse(legacy.exists());
    }
    @Test public void unknownSourcesArePreservedAndIdenticalFilenamesDoNotMerge()throws Exception{
        File root=Files.createTempDirectory("state-isolation").toFile(),other=BackendStateStore.file(root,"/other/Crash.chd",0),missing=BackendStateStore.file(root,"/missing/Crash.chd",1);
        BackendStateStore.write(other,"/other/Crash.chd","",new byte[]{1});BackendStateStore.write(missing,"/missing/Crash.chd","",new byte[]{2});
        assertEquals(0,BackendStateStore.adoptLegacy(root,id(1),"/current/Crash.chd","",java.util.Set.of(),source->{if(source.contains("missing"))throw new IOException("Missing image");return id(2);}));
        assertTrue(other.exists());assertTrue(missing.exists());assertFalse(BackendStateStore.find(root,id(1),"/current/Crash.chd","",0).exists());
    }
    @Test public void libraryAliasRecoversDeletedStagingWithoutOpeningTheOldCopy()throws Exception{
        File root=Files.createTempDirectory("staged-game-alias").toFile();String library="content://library/document/disc.pbp",staged="/data/user/0/com.nanodata.armsx/cache/saf-launch/old/game-"+MigrationBackup.digest(library.getBytes(java.nio.charset.StandardCharsets.UTF_8))+".pbp";
        File old=BackendStateStore.file(root,staged,1);BackendStateStore.write(old,staged,"",new byte[]{7});
        assertEquals(1,BackendStateStore.adoptLegacy(root,id(1),"/storage/emulated/0/Renamed.pbp","",java.util.Set.of(library),source->{throw new AssertionError("Alias must not reopen deleted media");}));
        assertArrayEquals(new byte[]{7},BackendStateStore.read(BackendStateStore.file(root,id(1),1)).payload());
    }
    @Test public void resolvedOtherGameIdentitiesAreCachedOutsideThePlaybackLoop()throws Exception{
        File root=Files.createTempDirectory("state-probe-cache").toFile(),other=BackendStateStore.file(root,"another-game",0);BackendStateStore.write(other,"another-game","",new byte[]{1});int[] probes={0};
        for(int attempt=0;attempt<2;attempt++)BackendStateStore.adoptLegacy(root,id(1),"current-game","",java.util.Set.of(),source->{probes[0]++;return id(2);});
        assertEquals(1,probes[0]);assertEquals(id(2),BackendStateStore.metadata(other).identity());assertTrue(other.exists());
    }
    @Test public void invalidIdentityDoesNotReplaceExistingState()throws Exception{
        File file=BackendStateStore.file(Files.createTempDirectory("state-header-identity").toFile(),id(1),0);BackendStateStore.write(file,"game","",id(1),new byte[]{1});byte[] before=Files.readAllBytes(file.toPath());
        assertThrows(IOException.class,()->BackendStateStore.write(file,"game","","bad-id",new byte[]{2}));assertArrayEquals(before,Files.readAllBytes(file.toPath()));
    }
    @Test public void brokenCanonicalFileDoesNotHideOrArchiveARecoverableLegacySlot()throws Exception{
        File root=Files.createTempDirectory("broken-canonical").toFile(),legacy=BackendStateStore.file(root,"source",0),target=BackendStateStore.file(root,id(1),0);
        BackendStateStore.write(legacy,"source","",new byte[]{8});Files.write(target.toPath(),new byte[]{1,2});
        assertEquals(0,BackendStateStore.adoptLegacy(root,id(1),"source","",java.util.Set.of(),source->id(1)));
        assertTrue(legacy.exists());assertEquals(legacy,BackendStateStore.find(root,id(1),"source","",0));
    }
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
