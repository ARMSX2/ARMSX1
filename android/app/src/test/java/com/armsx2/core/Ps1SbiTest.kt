package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Test

class Ps1SbiTest {
    @Test
    fun singleImageUsesItsStemForDiscZero() {
        assertEquals(mapOf(0 to "Dino Crisis (Spain).sbi"),
            Ps1Sbi.directSidecars("Dino Crisis (Spain).chd", listOf("Dino Crisis (Spain).chd", "Dino Crisis (Spain).sbi", "Other.sbi")))
    }

    @Test
    fun nonPbpImageIgnoresIndexedSidecars() {
        assertEquals(emptyMap<Int, String>(), Ps1Sbi.directSidecars("Game.chd", listOf("Game_1.sbi")))
    }

    @Test
    fun pbpMapsIndexedSidecarsToEmbeddedDiscs() {
        assertEquals(mapOf(0 to "Game_1.sbi", 1 to "Game_2.sbi"),
            Ps1Sbi.directSidecars("Game.pbp", listOf("Game.pbp", "Game_1.sbi", "Game_2.sbi")))
    }

    @Test
    fun pbpPlainStemOnlyFillsDiscZeroWhenNoIndexedFileExists() {
        assertEquals(mapOf(0 to "Game.sbi", 2 to "Game_3.sbi"),
            Ps1Sbi.directSidecars("Game.pbp", listOf("Game.sbi", "Game_3.sbi")))
        assertEquals(mapOf(0 to "Game_1.sbi"),
            Ps1Sbi.directSidecars("Game.pbp", listOf("Game.sbi", "Game_1.sbi")))
    }
}
