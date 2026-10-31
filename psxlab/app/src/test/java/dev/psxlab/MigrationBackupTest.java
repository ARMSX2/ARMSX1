package dev.psxlab;

import org.junit.Test;
import java.io.*;
import java.nio.file.*;
import java.util.zip.*;
import static org.junit.Assert.*;

public class MigrationBackupTest {
    @Test public void archivedCardsAndStatesAreRecoverableAndSourcesAreUnchanged()throws Exception{
        Path root=Files.createTempDirectory("psx-save-backup");Files.createDirectories(root.resolve("cards"));Files.createDirectories(root.resolve("states"));
        byte[] card=dev.psxlab.core.MemoryCard.formatted(),state=new byte[131071];new java.util.Random(91).nextBytes(state);
        Files.write(root.resolve("cards/card1.mcd"),card);Files.write(root.resolve("states/game.psxs"),state);
        File archive=MigrationBackup.create(root.toFile(),root.resolve("backups").toFile(),"cards","states");
        try(ZipFile zip=new ZipFile(archive)){
            assertArrayEquals(card,zip.getInputStream(zip.getEntry("cards/card1.mcd")).readAllBytes());
            assertArrayEquals(state,zip.getInputStream(zip.getEntry("states/game.psxs")).readAllBytes());
            String manifest=new String(zip.getInputStream(zip.getEntry("MANIFEST.tsv")).readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
            assertTrue(manifest.contains(MigrationBackup.digest(card)));assertTrue(manifest.contains(MigrationBackup.digest(state)));
        }
        assertArrayEquals(card,Files.readAllBytes(root.resolve("cards/card1.mcd")));assertArrayEquals(state,Files.readAllBytes(root.resolve("states/game.psxs")));
    }
    @Test public void traversalOutsideSaveRootIsRejected()throws Exception{
        Path parent=Files.createTempDirectory("psx-backup-boundary"),root=Files.createDirectory(parent.resolve("root"));Files.write(parent.resolve("outside"),new byte[]{1,2,3});
        try{MigrationBackup.create(root.toFile(),root.resolve("backup").toFile(),"../outside");fail("Traversal accepted");}catch(IOException expected){assertTrue(expected.getMessage().contains("Unsafe"));}
    }
}
