package com.armsx2.core

import android.content.ContentProvider
import android.content.ContentResolver
import android.content.ContentValues
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ProviderInfo
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Counted-provider scanning tests: no user ROMs or settings touched. */
@RunWith(AndroidJUnit4::class)
class Ps1ScanTest {
    private lateinit var root: File
    private lateinit var context: Context
    private lateinit var provider: FixtureProvider
    private val authority = "armsx.scan.fixture"
    private val tree get() = DocumentsContract.buildTreeDocumentUri(authority, "root")
    private fun uri(id: String) = DocumentsContract.buildDocumentUriUsingTree(tree, id)

    @Before fun setUp() {
        val base = InstrumentationRegistry.getInstrumentation().targetContext
        // The POSIX scanner deliberately rejects anything under cache/ (app data, not ROMs).
        root = File(base.filesDir, "scan-test-${System.nanoTime()}").apply { mkdirs() }
        provider = FixtureProvider()
        provider.attachInfo(base, ProviderInfo().apply { authority = this@Ps1ScanTest.authority })
        val resolver = ContentResolver.wrap(provider)
        context = object : ContextWrapper(base) {
            override fun getContentResolver() = resolver
            override fun getCacheDir() = File(root, "cache").apply { mkdirs() }
            override fun getFilesDir() = File(root, "files").apply { mkdirs() }
            override fun getExternalFilesDir(type: String?) = File(root, "external").apply { mkdirs() }
        }
    }

    @After fun tearDown() { root.deleteRecursively() }

    @Test fun folderMetadataIsBatchedAndCueTracksStillCollapse() {
        repeat(100) { provider.add("root/Game $it.chd", size = 4L * 1024 * 1024) }
        provider.add("root/sub", directory = true)
        provider.add("root/sub/Subgame.iso", size = 4L * 1024 * 1024)
        provider.add("root/covers", directory = true)
        provider.add("root/covers/Not a game.chd", size = 4L * 1024 * 1024)
        provider.add("root/BIOS.bin", size = 512L * 1024)
        provider.add("root/Unknown.chd", size = null)
        provider.add("root/Game.cue", file = File(root, "game.cue").apply {
            writeText("FILE \"Game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n")
        })
        provider.add("root/Game.bin", size = 4L * 1024 * 1024)
        val games = Ps1Library.scan(context, listOf(tree.toString(), tree.toString()))
        assertEquals(103, games.size)
        assertEquals("one query per traversed directory, not per file", 2, provider.queries)
        assertTrue(games.any { it.name == "Game.cue" })
        assertFalse(games.any { it.name == "Game.bin" || it.name == "BIOS.bin" })
    }

    @Test fun localScanSkipsAppDataButKeepsUnknownGames() {
        val folder = File(root, "roms").apply { mkdirs() }
        File(folder, "Homebrew.exe").writeBytes(ByteArray(32))
        File(folder, "covers").mkdirs()
        File(folder, "covers/False game.exe").writeBytes(ByteArray(32))
        assertEquals(listOf("Homebrew.exe"), Ps1Library.scan(context, listOf(folder.absolutePath)).map { it.name })
    }

    private class FixtureProvider : ContentProvider() {
        data class Entry(val id: String, val size: Long?, val directory: Boolean, val file: File?)
        private val entries = ArrayList<Entry>()
        var queries = 0
        var pipe = false
        fun add(id: String, size: Long? = 0, directory: Boolean = false, file: File? = null) {
            entries += Entry(id, file?.length() ?: size, directory, file)
        }
        override fun onCreate() = true
        override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
            selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
            queries++
            val id = DocumentsContract.getDocumentId(uri)
            val columns = projection ?: error("explicit projection required")
            val rows = if (uri.lastPathSegment == "children") entries.filter {
                it.id.substringBeforeLast('/') == id
            } else entries.filter { it.id == id }
            return MatrixCursor(columns).apply {
                rows.forEach { entry -> addRow(columns.map { column -> when (column) {
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID -> entry.id
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME -> entry.id.substringAfterLast('/')
                    DocumentsContract.Document.COLUMN_MIME_TYPE -> if (entry.directory)
                        DocumentsContract.Document.MIME_TYPE_DIR else "application/octet-stream"
                    DocumentsContract.Document.COLUMN_SIZE -> entry.size
                    else -> null
                } }) }
            }
        }
        override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
            check(mode == "r")
            if (pipe) {
                val ends = ParcelFileDescriptor.createPipe()
                ends[1].close()
                return ends[0]
            }
            val file = entries.first { it.id == DocumentsContract.getDocumentId(uri) }.file
                ?: error("fixture has no data")
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
        }
        override fun getType(uri: Uri) = "application/octet-stream"
        override fun insert(uri: Uri, values: ContentValues?): Uri? = error("read only")
        override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = error("read only")
        override fun update(uri: Uri, values: ContentValues?, selection: String?,
            selectionArgs: Array<out String>?) = error("read only")
    }
}
