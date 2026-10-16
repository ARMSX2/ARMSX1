package com.armsx2.core

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import androidx.documentfile.provider.DocumentFile
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicLong

/**
 * Copies SAF documents through their granted content URI into a private session directory.
 * Native readers can reopen and seek these regular files without broad storage permission.
 * CUE references are rewritten to copied sibling tracks. Session files are removed after
 * runVMThread returns, including failed launches. Original ROMs are never modified.
 */
object Ps1SafAccess {
    private const val MAX_TEXT_BYTES = 512 * 1024
    private const val MAX_PLAYLIST_DEPTH = 4
    private const val SIBLING_LIST_ATTEMPTS = 3
    private const val SIBLING_LIST_RETRY_MS = 200L
    private val sessionIds = AtomicLong()

    class Session internal constructor(
        val launchPath: String,
        private val directory: File? = null,
        val discs: List<Ps1Playlist.Disc> = emptyList(),
    ) : Closeable {
        override fun close() {
            directory?.let { runCatching { it.deleteRecursively() } }
        }
    }

    fun prepare(context: Context, pathOrUri: String, cancelled: () -> Boolean = { false }): Session =
        prepareSession(context, pathOrUri, true, cancelled)

    /** Hash only the boot image; do not enumerate sibling discs for a picker. */
    fun prepareForHash(context: Context, pathOrUri: String): Session =
        prepareSession(context, pathOrUri, false) { false }

    private fun prepareSession(context: Context, pathOrUri: String, discoverDiscs: Boolean, cancelled: () -> Boolean): Session {
        if (!pathOrUri.startsWith("content://")) return Session(pathOrUri)

        val uri = Uri.parse(pathOrUri)
        val root = File(
            context.cacheDir,
            "saf-launch/${android.os.SystemClock.elapsedRealtimeNanos()}-${sessionIds.incrementAndGet()}",
        )
        if (!root.mkdirs() && !root.isDirectory) {
            throw IOException("Unable to create the private SAF launch directory")
        }

        return try {
            if (cancelled()) throw IOException("Game preparation cancelled")
            val name = displayName(context, uri)
            val discs = if (!discoverDiscs) emptyList() else if (Ps1SafText.extension(name) == "m3u") {
                playlistDiscs(context.contentResolver, uri)
            } else {
                // A single-file grant need not permit sibling listing. Still allow that game.
                runCatching { discsForImage(context.contentResolver, uri, name) }.getOrDefault(emptyList())
            }
            val launch = materialize(context.contentResolver, uri, name, root, 0, discoverDiscs, cancelled)
            Session(launch.absolutePath, root, discs)
        } catch (failure: Throwable) {
            runCatching { root.deleteRecursively() }
            throw failure
        }
    }

    private fun materialize(
        resolver: ContentResolver,
        uri: Uri,
        displayName: String,
        root: File,
        depth: Int,
        includeSbi: Boolean,
        cancelled: () -> Boolean,
    ): File {
        if (cancelled()) throw IOException("Game preparation cancelled")
        if (depth > MAX_PLAYLIST_DEPTH) throw IOException("Nested playlist depth exceeded")
        return when (Ps1SafText.extension(displayName)) {
            "cue" -> materializeCue(resolver, uri, displayName, root, includeSbi, cancelled)
            "m3u" -> materializePlaylist(resolver, uri, root, depth, includeSbi, cancelled)
            else -> copyDocument(resolver, uri, Ps1SafText.launchFileName(uri.toString(), displayName), root, cancelled)
                .also { if (includeSbi) materializeSbi(resolver, uri, displayName, it, cancelled) }
        }
    }

    private fun materializeCue(
        resolver: ContentResolver,
        cueUri: Uri,
        displayName: String,
        root: File,
        includeSbi: Boolean,
        cancelled: () -> Boolean,
    ): File {
        val text = readSmallText(resolver, cueUri)
        val references = Ps1SafText.cueReferences(text)
        if (references.isEmpty()) throw IOException("The selected CUE contains no FILE entries")
        val siblings = siblingDocuments(resolver, cueUri)
        val localNames = ArrayList<String>(references.size)

        references.forEachIndexed { index, reference ->
            val requested = Ps1SafText.baseName(reference)
            val sibling = resolveReference(resolver, cueUri, reference, siblings)
                ?: throw IOException("CUE track is not accessible: $requested")
            val extension = Ps1SafText.extension(requested).ifBlank { "bin" }
            val localName = "track-${index.toString().padStart(2, '0')}.$extension"
            copyDocument(resolver, sibling, localName, root, cancelled)
            localNames += localName
        }

        return File(root, Ps1SafText.launchFileName(cueUri.toString(), "game.cue"))
            .also {
                it.writeText(Ps1SafText.rewriteCue(text, localNames))
                if (includeSbi) materializeSbi(resolver, cueUri, displayName, it, cancelled, siblings)
            }
    }

