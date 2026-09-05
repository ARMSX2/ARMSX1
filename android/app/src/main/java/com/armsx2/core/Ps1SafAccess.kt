package com.armsx2.core

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.system.Os
import android.system.OsConstants
import androidx.documentfile.provider.DocumentFile
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicLong

/**
 * Makes a persisted SAF document look like an ordinary, extension-bearing file to the native core.
 *
 * Android's external-storage DocumentsProvider returns seekable file descriptors, while the core
 * dispatches formats by filename extension and then uses POSIX seek/read. A session therefore keeps
 * each descriptor open and creates a private symlink such as `game.chd -> /proc/self/fd/42`.
 * Nothing is copied, broad storage access is unnecessary, and the native disc readers remain the
 * single implementation of ISO/CHD/PBP/ZIP semantics.
 *
 * CUE sheets are the one multi-file case. Their tiny text is rewritten into the private session
 * directory and each referenced sibling track gets its own descriptor-backed symlink. The session
 * is closed only after runVMThread returns, then its descriptors and private directory are removed.
 */
object Ps1SafAccess {
    private const val MAX_TEXT_BYTES = 512 * 1024
    private const val MAX_PLAYLIST_DEPTH = 4
    private const val SIBLING_LIST_ATTEMPTS = 3
    private const val SIBLING_LIST_RETRY_MS = 200L
    private val sessionIds = AtomicLong()

    class Session internal constructor(
        val launchPath: String,
        private val descriptors: List<ParcelFileDescriptor> = emptyList(),
        private val directory: File? = null,
    ) : Closeable {
        override fun close() {
            descriptors.asReversed().forEach { runCatching { it.close() } }
            directory?.let { runCatching { it.deleteRecursively() } }
        }
    }

    fun prepare(context: Context, pathOrUri: String): Session {
        if (!pathOrUri.startsWith("content://")) return Session(pathOrUri)

        val uri = Uri.parse(pathOrUri)
        val root = File(
            context.cacheDir,
            "saf-launch/${android.os.SystemClock.elapsedRealtimeNanos()}-${sessionIds.incrementAndGet()}",
        )
        if (!root.mkdirs() && !root.isDirectory) {
            throw IOException("Unable to create the private SAF launch directory")
        }

        val held = ArrayList<ParcelFileDescriptor>()
        return try {
            val name = displayName(context, uri)
            val launch = materialize(context.contentResolver, uri, name, root, held, 0)
            Session(launch.absolutePath, held, root)
        } catch (failure: Throwable) {
            held.asReversed().forEach { runCatching { it.close() } }
            runCatching { root.deleteRecursively() }
            throw failure
        }
    }

    private fun materialize(
        resolver: ContentResolver,
        uri: Uri,
        displayName: String,
        root: File,
        held: MutableList<ParcelFileDescriptor>,
        depth: Int,
    ): File {
        if (depth > MAX_PLAYLIST_DEPTH) throw IOException("Nested playlist depth exceeded")
        return when (Ps1SafText.extension(displayName)) {
            "cue" -> materializeCue(resolver, uri, root, held)
            "m3u" -> materializePlaylist(resolver, uri, root, held, depth)
            else -> descriptorLink(resolver, uri, "game.${Ps1SafText.extension(displayName).ifBlank { "bin" }}", root, held)
        }
    }

    private fun materializeCue(
        resolver: ContentResolver,
        cueUri: Uri,
        root: File,
        held: MutableList<ParcelFileDescriptor>,
    ): File {
        val text = readSmallText(resolver, cueUri)
        val references = Ps1SafText.cueReferences(text)
        if (references.isEmpty()) throw IOException("The selected CUE contains no FILE entries")
        val siblings = siblingDocuments(resolver, cueUri)
        val localNames = ArrayList<String>(references.size)

        references.forEachIndexed { index, reference ->
            val requested = Ps1SafText.baseName(reference)
            val sibling = siblings[requested]
                ?: siblings.entries.firstOrNull { it.key.equals(requested, ignoreCase = true) }?.value
                ?: childByName(resolver, cueUri, requested)
                ?: throw IOException("CUE track is not accessible: $requested")
            val extension = Ps1SafText.extension(requested).ifBlank { "bin" }
            val localName = "track-${index.toString().padStart(2, '0')}.$extension"
            descriptorLink(resolver, sibling, localName, root, held)
            localNames += localName
        }

        return File(root, "game.cue").also { it.writeText(Ps1SafText.rewriteCue(text, localNames)) }
    }

