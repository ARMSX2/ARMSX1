package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class Ps1TitleSerialsTest {
    @Test fun dragonWarriorDiscsAndPlaylist() {
        assertEquals("SLUS-01206", Ps1TitleSerials.coverSerialFor(null, "Dragon Warrior VII (USA).m3u"))
        assertEquals("SLUS-01346", Ps1TitleSerials.coverSerialFor(null, "Dragon Warrior VII (USA) (Disc 2).chd"))
        assertEquals("SLUS-01346", Ps1TitleSerials.coverSerialFor(null, "Dragon Warrior VII Disc 2.chd"))
        assertNull(Ps1TitleSerials.coverSerialFor(null, "Dragon Warrior VII (Disc 3).chd"))
    }

    @Test fun screenshotTitlesAndRegions() {
        assertEquals("SLPS-03266", Ps1TitleSerials.coverSerialFor(null, "Hamster Club-i (Japan).chd"))
        assertEquals("SLPS-03266", Ps1TitleSerials.coverSerialFor("Hamster Club-i", null))
        assertEquals("SLUS-00402", Ps1TitleSerials.coverSerialFor(null, "Tekken 3 (USA).chd"))
        assertEquals("SCES-01237", Ps1TitleSerials.coverSerialFor(null, "Tekken 3 (Europe).chd"))
        assertNull(Ps1TitleSerials.coverSerialFor(null, "Tekken 3 (Japan).chd"))
        assertNull(Ps1TitleSerials.coverSerialFor(null, "Hamster Club-i (USA).chd"))
        assertNull(Ps1TitleSerials.coverSerialFor(null, "Dragon Warrior VII (Japan).chd"))
    }

    @Test fun existingTitlesAndUnknownGames() {
        assertEquals("SLUS-01080", Ps1TitleSerials.coverSerialFor(null, "Chrono Cross (USA) (Disc 2).chd"))
        assertNull(Ps1TitleSerials.coverSerialFor(null, "Unrecognised game.chd"))
    }
}
