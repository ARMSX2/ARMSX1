package com.armsx2.cheats

import org.junit.Assert.*
import org.junit.Test

class Ps1CheatDatabaseTest {
    private val usa = "0123456789abcdef0123\tExample (USA) (GameShark)\tSLUS-00123\tSLUS-00123\t2\t1"
    private val pal = "abcdef0123456789abcd\tExample (Europe) (GameShark)\tSLES-00123\tSLES-00123\t3\t0"
    private val world = "11111111111111111111\tOther Game (World) (GameShark)\t\tSLUS-00456\t1\t1"

    @Test fun automaticLookupRequiresTheCorrectRegionalSerial() {
        val index = Ps1CheatDatabaseIndex("$usa\n$pal\n$world".reader())
        assertEquals(2, index.automatic("slus_001.23")?.cheatCount)
        assertEquals(3, index.automatic("SLES-00123")?.cheatCount)
        assertNull(index.automatic("SLUS-00999"))
        assertNull(index.automatic("Example"))
    }

    @Test fun worldAndAmbiguousListsRequireConfirmation() {
        val index = Ps1CheatDatabaseIndex("$usa\n$world".reader())
        assertNull(index.automatic("SLUS-00456"))
        assertEquals(1, index.suggestions("SLUS-00456", null).size)
        val alternative = usa.replace("0123456789abcdef0123", "22222222222222222222")
        assertNull(Ps1CheatDatabaseIndex("$usa\n$alternative".reader()).automatic("SLUS-00123"))
    }

    @Test fun searchNarrowsByEveryWordAndIncludesSerials() {
        val index = Ps1CheatDatabaseIndex("$usa\n$pal\n$world".reader())
        assertEquals(2, index.search("example").size)
        assertEquals(1, index.search("example europe").size)
        assertEquals(1, index.search("SLES-00123").size)
        assertTrue(index.search("nothing").isEmpty())
    }

    @Test fun addingDatabaseCheatsPreservesCustomTextAndIsIdempotent() {
        val custom = "# My codes\n[Infinite Health]\n80000000 0001\n"
        val incoming = "[Infinite Health]\n80000000 0002\n[Infinite Lives]\n30000002 0009\n"
        val merged = Ps1CheatCatalogue.merge(custom, incoming)
        assertEquals(1, merged.added)
        assertEquals(listOf("Infinite Lives"), merged.addedNames)
        assertTrue(merged.text.startsWith(custom))
        assertFalse(merged.text.contains("80000000 0002"))
        assertEquals(Ps1CheatCatalogue.Merge(merged.text, 0), Ps1CheatCatalogue.merge(merged.text, incoming))
    }

    @Test fun duplicateNamesAreComparedCaseInsensitively() {
        val existing = "[infinite health]\n80000000 0001"
        assertEquals(Ps1CheatCatalogue.Merge(existing, 0),
            Ps1CheatCatalogue.merge(existing, "[ Infinite Health ]\n80000000 0002\n"))
    }

    @Test fun indentedAndBomPrefixedCustomGroupsArePreserved() {
        val existing = "\uFEFF  [Infinite Health]\n80000000 0001\n"
        assertEquals(Ps1CheatCatalogue.Merge(existing, 0),
            Ps1CheatCatalogue.merge(existing, "[Infinite Health]\n80000000 0002\n"))
    }

    @Test fun newlyAddedGroupsCannotBeArmedByStaleSavedNames() {
        val merged = Ps1CheatCatalogue.merge("[Custom]\n80000000 0001\n",
            "[New database cheat]\n80000002 0002\n")
        assertEquals(listOf("Custom", "Other orphan"),
            merged.selectionAfterImport(listOf("Custom", "new DATABASE cheat", "Other orphan")))
    }
}