    private fun materializeSbi(
        resolver: ContentResolver,
        imageUri: Uri,
        imageName: String,
        localImage: File,
        cancelled: () -> Boolean,
        knownSiblings: Map<String, Uri>? = null,
    ) {
        // A single-document grant may not allow listing its parent. Don't break ordinary
        // games in that case; the core still checks protected games for a usable SBI.
        val siblings = knownSiblings ?: runCatching { siblingDocuments(resolver, imageUri) }
            .getOrElse { return }
        val count = Ps1Sbi.stage(imageName, localImage, siblings.keys, { name ->
            resolver.openInputStream(siblings.getValue(name))
                ?: throw IOException("Unable to read SBI file: $name")
        }, cancelled)
        if (count > 0) println("@@ARMSX_SBI_STAGED@@ files=$count")
    }

    private fun materializePlaylist(
        resolver: ContentResolver,
        playlistUri: Uri,
        root: File,
        depth: Int,
        includeSbi: Boolean,
        cancelled: () -> Boolean,
    ): File {
        val entries = Ps1SafText.playlistEntries(readSmallText(resolver, playlistUri))
        if (entries.isEmpty()) throw IOException("The selected playlist contains no discs")
        val siblings = siblingDocuments(resolver, playlistUri)
        for (entry in entries) {
            val requested = Ps1SafText.baseName(entry)
            val sibling = resolveReference(resolver, playlistUri, entry, siblings)
                ?: continue
            return materialize(resolver, sibling, requested, root, depth + 1, includeSbi, cancelled)
        }
        throw IOException("No accessible disc from the selected playlist was found")
    }

    private fun playlistDiscs(resolver: ContentResolver, playlistUri: Uri): List<Ps1Playlist.Disc> =
        Ps1SafText.playlistEntries(readSmallText(resolver, playlistUri)).mapIndexed { index, reference ->
            val name = Ps1SafText.baseName(reference)
            val source = resolveReference(resolver, playlistUri, reference)
            Ps1Playlist.Disc(index, Ps1Playlist.discLabel(name, index), File(name), source != null,
                source?.toString().orEmpty())
        }

    private fun discsForImage(resolver: ContentResolver, image: Uri, name: String): List<Ps1Playlist.Disc> {
        val siblings = siblingDocuments(resolver, image)
        fun sameDocument(a: String, b: Uri): Boolean = runCatching {
            DocumentsContract.getDocumentId(Uri.parse(a)) == DocumentsContract.getDocumentId(b)
        }.getOrDefault(a == b.toString())
        val parent = parentDocumentUri(resolver, image)
        val parentSiblings = parent?.let { runCatching { siblingDocuments(resolver, it) }.getOrNull() }.orEmpty()
        for (candidates in listOf(siblings, parentSiblings)) {
            val playlists = candidates.filterKeys { Ps1SafText.extension(it) == "m3u" }.values
                .mapNotNull { runCatching { playlistDiscs(resolver, it) }.getOrNull() }
                .filter { discs -> discs.any { it.exists && sameDocument(it.path, image) } }
            if (playlists.size == 1) return playlists.single()
            if (playlists.size > 1) return emptyList()
        }
        return Ps1Playlist.siblingDiscNames(name, siblings.keys).mapIndexed { index, sibling ->
            Ps1Playlist.Disc(index, Ps1Playlist.discLabel(sibling, index), File(sibling), true,
                siblings.getValue(sibling).toString())
        }.takeIf { it.size > 1 }.orEmpty()
    }

    private fun copyDocument(
        resolver: ContentResolver,
        uri: Uri,
        localName: String,
        root: File,
        cancelled: () -> Boolean,
    ): File {
        val destination = File(root, localName)
        val partial = File(root, "$localName.part")
        try {
            if (cancelled() || Thread.currentThread().isInterrupted) throw IOException("Game preparation cancelled")
            val required = runCatching {
                resolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
                    val index = cursor.getColumnIndex(OpenableColumns.SIZE)
                    if (cursor.moveToFirst() && index >= 0 && !cursor.isNull(index)) cursor.getLong(index) else -1L
                } ?: -1L
            }.getOrDefault(-1L)
            if (required > 0 && required > root.usableSpace - 16L * 1024 * 1024) {
                throw IOException("Not enough free app storage to stage this game from its document provider")
            }
            val input = resolver.openInputStream(uri)
                ?: throw IOException("The document provider did not return a readable stream")
            input.use { source ->
                partial.outputStream().use { target ->
                    val buffer = ByteArray(256 * 1024)
                    var sinceSpaceCheck = 16L * 1024 * 1024
                    while (true) {
                        if (sinceSpaceCheck >= 16L * 1024 * 1024) {
                            if (root.usableSpace < 16L * 1024 * 1024) {
                                throw IOException("Not enough free app storage to prepare this game")
                            }
                            sinceSpaceCheck = 0
                        }
                        if (cancelled() || Thread.currentThread().isInterrupted) throw IOException("Game preparation cancelled")
                        val count = source.read(buffer)
                        if (count < 0) break
                        target.write(buffer, 0, count)
                        sinceSpaceCheck += count
                    }
                }
            }
            if (partial.length() == 0L || (required >= 0 && partial.length() != required)) {
                throw IOException("The document provider returned an incomplete game file")
            }
            if (cancelled() || Thread.currentThread().isInterrupted) throw IOException("Game preparation cancelled")
            if (!partial.renameTo(destination)) {
                throw IOException("Unable to finish preparing the disc image")
            }
            println("@@ARMSX_SAF_STAGED@@ name=$localName bytes=${destination.length()}")
            return destination
        } catch (failure: Throwable) {
            partial.delete()
            throw IOException("Unable to prepare the selected game: ${failure.message}", failure)
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
        return childDocuments(resolver, parent)
    }

