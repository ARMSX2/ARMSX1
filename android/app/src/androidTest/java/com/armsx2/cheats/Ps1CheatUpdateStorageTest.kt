package com.armsx2.cheats

import android.content.Context
import android.content.ContextWrapper
import androidx.test.platform.app.InstrumentationRegistry
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.*
import org.junit.Test
import kotlinx.coroutines.runBlocking

/** Isolated filesDir: never modifies real user databases, cheat files or preferences. */
class Ps1CheatUpdateStorageTest {
    private val app = InstrumentationRegistry.getInstrumentation().targetContext
    private fun isolated(directory: File) = object : ContextWrapper(app) {
        override fun getFilesDir(): File = directory
    }
    private fun packageBytes(): ByteArray {
        val files = Ps1CheatUpdatePackage.FILES.associateWith {
            app.assets.open(it).use { input -> input.readBytes() }
        }.toMutableMap()
        files["format-version.txt"] = "1\n".toByteArray()
        files["hashes.tsv"] = Ps1CheatUpdatePackage.FILES.joinToString("\n") {
            "$it\t${Ps1CheatUpdatePackage.sha256(files.getValue(it))}"
        }.toByteArray()
        return ByteArrayOutputStream().also { bytes ->
            ZipOutputStream(bytes).use { zip ->
                for ((name, body) in files) {
                    zip.putNextEntry(ZipEntry(name))
                    zip.write(body)
                    zip.closeEntry()
                }
            }
        }.toByteArray()
    }
    @Test fun installedUpdatePersistsOfflineAndInvalidReplacementKeepsPreviousData() {
        val root = File(app.cacheDir, "cheat-update-test-${System.nanoTime()}").apply { mkdirs() }
        val context = isolated(File(root, "first").apply { mkdirs() })
        val other = isolated(File(root, "other").apply { mkdirs() })
        try {
            val userFile = File(context.filesDir, "cheats/User.cht").apply {
                parentFile!!.mkdirs(); writeText("[Custom]\n80000100 0001\n")
            }
            val original = userFile.readText()
            val data = packageBytes()
            assertTrue(Ps1CheatDatabase.installUpdate(context, data))
            assertFalse(Ps1CheatDatabase.installUpdate(context, data))
            val before = File(context.filesDir, "cheat-database/update-v1.zip").readBytes()
            try { Ps1CheatDatabase.installUpdate(context, byteArrayOf(1, 2, 3)); fail("Invalid update accepted") }
            catch (_: Exception) { }
            assertArrayEquals(before, File(context.filesDir, "cheat-database/update-v1.zip").readBytes())
            assertEquals(original, userFile.readText())
            Ps1CheatDatabase.index(other) // Drop the in-memory snapshot; simulate a cold/offline load.
            assertEquals(Ps1CheatUpdatePackage.sha256(data).take(12), Ps1CheatDatabase.downloadedRevision(context))
            assertEquals(1664, Ps1CheatDatabase.index(context).items.size)
            assertTrue(Ps1CheatDatabase.catalogue(context,
                Ps1CheatDatabase.index(context).automaticCatalogues("SLUS-00860").single()).contains("[Infinite Time]"))
            assertEquals(original, userFile.readText())
        } finally {
            // Explicit test-created cache directory only; normal user files stay untouched.
            root.deleteRecursively()
            Ps1CheatDatabase.index(app)
        }
    }
    @Test fun corruptCachedDatabaseFallsBackToBundledCatalogue() {
        val root = File(app.cacheDir, "cheat-update-corrupt-test-${System.nanoTime()}").apply { mkdirs() }
        val context: Context = isolated(root)
        try {
            File(root, "cheat-database/update-v1.zip").apply { parentFile!!.mkdirs(); writeBytes(byteArrayOf(1, 2, 3)) }
            assertNull(Ps1CheatDatabase.downloadedRevision(context))
            assertEquals(1664, Ps1CheatDatabase.index(context).items.size)
        } finally {
            root.deleteRecursively()
            Ps1CheatDatabase.index(app)
        }
    }
    /** Explicit opt-in so routine offline instrumentation does not depend on GitHub. */
    @Test fun githubDownloadInstallsOfflineDataWithoutTouchingUserCheats() = runBlocking {
        org.junit.Assume.assumeTrue(InstrumentationRegistry.getArguments().getString("cheatDownloadNetwork") == "true")
        val root = File(app.cacheDir, "cheat-download-network-test-${System.nanoTime()}").apply { mkdirs() }
        val context = isolated(root)
        try {
            val user = File(root, "cheats/User.cht").apply { parentFile!!.mkdirs(); writeText("[Custom]\n80000100 0001\n") }
            val original = user.readText()
            assertTrue(Ps1CheatDownloader.download(context))
            val bytes = File(root, "cheat-database/update-v1.zip").readBytes()
            val update = Ps1CheatUpdatePackage.parse(bytes)
            assertEquals(update.digest.take(12), Ps1CheatDatabase.downloadedRevision(context))
            assertFalse(Ps1CheatDownloader.download(context))
            assertEquals(original, user.readText())
            assertTrue(Ps1CheatDatabase.index(context).items.isNotEmpty())
        } finally {
            root.deleteRecursively()
            Ps1CheatDatabase.index(app)
        }
    }
}
