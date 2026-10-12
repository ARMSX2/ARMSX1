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
}
