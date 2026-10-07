package com.armsx2.core

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class Ps1CoverTitleIndexTest {
    private val index = File("src/main/assets/ps1-cover-titles.tsv").reader().use(::Ps1CoverTitleIndex)

    @Test fun resolvesUnidentifiedSafFilenamesAcrossTheLibrary() {
        assertTrue(index.size > 8_000)
        assertEquals("SLUS-01272", index.find(null, "007 - the world is not enough (usa).chd"))
        assertEquals("SLUS-00838", index.find(null, "bugs bunny - lost in time (usa) (en,fr,es).chd"))
        assertEquals("SLUS-01476", index.find(null, "capcom vs. snk pro (usa).chd"))
        assertEquals("SLUS-00844", index.find(null, "chocobo racing (usa).chd"))
        assertEquals("SLUS-01436", index.find(null, "digimon world 3 (usa).chd"))
    }

    @Test fun honoursRegionDiscAndPlaylist() {
        assertEquals("SLES-02079", index.find(null, "Chocobo Racing (Europe).chd"))
        assertNull(index.find(null, "Chocobo Racing (Japan).chd"))
        assertEquals("SLUS-01201", index.find(null, "Alone in the Dark - The New Nightmare (USA).m3u"))
        assertEquals("SLUS-01377", index.find(null, "Alone in the Dark - The New Nightmare (USA) (Disc 2).chd"))
        assertNull(index.find(null, "Alone in the Dark - The New Nightmare (USA) (Disc 3).chd"))
        assertEquals("SLPS-01300", index.find(null, "Tekken 3 (Japan).chd"))
    }

    @Test fun toleratesArticlesAndTitlesContainingPeriods() {
        assertEquals("SLUS-00515", index.find(null, "Jurassic Park - The Lost World (USA).chd"))
        assertEquals("SLUS-00515", index.find(null, "The Lost World - Jurassic Park (USA).chd"))
        assertEquals("SLUS-01476", index.find("Capcom vs. SNK Pro", null))
        assertNull(index.find(null, "Unrecognised Game (USA).chd"))
    }

    @Test fun malformedRecordsAndNonRetailSerialsCannotSupplyArtwork() {
        val small = Ps1CoverTitleIndex("Bad row\nGame (USA)\tUSA\tSLUS-00001-P\nGame (USA)\tUSA\tSLUS-00001\n".reader())
        assertEquals(1, small.size)
        assertEquals("SLUS-00001", small.find("Game", null))
        assertNull(small.find(null, "Game (Europe).chd"))
    }
}
