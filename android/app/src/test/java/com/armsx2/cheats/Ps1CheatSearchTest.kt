package com.armsx2.cheats

import org.junit.Assert.*
import org.junit.Test

class Ps1CheatSearchTest {
    private val names = listOf("Full special meter", "Fast movement", "Freeze timer",
        "Infinite Time", "No Sound FX", "Maximum score")
    private fun search(query: String) = names.filter { Ps1CheatSearch.matches(it, query) }

    @Test fun firstLetterMatchesOnlyNamesStartingWithIt() {
        assertEquals(listOf("Full special meter", "Fast movement", "Freeze timer"), search("F"))
        assertFalse(Ps1CheatSearch.matches("Infinite Time", "f"))
        assertFalse(Ps1CheatSearch.matches("No Sound FX", "F"))
    }

    @Test fun everyAdditionalLetterImmediatelyNarrowsTheSamePrefix() {
        assertEquals(3, search("f").size)
        for (query in listOf("fu", "ful", "full", "full s", "full special")) {
            assertEquals(query, listOf("Full special meter"), search(query))
        }
        assertEquals(listOf("Freeze timer"), search("fr"))
        assertTrue(search("full x").isEmpty())
    }

    @Test fun clearingAndBackspacingRestoreMatchingEntries() {
        assertEquals(1, search("ful").size)
        assertEquals(3, search("f").size)
        assertEquals(names, search(""))
        assertEquals(names, search("  "))
    }

    @Test fun caseSpacesNumbersAndSourceSuffixesDoNotBreakPrefixes() {
        assertTrue(Ps1CheatSearch.matches("  Full special meter (Action Replay)", " fU "))
        assertTrue(Ps1CheatSearch.matches("100 Lives", "1"))
        assertTrue(Ps1CheatSearch.matches("100 Lives", "100"))
        assertFalse(Ps1CheatSearch.matches("100 Lives", "Lives"))
        assertFalse(Ps1CheatSearch.matches("Unlock Full special meter", "Full"))
    }
}