    private fun materializePlaylist(
        resolver: ContentResolver,
        playlistUri: Uri,
        root: File,
        held: MutableList<ParcelFileDescriptor>,
        depth: Int,
    ): File {
        val entries = Ps1SafText.playlistEntries(readSmallText(resolver, playlistUri))
        if (entries.isEmpty()) throw IOException("The selected playlist contains no discs")
        val siblings = siblingDocuments(resolver, playlistUri)
        for (entry in entries) {
            val requested = Ps1SafText.baseName(entry)
            val sibling = siblings[requested]
                ?: siblings.entries.firstOrNull { it.key.equals(requested, ignoreCase = true) }?.value
                ?: continue
            return materialize(resolver, sibling, requested, root, held, depth + 1)
        }
        throw IOException("No accessible disc from the selected playlist was found")
    }

    private fun descriptorLink(
        resolver: ContentResolver,
        uri: Uri,
        localName: String,
        root: File,
        held: MutableList<ParcelFileDescriptor>,
    ): File {
        val descriptor = resolver.openFileDescriptor(uri, "r")
            ?: throw IOException("The document provider did not return a file descriptor")
        try {
            // Disc readers require random access. Fail here with a useful launch error rather than
            // letting a pipe-backed cloud provider look like a corrupt ISO later in the core.
            Os.lseek(descriptor.fileDescriptor, 0L, OsConstants.SEEK_SET)
            val link = File(root, localName)
            Os.symlink("/proc/self/fd/${descriptor.fd}", link.absolutePath)
            held += descriptor
            return link
        } catch (failure: Throwable) {
            runCatching { descriptor.close() }
            throw IOException("The selected game is not available as a seekable file", failure)
        }
    }

    private fun readSmallText(resolver: ContentResolver, uri: Uri): String {
        val input = resolver.openInputStream(uri) ?: throw IOException("Unable to read document")
        return input.use {
            val bytes = ByteArray(MAX_TEXT_BYTES + 1)
            var count = 0
            while (count < bytes.size) {
                val read = it.read(bytes, count, bytes.size - count)
                if (read <= 0) break
                count += read
            }
            if (count > MAX_TEXT_BYTES) throw IOException("Playlist/CUE is unexpectedly large")
            bytes.decodeToString(0, count)
        }
    }

    private fun displayName(context: Context, uri: Uri): String {
        val resolver = context.contentResolver
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) cursor.getString(index)?.takeIf(String::isNotBlank)?.let { return it }
            }
        }
        return DocumentFile.fromSingleUri(context, uri)?.name
            ?: uri.lastPathSegment?.substringAfterLast(':')?.substringAfterLast('/')
            ?: "game.bin"
    }

    // Retry null or partial provider results before treating a CUE sibling as missing.
    private fun siblingDocuments(resolver: ContentResolver, documentUri: Uri): Map<String, Uri> {
        val parent = parentDocumentUri(resolver, documentUri)
            ?: throw IOException("The document provider did not expose the CUE/playlist folder")
        val parentId = DocumentsContract.getDocumentId(parent)
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(parent, parentId)
        val result = LinkedHashMap<String, Uri>()
        repeat(SIBLING_LIST_ATTEMPTS) { attempt ->
            val listing = LinkedHashMap<String, Uri>()
            var complete = false
            resolver.query(
                children,
                arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME),
                null,
                null,
                null,
            )?.use { cursor ->
                val idIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                while (cursor.moveToNext()) {
                    val id = cursor.getString(idIndex) ?: continue
                    val name = cursor.getString(nameIndex) ?: continue
                    listing[name] = DocumentsContract.buildDocumentUriUsingTree(parent, id)
                }
                complete = cursor.extras?.getBoolean(DocumentsContract.EXTRA_LOADING, false) != true
            }
            result.putAll(listing)
            if (complete && result.isNotEmpty()) return result
            if (attempt < SIBLING_LIST_ATTEMPTS - 1) android.os.SystemClock.sleep(SIBLING_LIST_RETRY_MS)
        }
        return result
    }

    // Path-based providers can resolve a child omitted from the folder listing.
    private fun childByName(resolver: ContentResolver, documentUri: Uri, name: String): Uri? =
        runCatching {
            val parent = parentDocumentUri(resolver, documentUri) ?: return null
            val childId = DocumentsContract.getDocumentId(parent) + "/" + name
            val child = DocumentsContract.buildDocumentUriUsingTree(parent, childId)
            resolver.query(child, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID), null, null, null)
                ?.use { cursor -> if (cursor.moveToFirst()) child else null }
        }.getOrNull()

    private fun parentDocumentUri(resolver: ContentResolver, uri: Uri): Uri? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            runCatching {
                val path = DocumentsContract.findDocumentPath(resolver, uri)?.path.orEmpty()
                if (path.size >= 2) {
                    return DocumentsContract.buildDocumentUriUsingTree(uri, path[path.lastIndex - 1])
                }
            }
        }
        return runCatching {
            val id = DocumentsContract.getDocumentId(uri)
            val parentId = id.substringBeforeLast('/', missingDelimiterValue = "")
            parentId.takeIf(String::isNotBlank)?.let {
                DocumentsContract.buildDocumentUriUsingTree(uri, it)
            }
        }.getOrNull()
    }
}
