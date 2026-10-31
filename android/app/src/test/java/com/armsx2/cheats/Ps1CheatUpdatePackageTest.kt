package com.armsx2.cheats

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.*
import org.junit.Test

class Ps1CheatUpdatePackageTest {
    private val id = "a".repeat(20)
    private val body = "[Health]\n@desc Test\n80000100 0064\n"
    private fun zip(files: List<Pair<String, ByteArray>>): ByteArray = ByteArrayOutputStream().also { bytes ->
        ZipOutputStream(bytes).use { archive ->
            for ((name, data) in files) {
                archive.putNextEntry(ZipEntry(name))
                archive.write(data)
                archive.closeEntry()
            }
        }
    }.toByteArray()
    private fun payload() = Ps1CheatUpdatePackage.FILES.associateWith { name -> when (name) {
        "ps1-cheats.zip" -> zip(listOf("$id.cht" to body.toByteArray()))
        "ps1-cheats-index.tsv" -> "$id\tGame (USA) (GameShark)\tSLUS-00860\tSLUS-00860\t1\t0\n".toByteArray()
        "ps1-cover-titles.tsv" -> "Game (USA)\tUSA\tSLUS-00860\n".toByteArray()
        else -> "License / original author notice\n".toByteArray()
    } }.toMutableMap()
    private fun pack(files: Map<String, ByteArray>, version: String = "1"): ByteArray = zip(
        files.entries.map { it.key to it.value } + listOf(
            "format-version.txt" to version.toByteArray(),
            "hashes.tsv" to files.entries.joinToString("\n") { "${it.key}\t${Ps1CheatUpdatePackage.sha256(it.value)}" }.toByteArray()))
    private fun rejects(data: ByteArray) {
        try { Ps1CheatUpdatePackage.parse(data); fail("Invalid package accepted") }
        catch (_: Exception) { }
    }

    @Test fun validPackageSupportsExactSerialLookupAndPreservesAttribution() {
        val data = pack(payload())
        val update = Ps1CheatUpdatePackage.parse(data)
        assertEquals(body, update.bodies[id])
        assertEquals(id, update.index.automaticCatalogues("SLUS-00860").single().id)
        assertTrue(update.index.automaticCatalogues("SLES-00860").isEmpty())
        assertEquals(Ps1CheatUpdatePackage.sha256(data), update.digest)
        assertTrue(update.notice.contains("original author"))
    }
    @Test fun currentPublishedPackageMatchesBundledData() {
        val root = generateSequence(File(requireNotNull(System.getProperty("user.dir")))) { it.parentFile }
            .first { File(it, "cheats/ps1-cheats-update.zip").isFile }
        val update = Ps1CheatUpdatePackage.parse(File(root, "cheats/ps1-cheats-update.zip").readBytes())
        val assets = File(root, "android/app/src/main/assets")
        val bundled = File(assets, "ps1-cheats-index.tsv").reader().use(::Ps1CheatDatabaseIndex)
        assertEquals(bundled.items, update.index.items)
        assertEquals(1664, update.bodies.size)
        assertFalse(update.bodies.containsKey("d35596600326aec0596d"))
    }
    @Test fun corruptMissingUnknownAndFuturePackagesAreRejected() {
        rejects(byteArrayOf(1, 2, 3))
        rejects(pack(payload(), "2"))
        rejects(pack(payload().apply { remove("ps1-cheats-LICENSE.txt") }))
        rejects(pack(payload().apply { put("../../outside", byteArrayOf(1)) }))
        val files = payload()
        val good = files.entries.map { it.key to it.value }
        rejects(zip(good + listOf("format-version.txt" to "1".toByteArray(),
            "hashes.tsv" to files.keys.joinToString("\n") { "$it\t${"0".repeat(64)}" }.toByteArray())))
    }
    @Test fun unsafeOrIncompleteInnerArchiveIsRejected() {
        for (name in listOf("../$id.cht", "unknown.cht", "folder/$id.cht")) {
            rejects(pack(payload().apply { put("ps1-cheats.zip", zip(listOf(name to body.toByteArray()))) }))
        }
        rejects(pack(payload().apply { put("ps1-cheats.zip", zip(emptyList())) }))
        rejects(pack(payload().apply { put("ps1-cheats.zip", zip(listOf("$id.cht" to ByteArray(2_000_001) { 65 }))) }))
    }
    @Test fun invalidIndexMetadataUtf8AndCountAreRejected() {
        for (text in listOf("invalid", "$id\tGame\tBAD\t\t1\t0", "$id\tGame\t\t\t2\t0",
            "$id\tGame\t\t\t1\t0\n$id\tGame\t\t\t1\t0")) {
            rejects(pack(payload().apply { put("ps1-cheats-index.tsv", text.toByteArray()) }))
        }
        rejects(pack(payload().apply { put("ps1-cover-titles.tsv", "Game\tUSA\tBAD".toByteArray()) }))
        rejects(pack(payload().apply { put("ps1-cheats-NOTICE.txt", byteArrayOf(0xC3.toByte(), 0x28)) }))
    }
    @Test fun sizeLimitsAndCancellationAreEnforced() {
        try { Ps1CheatUpdatePackage.readBounded(ByteArrayInputStream(ByteArray(5)), 4); fail() }
        catch (_: IllegalArgumentException) { }
        try {
            Ps1CheatUpdatePackage.readBounded(ByteArrayInputStream(ByteArray(5)), 5) { throw java.util.concurrent.CancellationException() }
            fail()
        } catch (_: java.util.concurrent.CancellationException) { }
        rejects(ByteArray(Ps1CheatUpdatePackage.MAX_DOWNLOAD + 1))
    }
    @Test fun downloadedCodesMergeWithoutArmingOrReplacingCustomCheats() {
        val update = Ps1CheatUpdatePackage.parse(pack(payload()))
        val custom = "[Custom]\n80000200 0001\n"
        val merge = Ps1CheatCatalogue.mergeCatalogues(custom, listOf("GameShark" to update.bodies.getValue(id)))
        assertTrue(merge.text.startsWith(custom))
        assertEquals(listOf("Custom"), merge.selectionAfterImport(listOf("Custom", "Health")))
        assertEquals(0, Ps1CheatCatalogue.mergeCatalogues(merge.text, listOf("GameShark" to body)).added)
    }
}
