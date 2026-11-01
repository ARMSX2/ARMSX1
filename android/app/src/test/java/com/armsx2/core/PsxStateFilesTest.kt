package com.armsx2.core

import org.junit.Assert.*
import org.junit.Test

class PsxStateFilesTest {
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
