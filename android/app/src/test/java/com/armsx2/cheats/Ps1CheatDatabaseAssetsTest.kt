package com.armsx2.cheats

import java.io.File
import java.util.zip.ZipFile
import org.junit.Assert.*
import org.junit.Test

/** Exercise the real packaged catalogue, not just synthetic index fixtures. */
class Ps1CheatDatabaseAssetsTest {
    private fun assets(): File = generateSequence(File(requireNotNull(System.getProperty("user.dir")))) { it.parentFile }
        .map { File(it, "android/app/src/main/assets") }.first { File(it, "ps1-cheats-index.tsv").isFile }

    @Test fun everyBundledListContainsExactlyItsAdvertisedCompleteCheats() {
        val assets = assets()
        val index = File(assets, "ps1-cheats-index.tsv").reader().use(::Ps1CheatDatabaseIndex)
        assertTrue(index.items.size > 1500)
        assertEquals(index.items.size, index.items.map { it.id }.distinct().size)
        ZipFile(File(assets, "ps1-cheats.zip")).use { zip ->
            assertEquals(index.items.size, zip.size())
            for (item in index.items) {
                val entry = zip.getEntry(item.id + ".cht")
                assertNotNull(item.title, entry)
                val text = zip.getInputStream(entry).bufferedReader().use { it.readText() }
                assertEquals(item.title, item.cheatCount, Ps1CheatCatalogue.merge("", text).added)
                for (line in text.lineSequence().filter { it.isNotBlank() && !it.startsWith('#') && !it.startsWith(';') && !it.startsWith('@') && !it.startsWith('[') }) {
                    assertTrue(item.title + ": " + line, line.matches(Regex("[0-9A-F]{8} [0-9A-F]{4}")))
                    assertTrue(line, line.take(2) in setOf("30", "80", "50", "D0", "D1", "D2", "D3", "E0", "E1", "E2", "E3"))
                }
            }
        }
    }

    @Test fun realRegionalMatchWorksAndWorldListNeedsConfirmation() {
        val index = File(assets(), "ps1-cheats-index.tsv").reader().use(::Ps1CheatDatabaseIndex)
        assertTrue(index.automatic("SLUS-01201")!!.title.contains("Alone in the Dark - The New Nightmare"))
        assertNull(index.automatic("SLUS-00402"))
        assertTrue(index.suggestions("SLUS-00402", null).any { it.title.contains("Tekken 3") })
        assertNull(index.automatic("SLES-00402"))
    }

    @Test fun standalonePreviewIsAvailableForEveryCatalogueWithoutGameIdentity() {
        val index = File(assets(), "ps1-cheats-index.tsv").reader().use(::Ps1CheatDatabaseIndex)
        assertEquals(index.items, index.search(""))
        ZipFile(File(assets(), "ps1-cheats.zip")).use { zip ->
            for (item in index.items) {
                val text = zip.getInputStream(zip.getEntry(item.id + ".cht")).bufferedReader().use { it.readText() }
                val preview = Ps1CheatCatalogue.merge("", text)
                assertEquals(item.title, item.cheatCount, preview.addedNames.size)
                assertTrue(item.title, preview.addedNames.all { it.isNotBlank() })
            }
        }
    }

    @Test fun warpedUsaMatchesOnlyItsOwnSerialAndLeavesEuropeSeparate() {
        val index = File(assets(), "ps1-cheats-index.tsv").reader().use(::Ps1CheatDatabaseIndex)
        val usa = requireNotNull(index.automatic("SCUS_942.44"))
        val europe = requireNotNull(index.automatic("SCES-01420"))
        assertTrue(usa.title.contains("(USA)"))
        assertEquals(6, usa.cheatCount)
        assertEquals(setOf("SCUS-94244"), usa.automaticSerials)
        assertTrue(europe.title.contains("(Europe)"))
        assertEquals(8, europe.cheatCount)
        assertFalse(europe.relatedSerials.contains("SCUS-94244"))
        assertNotEquals(usa.id, europe.id)
        assertEquals(listOf(usa), index.search("warped usa"))
        ZipFile(File(assets(), "ps1-cheats.zip")).use { zip ->
            val text = zip.getInputStream(zip.getEntry(usa.id + ".cht")).bufferedReader().use { it.readText() }
            val preview = Ps1CheatCatalogue.merge("", text)
            assertEquals(6, preview.addedNames.size)
            assertTrue(preview.addedNames.contains("99 Lives"))
            assertTrue(preview.addedNames.contains("Have All Platinum Relics"))
        }
    }

    @Test fun everyLinkedCatalogueCanBeFoundByItsSerialAcrossTheWholeDatabase() {
        val assets = assets()
        val index = File(assets, "ps1-cheats-index.tsv").reader().use { reader ->
            File(assets, "ps1-cover-titles.tsv").reader().use { metadata -> Ps1CheatDatabaseIndex(reader, metadata) }
        }
        assertTrue(index.knownGameSerialCount > index.coveredGameSerialCount)
        assertTrue(index.coveredGameSerialCount > 2700)
        for (item in index.items) {
            for (serial in item.relatedSerials) {
                assertTrue("$serial / ${item.title}", index.suggestions(serial, null).any { it.id == item.id })
                assertFalse("$serial / ${item.title}", index.isIncompatible(item, serial))
                assertTrue("Formatted search $serial", index.search(serial.replace('-', '_').lowercase()).any { it.id == item.id })
            }
        }
    }

    @Test fun titleAliasesFindAvailableListsWithoutClaimingAutomaticCompatibility() {
        val assets = assets()
        val index = File(assets, "ps1-cheats-index.tsv").reader().use { reader ->
            File(assets, "ps1-cover-titles.tsv").reader().use { metadata -> Ps1CheatDatabaseIndex(reader, metadata) }
        }
        assertTrue(index.suggestions("SLUS-01333", "Top Shop").any { it.title.startsWith("Board Game - Top Shop") })
        assertTrue(index.search("top shop usa").any { it.title.startsWith("Board Game - Top Shop") })
        assertTrue(index.suggestions("SLUS-00439", "Pac-Man World - 20th Anniversary").any { it.title.startsWith("Pac-Man World") })
        assertTrue(index.search("pac man world anniversary usa").any { it.title.startsWith("Pac-Man World") })
        assertTrue(index.suggestions("SCUS-94455", "Gran Turismo 2").isNotEmpty())
        assertNull(index.automatic("SCUS-94455"))
    }
}
