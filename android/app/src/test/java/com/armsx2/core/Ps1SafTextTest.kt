package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class Ps1SafTextTest {
    @Test
    fun launchIdentitySeparatesGamesWithTheSameNameAndDiscLayout() {
        val first = "content://provider/tree/roms/document/roms%2FA%2Fgame.chd"
        val second = "content://provider/tree/roms/document/roms%2FB%2Fgame.chd"
        for (extension in listOf("chd", "bin", "cue", "iso", "pbp")) {
            val a = Ps1SafText.launchFileName(first, "game.$extension")
            val b = Ps1SafText.launchFileName(second, "game.$extension")
            assertNotEquals(a, b)
            // The native path builder discards each temporary parent directory.
            assertEquals(a, Ps1SafText.baseName("session-1/$a"))
            assertEquals(a, Ps1SafText.baseName("session-2/${Ps1SafText.launchFileName(first, "game.$extension")}"))
            assertTrue(a.endsWith(".$extension"))
        }
    }

    @Test
    fun launchIdentitySurvivesNativeStemLimitsAndUntrustedDisplayNames() {
        val uri = "content://provider/document/roms%2Fgame"
        for (name in listOf("../game.BIN", "x".repeat(500) + ".chd", "game.iso/../../outside")) {
            val local = Ps1SafText.launchFileName(uri, name)
            assertTrue(local.matches(Regex("game-[0-9a-f]{64}\\.[a-z0-9]{1,8}")))
            assertTrue(local.substringBeforeLast('.').length < 128)
        }
        assertEquals(Ps1SafText.launchFileName(uri, "game.bin"), Ps1SafText.launchFileName(uri, "renamed.BIN"))
    }

    @Test
    fun cueReferencesAndRewritePreserveTrackKinds() {
        val cue = """
            FILE "Tekken 3 (Track 1).bin" BINARY
              TRACK 01 MODE2/2352
            FILE Audio\Tekken-02.wav WAVE
              TRACK 02 AUDIO
        """.trimIndent()

        assertEquals(
            listOf("Tekken 3 (Track 1).bin", "Audio\\Tekken-02.wav"),
            Ps1SafText.cueReferences(cue),
        )
        assertEquals(
            """
                FILE "track-00.bin" BINARY
                  TRACK 01 MODE2/2352
                FILE "track-01.wav" WAVE
                  TRACK 02 AUDIO
            """.trimIndent(),
            Ps1SafText.rewriteCue(cue, listOf("track-00.bin", "track-01.wav")),
        )
    }

    @Test
    fun playlistSkipsHeadersCommentsAndWhitespace() {
        val playlist = """
            #EXTM3U

            # Disc order
              Game (Disc 1).cue
            Game (Disc 2).chd
        """.trimIndent()

        assertEquals(
            listOf("Game (Disc 1).cue", "Game (Disc 2).chd"),
            Ps1SafText.playlistEntries(playlist),
        )
    }

    @Test
    fun baseNameHandlesCueWindowsSeparators() {
        assertEquals("Track 01.BIN", Ps1SafText.baseName("tracks\\Track 01.BIN"))
        assertEquals("bin", Ps1SafText.extension("game.BIN"))
        assertEquals("", Ps1SafText.extension("game.iso/../../outside"))
        assertEquals("", Ps1SafText.extension("game.extension-is-too-long"))
    }

    @Test
    fun bomAndNestedReferencesRemainResolvable() {
        assertEquals(listOf("discs/Game.cue"), Ps1SafText.playlistEntries("\uFEFF#EXTM3U\ndiscs/Game.cue"))
        assertEquals(listOf("tracks\\Game.bin"), Ps1SafText.cueReferences("\uFEFFFILE \"tracks\\Game.bin\" BINARY"))
        assertEquals(listOf("tracks", "Game.bin"), Ps1SafText.relativeSegments(".\\tracks\\Game.bin"))
        assertEquals(listOf("..", "Game.bin"), Ps1SafText.relativeSegments("../Game.bin"))
    }

    @Test(expected = IllegalArgumentException::class)
    fun absoluteReferencesAreRejected() {
        Ps1SafText.relativeSegments("/storage/elsewhere/Game.bin")
    }
}
