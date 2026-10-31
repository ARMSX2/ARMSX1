package com.armsx2.core

import org.junit.Assert.*
import org.junit.Test

class Ps1BulkCardMatcherTest {
    private val usa = Ps1BulkCardMatcher.Game("usa", "SLUS-00860", listOf("Tony Hawk's Pro Skater"))
    private val pal = Ps1BulkCardMatcher.Game("pal", "SLES-02055", listOf("Tony Hawk's Pro Skater"))
    @Test fun matchesRegionBySerialAndRecognizesDuckstationSlotSuffix() {
        assertEquals(listOf(Ps1BulkCardMatcher.Match("usa", 2)),
            Ps1BulkCardMatcher.match("SLUS_008.60_2.mcd", listOf(usa, pal)))
    }
    @Test fun regionMismatchNeverFallsBackToTitle() {
        assertTrue(Ps1BulkCardMatcher.match("Tony Hawk's Pro Skater SLUS-99999_1.mcd", listOf(usa, pal)).isEmpty())
    }
    @Test fun exactUniqueTitleMatchesButAmbiguousTitleDoesNot() {
        assertEquals(listOf(Ps1BulkCardMatcher.Match("usa", 1)),
            Ps1BulkCardMatcher.match("Tony Hawk's Pro Skater_1.mcd", listOf(usa)))
        assertTrue(Ps1BulkCardMatcher.match("Tony Hawk's Pro Skater_1.mcd", listOf(usa, pal)).isEmpty())
        assertTrue(Ps1BulkCardMatcher.match("Tony_1.mcd", listOf(usa)).isEmpty())
    }
    @Test fun plainSrmAndGameNamesEndingInNumbersDoNotLoseTheNumber() {
        val game = Ps1BulkCardMatcher.Game("crash", "SCUS-94244", listOf("Crash Bandicoot 3"))
        assertEquals(listOf(Ps1BulkCardMatcher.Match("crash", 1)),
            Ps1BulkCardMatcher.match("Crash Bandicoot 3.srm", listOf(game)))
        val second = game.copy(titles = listOf("Crash Bandicoot 2"))
        assertEquals(listOf(Ps1BulkCardMatcher.Match("crash", 1)),
            Ps1BulkCardMatcher.match("Crash Bandicoot 2.mcd", listOf(second)))
    }
}
