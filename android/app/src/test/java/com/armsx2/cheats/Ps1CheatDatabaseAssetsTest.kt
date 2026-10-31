package com.armsx2.cheats

import java.io.File
import java.util.zip.ZipFile
import org.junit.Assert.*
import org.junit.Test

/** Exercise the real packaged catalogue, not just synthetic index fixtures. */
class Ps1CheatDatabaseAssetsTest {
    @Test fun tonyHawkRetainsUpstreamCheatsWithoutRemovedSupplement() {
        val root = assets()
        val index = File(root, "ps1-cheats-index.tsv").reader().use { reader ->
            File(root, "ps1-cover-titles.tsv").reader().use { Ps1CheatDatabaseIndex(reader, it) }
        }
        val usa = index.automaticCatalogues("SLUS-00860")
        assertEquals(1, usa.size)
        assertEquals(setOf("GameShark"), usa.map { Ps1CheatDatabaseIndex.formatLabel(it.title) }.toSet())
        assertEquals(1, index.automaticCatalogues("SLPM-86429").size)
        assertFalse(index.items.any { it.id == "d35596600326aec0596d" })
        ZipFile(File(root, "ps1-cheats.zip")).use { zip ->
            val bodies = usa.map { item -> Ps1CheatDatabaseIndex.formatLabel(item.title) to
                zip.getInputStream(zip.getEntry(item.id + ".cht")).bufferedReader().use { it.readText() } }
            val merged = Ps1CheatCatalogue.mergeCatalogues("", bodies)
            assertEquals(setOf("Infinite Time", "No Sound FX"), merged.addedNames.toSet())
            assertFalse(merged.text.contains("????"))
            assertFalse(merged.text.contains("@desc Author: ViperByte"))
            assertNull(zip.getEntry("d35596600326aec0596d.cht"))
            assertEquals(0, Ps1CheatCatalogue.mergeCatalogues(merged.text, bodies).added)
        }
    }
    @Test fun realMultipleFormatsCombineWithoutCrossRegionCodesOrRepeatedImports() {
        val root = assets()
        val index = File(root, "ps1-cheats-index.tsv").reader().use { reader ->
            File(root, "ps1-cover-titles.tsv").reader().use { Ps1CheatDatabaseIndex(reader, it) }
        }
        val lists = index.automaticCatalogues("SCES-01420")
        assertEquals(setOf("Game Buster", "GameShark"), lists.map { Ps1CheatDatabaseIndex.formatLabel(it.title) }.toSet())
        assertEquals(1, index.automaticCatalogues("SCUS-94244").size)
        ZipFile(File(root, "ps1-cheats.zip")).use { zip ->
            val bodies = lists.map { item -> Ps1CheatDatabaseIndex.formatLabel(item.title) to
                zip.getInputStream(zip.getEntry(item.id + ".cht")).bufferedReader().use { it.readText() } }
            val merged = Ps1CheatCatalogue.mergeCatalogues("", bodies)
            assertTrue(merged.added >= 8)
            assertEquals(Ps1CheatCatalogue.Merge(merged.text, 0), Ps1CheatCatalogue.mergeCatalogues(merged.text, bodies))
            for ((_, body) in bodies) for (line in body.lineSequence().filter { it.matches(Regex("[0-9A-F]{8} [0-9A-F]{4}")) }) {
                assertTrue(line, merged.text.contains(line))
            }
        }
        for (serial in index.items.flatMap { it.automaticSerials }.distinct()) {
            val returned = index.automaticCatalogues(serial)
            assertEquals(returned.size, returned.map { Ps1CheatDatabaseIndex.formatLabel(it.title).lowercase() }.distinct().size)
            assertTrue(returned.all { serial in it.automaticSerials && !index.isIncompatible(it, serial) })
        }
    }
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
