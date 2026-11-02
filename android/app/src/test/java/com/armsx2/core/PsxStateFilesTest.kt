package com.armsx2.core

import org.junit.Assert.*
import org.junit.Test

class PsxStateFilesTest {
    @Test fun renamedSaveSourcesResolveByDiscIdWithoutMatchingFilenames() {
        val identity = dev.psxlab.BackendStateStore.identity("SLUS_005.94;1", byteArrayOf(1))
        val library = "content://library/new-name.chd"
        assertEquals(library, PsxStateFiles.source("/old/file.pbp", identity, mapOf(library to "SLUS-00594")))
        assertEquals("/old/file.pbp", PsxStateFiles.source("/old/file.pbp", identity, mapOf(library to "SLES-00594")))
    }
    @Test fun exactSourceWinsAndAmbiguousDiscIdsDoNotPickAnotherRevision() {
        val identity = dev.psxlab.BackendStateStore.identity("SLUS_005.94;1", byteArrayOf(1))
        val sources = mapOf("/games/original.chd" to "SLUS-00594", "/games/translation.pbp" to "SLUS-00594")
        assertEquals("/games/original.chd", PsxStateFiles.source("/games/original.chd", identity, sources))
        assertEquals("/renamed/missing.chd", PsxStateFiles.source("/renamed/missing.chd", identity, sources))
    }
    @Test fun restoresOldStagedIdentityFromTheLibraryWithoutAnActiveGame() {
        val source = "content://games/tree/primary%3Agames/document/primary%3Agames%2FCrash.pbp"
        val staged = "/data/user/0/com.nanodata.armsx/cache/saf-launch/123/${Ps1SafText.launchFileName(source, "Crash.pbp")}"
        assertEquals(source, PsxStateFiles.source(staged, listOf("content://other/Crash.pbp", source)))
        assertNull(PsxStateFiles.source(staged, listOf("content://other/Crash.pbp")))
    }
    @Test fun newStatesRetainTheirSourceEvenWithoutALibraryScan() {
        assertEquals("content://games/Crash.chd", PsxStateFiles.source("content://games/Crash.chd", emptyList()))
        val file = java.io.File("games/Crash.cue").absoluteFile
        assertEquals(file.toURI().toString(), PsxStateFiles.source(file.path, listOf(file.toURI().toString())))
        assertEquals(listOf("slot.psxlabstate.png"), PsxStateFiles.companions("slot.psxlabstate"))
    }
}
