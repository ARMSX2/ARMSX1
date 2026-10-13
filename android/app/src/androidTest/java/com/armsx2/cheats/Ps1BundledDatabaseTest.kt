package com.armsx2.cheats

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Reads packaged assets directly: no game, original app, imported data or core setup needed. */
@RunWith(AndroidJUnit4::class)
class Ps1BundledDatabaseTest {
    @Test fun tonyHawkUsaPackagedListExcludesRemovedSupplement() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = Ps1CheatDatabase.index(context)
        val matches = index.automaticCatalogues("SLUS-00860")
        assertEquals(1, matches.size)
        assertFalse(index.items.any { it.id == "d35596600326aec0596d" })
        val bodies = Ps1CheatDatabase.catalogues(context, matches)
        val merged = Ps1CheatCatalogue.mergeCatalogues("", bodies)
        assertEquals(setOf("Infinite Time", "No Sound FX"), merged.addedNames.toSet())
        assertEquals(emptyList<String>(), merged.selectionAfterImport(merged.addedNames))
        assertEquals(1, index.automaticCatalogues("SLPM-86429").size)
        assertEquals(0, Ps1CheatCatalogue.mergeCatalogues(merged.text, bodies).added)
    }
    @Test fun packagedFormatsAreReadTogetherAndMergeWithoutEnablingNewCodes() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = Ps1CheatDatabase.index(context)
        val matches = index.automaticCatalogues("SCES-01420")
        assertEquals(setOf("GameShark", "Game Buster"), matches.map { Ps1CheatDatabaseIndex.formatLabel(it.title) }.toSet())
        val catalogues = Ps1CheatDatabase.catalogues(context, matches)
        assertEquals(2, catalogues.size)
        val custom = "[My custom cheat]\n80000100 0001\n"
        val merged = Ps1CheatCatalogue.mergeCatalogues(custom, catalogues)
        assertTrue(merged.text.startsWith(custom))
        assertTrue(merged.added >= 8)
        assertEquals(listOf("My custom cheat"), merged.selectionAfterImport(listOf("My custom cheat") + merged.addedNames))
        assertEquals(0, Ps1CheatCatalogue.mergeCatalogues(merged.text, catalogues).added)
        assertEquals(1, index.automaticCatalogues("SCUS-94244").size)
    }
    @Test fun catalogueAvailableWithoutSelectingOrImportingAGame() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = Ps1CheatDatabase.index(context)
        assertEquals(1664, index.items.size)
        assertEquals(index.items, index.search(""))
        for (item in listOf(index.items.first(), index.items[index.items.size / 2], index.items.last())) {
            val preview = Ps1CheatCatalogue.merge("", Ps1CheatDatabase.catalogue(context, item))
            assertEquals(item.title, item.cheatCount, preview.addedNames.size)
            assertTrue(preview.addedNames.all { it.isNotBlank() })
        }
        assertTrue(index.search("crash bandicoot").isNotEmpty())
    }

    @Test fun usaWarpedIsPackagedAndSelectedByUsaSerialOnly() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = Ps1CheatDatabase.index(context)
        val usa = requireNotNull(index.automatic("SCUS-94244"))
        val europe = requireNotNull(index.automatic("SCES-01420"))
        assertNotEquals(usa.id, europe.id)
        assertTrue(usa.title.contains("(USA)"))
        assertEquals(6, Ps1CheatCatalogue.merge("", Ps1CheatDatabase.catalogue(context, usa)).addedNames.size)
        assertEquals(8, Ps1CheatCatalogue.merge("", Ps1CheatDatabase.catalogue(context, europe)).addedNames.size)
    }

    @Test fun allPackagedSerialMappingsAreDiscoverableWithoutImporting() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val index = Ps1CheatDatabase.index(context)
        assertTrue(index.coveredGameSerialCount > 2700)
        for (item in index.items) for (serial in item.relatedSerials) {
            assertTrue("$serial / ${item.title}", index.suggestions(serial, null).any { it.id == item.id })
        }
    }

    @Test fun selectedGameIdentityUsesDiscContentsNotTheFilename() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val fixture = java.io.File(context.filesDir, "swan-smoke/Crash2.chd")
        org.junit.Assume.assumeTrue("Local user-supplied smoke fixture not present", fixture.isFile)
        assertEquals("SCUS-94154", Ps1CheatIdentity.resolve(context, android.net.Uri.fromFile(fixture).toString()))
        assertNull(Ps1CheatIdentity.resolve(context, android.net.Uri.fromFile(fixture).toString()) { true })
    }
}
