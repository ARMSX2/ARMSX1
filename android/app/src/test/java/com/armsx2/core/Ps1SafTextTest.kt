package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Test

class Ps1SafTextTest {
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
}
