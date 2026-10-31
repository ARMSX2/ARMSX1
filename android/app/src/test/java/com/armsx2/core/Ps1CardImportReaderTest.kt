package com.armsx2.core

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

class Ps1CardImportReaderTest {
    @Test fun acceptsRawCardsButRejectsStatesHeadersAndTraversal() {
        val raw = Ps1MemoryCardImage.formatted()
        val reader = Ps1CardImportReader()
        for (name in listOf("Game_1.mcd", "folder/Game_2.mcr", "Game.srm", "Game.mc"))
            assertArrayEquals(raw, reader.read(name, ByteArrayInputStream(raw)))
        for (name in listOf("../Game.mcd", "C:\\Game.mcd", "/Game.mcd", "Game.state"))
            assertNull(reader.read(name, ByteArrayInputStream(raw)))
        assertNull(reader.read("Bad.mcd", ByteArrayInputStream(ByteArray(raw.size))))
        assertNull(reader.read("Header.mcd", ByteArrayInputStream(raw + ByteArray(128))))
    }
    @Test fun limitsExpandedBytesEvenForUnrelatedFiles() {
        val reader = Ps1CardImportReader(maxBytes = 100)
        assertNull(reader.read("readme.txt", ByteArrayInputStream(ByteArray(60))))
        assertThrows(IllegalStateException::class.java) {
            reader.read("readme2.txt", ByteArrayInputStream(ByteArray(60)))
        }
    }
    @Test fun boundsEntryCountAndReadsNestedZipsWithoutTrustingMetadata() {
        val bytes = ByteArrayOutputStream()
        val raw = Ps1MemoryCardImage.formatted()
        ZipOutputStream(bytes).use { zip ->
            listOf("cards/Game_1.mcd", "../escape.mcd", "second.mcd").forEach { name ->
                zip.putNextEntry(ZipEntry(name)); zip.write(raw); zip.closeEntry()
            }
        }
        val reader = Ps1CardImportReader(maxEntries = 2)
        ZipInputStream(ByteArrayInputStream(bytes.toByteArray())).use { zip ->
            assertArrayEquals(raw, reader.read(zip.nextEntry.name, zip)); zip.closeEntry()
            assertNull(reader.read(zip.nextEntry.name, zip)); zip.closeEntry()
            assertThrows(IllegalStateException::class.java) { reader.read(zip.nextEntry.name, zip) }
        }
    }
}
