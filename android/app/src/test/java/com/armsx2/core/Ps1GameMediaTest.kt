package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class Ps1GameMediaTest {
    private val base = "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master"

    @Test fun dumpNameKeepsRegionAndDropsExtension() {
        assertEquals("Disney's Donald Duck - Goin' Quackers (USA)",
            Ps1GameMedia.libretroName("Disney's Donald Duck - Goin' Quackers (USA).chd"))
    }

    @Test fun libretroReservedCharactersBecomeUnderscores() {
        assertEquals("Tom _ Jerry_ Fists of Furry (USA)", Ps1GameMedia.libretroName("Tom & Jerry: Fists of Furry (USA).cue"))
    }

    @Test fun snapsComeBeforeTitlesAndSpacesAreEncoded() {
        val urls = Ps1GameMedia.snapUrls("Crash Bandicoot (USA).bin")
        assertEquals(listOf(
            "$base/Named_Snaps/Crash%20Bandicoot%20%28USA%29.png",
            "$base/Named_Titles/Crash%20Bandicoot%20%28USA%29.png",
        ), urls)
    }

    @Test fun multiDiscAlsoTriesTheDisclessName() {
        val urls = Ps1GameMedia.snapUrls("Final Fantasy VII (USA) (Disc 1).chd", titles = false)
        assertEquals("$base/Named_Snaps/Final%20Fantasy%20VII%20%28USA%29%20%28Disc%201%29.png", urls[0])
        assertTrue(urls.contains("$base/Named_Snaps/Final%20Fantasy%20VII%20%28USA%29.png"))
    }

    @Test fun lowercasedFileNamesAlsoTryRedumpCapitalisation() {
        assertEquals("007 - The World Is Not Enough (USA)",
            Ps1GameMedia.titleCase("007 - the world is not enough (usa)", smallWords = false))
        assertEquals("Lord of the Rings (USA)", Ps1GameMedia.titleCase("lord of the rings (usa)", smallWords = true))
        assertTrue(Ps1GameMedia.snapUrls("007 - the world is not enough (usa).chd").contains(
            "$base/Named_Snaps/007%20-%20The%20World%20Is%20Not%20Enough%20%28USA%29.png"))
    }

    @Test fun switchedOffKindsAreLeftOut() {
        assertEquals(emptyList<String>(), Ps1GameMedia.snapUrls("Game (USA).chd", snaps = false, titles = false))
    }

    @Test fun searchTermDropsTags() {
        assertEquals("Chrono Cross", Ps1GameMedia.searchTerm("Chrono Cross (USA) (Disc 1)"))
    }
}