    private fun childDocuments(resolver: ContentResolver, parent: Uri): Map<String, Uri> {
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

    private fun resolveReference(
        resolver: ContentResolver,
        documentUri: Uri,
        reference: String,
        siblings: Map<String, Uri> = emptyMap(),
    ): Uri? {
        if (reference.startsWith("content://")) return Uri.parse(reference)
        val segments = Ps1SafText.relativeSegments(reference)
        var parent = parentDocumentUri(resolver, documentUri) ?: return null
        var children = if (siblings.isEmpty()) childDocuments(resolver, parent) else siblings
        segments.forEachIndexed { index, segment ->
            if (segment == "..") {
                parent = parentDocumentUri(resolver, parent) ?: return null
                children = childDocuments(resolver, parent)
            } else {
                val child = children[segment]
                    ?: children.entries.firstOrNull { it.key.equals(segment, ignoreCase = true) }?.value
                    ?: childByName(resolver, parent, segment, isParent = true)
                    ?: return null
                if (index == segments.lastIndex) return child
                parent = child
                children = childDocuments(resolver, parent)
            }
        }
        return null
    }

    // Path-based providers can resolve a child omitted from the folder listing.
    private fun childByName(resolver: ContentResolver, documentUri: Uri, name: String, isParent: Boolean = false): Uri? =
        runCatching {
            val parent = if (isParent) documentUri else parentDocumentUri(resolver, documentUri) ?: return null
            val parentId = DocumentsContract.getDocumentId(parent)
            val childId = parentId + (if (parentId.endsWith(':')) "" else "/") + name
            val child = DocumentsContract.buildDocumentUriUsingTree(parent, childId)
            resolver.query(child, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID), null, null, null)
                ?.use { cursor -> if (cursor.moveToFirst()) child else null }
        }.getOrNull()

    private fun parentDocumentUri(resolver: ContentResolver, uri: Uri): Uri? {
        // A document picked separately can still use an existing ROM-folder tree grant.
        val treeDocument = runCatching {
            if (DocumentsContract.isTreeUri(uri)) uri else {
                val id = DocumentsContract.getDocumentId(uri)
                val grant = resolver.persistedUriPermissions.filter { permission ->
                    permission.isReadPermission && permission.uri.authority == uri.authority &&
                        DocumentsContract.isTreeUri(permission.uri)
                }.sortedByDescending { DocumentsContract.getTreeDocumentId(it.uri).length }
                    .firstOrNull { permission ->
                        val root = DocumentsContract.getTreeDocumentId(permission.uri)
                        id == root || id.startsWith("$root/") || (root.endsWith(':') && id.startsWith(root))
                    }
                grant?.let { DocumentsContract.buildDocumentUriUsingTree(it.uri, id) } ?: uri
            }
        }.getOrDefault(uri)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            runCatching {
                val path = DocumentsContract.findDocumentPath(resolver, treeDocument)?.path.orEmpty()
                if (path.size >= 2) {
                    return DocumentsContract.buildDocumentUriUsingTree(treeDocument, path[path.lastIndex - 1])
                }
            }
        }
        return runCatching {
            val id = DocumentsContract.getDocumentId(treeDocument)
            val treeRoot = DocumentsContract.getTreeDocumentId(treeDocument)
            if (id == treeRoot) return null
            val parentId = if ('/' in id) id.substringBeforeLast('/')
                else if (':' in id) id.substringBefore(':') + ":" else ""
            parentId.takeIf(String::isNotBlank)?.let {
                DocumentsContract.buildDocumentUriUsingTree(treeDocument, it)
            }
        }.getOrNull()
    }
}
