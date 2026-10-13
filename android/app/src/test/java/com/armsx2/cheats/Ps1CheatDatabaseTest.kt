package com.armsx2.cheats

import org.junit.Assert.*
import org.junit.Test

class Ps1CheatDatabaseTest {
    @Test fun allVerifiedFormatsAreReturnedNotJustPreferredGameShark() {
        val rows = listOf("GameShark", "Game Buster", "Xploder", "Action Replay").mapIndexed { i, type ->
            "${i.toString().padStart(20, '0')}\tExample (USA) ($type)\tSLUS-00123\tSLUS-00123\t1\t0"
        }
        val index = Ps1CheatDatabaseIndex(rows.joinToString("\n").reader())
        assertEquals(4, index.automaticCatalogues("slus_001.23").size)
        assertTrue(index.automaticCatalogues("SLES-00123").isEmpty())
        assertTrue(index.automaticCatalogues(null).isEmpty())
    }

    @Test fun ambiguousVersionsStillNeedConfirmationButOtherFormatsCanLoad() {
        val first = "00000000000000000000\tExample (USA) (GameShark)\tSLUS-00123\tSLUS-00123\t1\t0"
        val second = first.replace("00000000000000000000", "11111111111111111111")
        val buster = first.replace("00000000000000000000", "22222222222222222222").replace("GameShark", "Game Buster")
        val index = Ps1CheatDatabaseIndex("$first\n$second\n$buster".reader())
        assertEquals(listOf("Game Buster"), index.automaticCatalogues("SLUS-00123").map { Ps1CheatDatabaseIndex.formatLabel(it.title) })
        assertEquals(3, index.suggestions("SLUS-00123", null).size)
        assertTrue(Ps1CheatDatabaseIndex(first.replace("\tSLUS-00123\tSLUS-00123", "\t\tSLUS-00123").reader())
            .automaticCatalogues("SLUS-00123").isEmpty())
    }

    @Test fun batchMergePreservesCustomSelectionsAndDistinctNamedVariants() {
        val existing = "# Keep me\n[Health]\n80000000 0001\n"
        val lists = listOf("GameShark" to "[Health]\n80000000 0002\n[Lives]\n30000002 0009\n",
            "Game Buster" to "[Health]\n80000000 0003\n[Other lives name]\n30000002 0009\n")
        val merged = Ps1CheatCatalogue.mergeCatalogues(existing, lists)
        assertTrue(merged.text.startsWith(existing))
        assertEquals(listOf("Health (GameShark)", "Lives", "Health (Game Buster)"), merged.addedNames)
        assertEquals(listOf("Health"), merged.selectionAfterImport(listOf("Health", "Lives")))
        assertEquals(Ps1CheatCatalogue.Merge(merged.text, 0), Ps1CheatCatalogue.mergeCatalogues(merged.text, lists))
    }

    @Test fun batchMergeDeduplicatesIdenticalCodesDespiteCaseAndComments() {
        val result = Ps1CheatCatalogue.mergeCatalogues("[Existing]\n8000abcd 1 # my code\n",
            listOf("GameShark" to "[Alternative name]\n8000ABCD 0001\n"))
        assertEquals(0, result.added)
    }

    @Test fun sourceSuffixesAreUniqueBoundedAndLiteral() {
        val name = "A".repeat(150)
        val original = "[$name]\n80000000 0001\n"
        val result = Ps1CheatCatalogue.mergeCatalogues(original, listOf("GameShark" to "[$name]\n80000000 0002\n"))
        assertTrue(result.addedNames.single().toByteArray().size <= 150)
        val money = Ps1CheatCatalogue.mergeCatalogues("[Money]\n80000000 0001\n",
            listOf("Game Buster" to "[Money]\n80000000 0002\n[Cash $100]\n80000000 0003\n"))
        assertTrue(money.text.contains("[Cash $100]"))
    }
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

    @Test fun serialFormatsAndSearchAreNormalisedWithoutAcceptingGarbage() {
        val index = Ps1CheatDatabaseIndex("$usa\n$pal".reader())
        for (serial in listOf("slus_001.23", "SLUS-001-23", "SLUS.00123", "SLUS_001.23;1")) {
            assertEquals("SLUS-00123", Ps1CheatDatabaseIndex.normaliseSerial(serial))
        }
        assertEquals(1, index.search("slus_001.23").size)
        assertEquals(1, index.search("example SLUS_001.23").size)
        assertEquals(1, index.search("slus_").size)
        assertNull(Ps1CheatDatabaseIndex.normaliseSerial("BOOT=cdrom:SLUS_001.23"))
        assertNull(Ps1CheatDatabaseIndex.normaliseSerial("SLUS-001234"))
    }

    @Test fun titleSuggestionsDoNotSubstituteAnotherRegion() {
        val index = Ps1CheatDatabaseIndex("$usa\n$pal".reader())
        assertEquals(listOf("Example (USA) (GameShark)"), index.suggestions("SLUS-00999", "Example").map { it.title })
        assertNull(index.automatic("SLUS-00999"))
        assertTrue(index.isRegionIncompatible(index.items[1], "SLUS-00999"))
    }

    @Test fun regionMetadataDistinguishesFrenchCodesFromOtherPalCountries() {
        val french = usa.replace("Example (USA)", "Example (France)").replace("SLUS-00123", "SLES-00124")
        val index = Ps1CheatDatabaseIndex(french.reader(), "Example (Europe)\tEurope\tSLES-00999".reader())
        assertTrue(index.suggestions("SLES-00999", "Example").isEmpty())
        assertTrue(index.isRegionIncompatible(index.items.single(), "SLES-00999"))
    }

    @Test fun romanNumeralsAreSuggestionsNotAutomaticIdentity() {
        val row = usa.replace("Example", "Final Fantasy VIII")
        val index = Ps1CheatDatabaseIndex(row.reader())
        assertEquals(1, index.suggestions(null, "Final Fantasy 8").size)
        assertNull(index.automatic("Final Fantasy 8"))
    }

    @Test fun demoAndDiscMismatchesAreBlockedWhenSerialMetadataIsKnown() {
        val demo = usa.replace("Example (USA)", "Example (Demo) (USA)")
        val demoIndex = Ps1CheatDatabaseIndex(demo.reader(), "Example (USA)\tUSA\tSLUS-00123".reader())
        assertTrue(demoIndex.isIncompatible(demoIndex.items.single(), "SLUS-00123"))
        assertNull(demoIndex.automatic("SLUS-00123"))
        assertTrue(demoIndex.suggestions("SLUS-00123", "Example").isEmpty())
        val disc = usa.replace("Example (USA)", "Example (USA) (Disc 1)")
        val discIndex = Ps1CheatDatabaseIndex(disc.reader(), "Example (USA) (Disc 2)\tUSA\tSLUS-00123".reader())
        assertTrue(discIndex.isIncompatible(discIndex.items.single(), "SLUS-00123"))
    }
}
