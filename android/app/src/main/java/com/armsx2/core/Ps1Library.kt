package com.armsx2.core

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.net.toUri
import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import java.util.Locale

/** A discovered, launchable game. */
data class Ps1Game(
    val title: String,
    val path: String,
    val name: String = File(path).name,
) {
    val file: File get() = File(path)
}

/**
 * Game discovery: walk the user's chosen POSIX folders plus persisted Storage Access Framework
 * trees, and the app's own external-files dirs (always readable) for bootable PS1 images.
 *
 * "Bootable" is [Ps1Native.isBootable] — `.cue/.bin/.iso/.img/.chd/.zip` and PS-X executables, and
 * deliberately NOTHING else. PS2 container formats (.mdf/.nrg/.cso/.zso/.gz/.dump/.elf/.irx) are
 * not listed anywhere in this app: a PS2 rip must not be able to appear in the library at all.
 * [looksLikeGame] then throws out the files that merely share an extension with a game — a BIOS
 * dump is a `.bin` too — and [Ps1PlaylistVisibility] collapses CUE tracks and M3U disc entries.
 *
 * This is the source the library repository ([com.armsx2.data.library.GameLibraryRepository])
 * builds its `GameInfo` rows from.
 */
object Ps1Library {

    /** Bump to force the home to re-scan (e.g. after adding/removing a library folder). */
    val rescan = mutableIntStateOf(0)

    /** POSIX roots only. `content://` entries are deliberately kept out of java.io.File: resolving
     *  a valid SAF grant to `/storage/...` is exactly what breaks on Android scoped storage. */
    fun scanRoots(
        context: Context,
        directories: List<String> = MainActivityRuntime.romsDirs.value,
    ): List<File> = buildList {
        directories.forEach { raw ->
            when {
                raw.startsWith("content:") -> Unit
                raw.startsWith("file:") -> runCatching { raw.toUri().path }.getOrNull()?.let { add(File(it)) }
                raw.isNotBlank() -> add(File(raw))
            }
        }
        context.getExternalFilesDir(null)?.let { add(File(it, "games")); add(it) }
        add(File(context.filesDir, "games"))
    }.distinct()

    fun scan(
        context: Context,
        directories: List<String> = MainActivityRuntime.romsDirs.value,
    ): List<Ps1Game> {
        val seen = HashSet<String>()
        val out = ArrayList<Ps1Game>()
        val hiddenSafDocuments = HashSet<String>()
        for (root in scanRoots(context, directories)) {
            if (!root.isDirectory) continue
            root.walkTopDown()
                .maxDepth(6)
                .onEnter { it.name.lowercase(Locale.US) !in BLOCKED_DIRS }
                .filter { Ps1Native.isBootable(it) && looksLikeGame(it) }
                .forEach { f ->
                    if (seen.add(f.absolutePath)) {
                        out += Ps1Game(title = f.nameWithoutExtension, path = f.absolutePath)
                    }
                }
        }
        // Resolve CUE and M3U membership together before hiding any metadata
        // entries, so hidden CUEs cannot leave orphan BIN/audio rows.
        val localEntries = out.map { Ps1PlaylistVisibility.Entry(it.path, it.path, it.name) }
        val hiddenLocalDiscs = Ps1PlaylistVisibility.hiddenIds(localEntries) { playlist ->
            runCatching { Ps1PlaylistVisibility.readText(File(playlist.id).inputStream()) }.getOrNull()
        }
        val localGames = out.filterNot { it.path in hiddenLocalDiscs }
        out.clear()
        out.addAll(localGames)
        directories.asSequence()
            .filter { it.startsWith("content:") }
            .distinct()
            .forEach { tree ->
                scanSafTree(context, tree, hiddenSafDocuments).forEach { game ->
                    if (seen.add(game.path)) out += game
                }
            }
        // The same document can be reachable through overlapping tree grants.
        // Keep the granted launch URI, but hide its aliases when another scanned
        // tree established that this exact provider/document belongs to an M3U.
        return out.filterNot { game ->
            game.path.startsWith("content:") && safDocumentKey(Uri.parse(game.path)) in hiddenSafDocuments
        }.sortedBy { it.title.lowercase(Locale.US) }
    }

    /** A disc image below this can't hold a PS1 data track. Filters the BIOS dumps, memory-card
     *  images and stray `.bin` blobs that live in the same folders we scan — [Ps1Native.isBootable]
     *  only knows extensions, and `.bin` is the same extension a BIOS uses. */
    private const val MIN_DISC_BYTES = 1L * 1024 * 1024

    private val SIZED_EXTS = setOf("cue", "bin", "iso", "img", "chd", "zip", "pbp")

    private fun looksLikeGame(file: File): Boolean {
        // Never surface our own app data (bios/, memcards/, savestates/, covers/) as games.
        val lowerPath = file.absolutePath.lowercase(Locale.US)
        if (BLOCKED_DIRS.any { lowerPath.contains("/$it/") }) return false
        val ext = file.extension.lowercase(Locale.US)
        // A .cue is a few hundred bytes of text pointing at the real track — never size-gate it.
        // A .m3u is smaller still: one line per disc. Both would be filtered out by MIN_DISC_BYTES.
        if (ext == "cue" || ext == Ps1Playlist.EXTENSION) return true
        if (ext !in SIZED_EXTS) return true // PS-X executables are legitimately tiny.
        return file.length() >= MIN_DISC_BYTES
    }

    /* The app's own folders, never a place a game lives.
       `resources` earned its place the hard way: we SHIP `resources/patches.zip`, .zip counts as
       bootable, and on an install with no custom data root the folder we copy assets into IS a
       scan root — so the library listed our own patch archive as a game called "patches", even
       when the user's chosen folder was empty. The rest are listed for the same reason: every
       one of them is a directory this app creates and fills (see BackupManager.INCLUDED). */
    private val BLOCKED_DIRS = setOf(
        "bios", "memcards", "savestates", "sstates", "covers", "cache", "resources",
        "patches", "cheats", "snaps", "inputprofiles", "gamesettings", "logs",
        "shaders", "textures", "skins", "saf-launch",
        "migration-backups", "performance", "fixtures",
    )

    private data class SafCandidate(
        val game: Ps1Game,
        val uri: Uri,
        val relativePath: String,
    )

    /** Walk a persisted tree directly through DocumentsProvider. This is the supported Android
     *  11+ path and works for primary storage, SD cards, and USB drives without MANAGE_EXTERNAL_STORAGE. */
    private fun scanSafTree(context: Context, rawTreeUri: String, hiddenDocuments: MutableSet<String>): List<Ps1Game> {
        val treeUri = runCatching { Uri.parse(rawTreeUri) }.getOrNull() ?: return emptyList()
        val rootId = runCatching { DocumentsContract.getTreeDocumentId(treeUri) }.getOrNull()
            ?: return emptyList()

        data class Pending(val documentId: String, val depth: Int, val relativePath: String)
        val pending = ArrayDeque<Pending>()
        val visited = HashSet<String>()
        val candidates = ArrayList<SafCandidate>()
        pending += Pending(rootId, 0, "")

        while (pending.isNotEmpty()) {
            val (directoryId, depth, relativePath) = pending.removeFirst()
            if (!visited.add(directoryId)) continue
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, directoryId)
            // DocumentFile.listFiles() only fetches IDs: name/type/size each trigger another
            // provider call per file. Fetch the complete row once per folder instead.
            runCatching {
                context.contentResolver.query(children, arrayOf(
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE,
                    DocumentsContract.Document.COLUMN_SIZE,
                ), null, null, null)?.use { cursor ->
                    val idColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                    val nameColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                    val typeColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
                    val sizeColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_SIZE)
                    while (cursor.moveToNext()) {
                        val id = cursor.getString(idColumn) ?: continue
                        val name = cursor.getString(nameColumn)?.takeIf(String::isNotBlank) ?: continue
                        val mime = cursor.getString(typeColumn)?.takeIf(String::isNotBlank) ?: continue
                        if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                            if (depth < 6 && name.lowercase(Locale.US) !in BLOCKED_DIRS) {
                                pending += Pending(id, depth + 1, if (relativePath.isEmpty()) name else "$relativePath/$name")
                            }
                            continue
                        }
                        if (!Ps1Native.isBootableName(name)) continue
                        val size = if (sizeColumn < 0 || cursor.isNull(sizeColumn)) 0L else cursor.getLong(sizeColumn)
                        if (!looksLikeGame(name, size)) continue
                        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, id)
                        candidates += SafCandidate(Ps1Game(name.substringBeforeLast('.'), uri.toString(), name),
                            uri, if (relativePath.isEmpty()) name else "$relativePath/$name")
                    }
                }
            }
        }

        val candidatesById = candidates.associateBy { it.game.path }
        val hiddenPlaylistDiscs = Ps1PlaylistVisibility.hiddenIds(candidates.map {
            Ps1PlaylistVisibility.Entry(it.game.path, it.relativePath, it.game.name, ignoreReferenceCase = true)
        }) { playlist ->
            candidatesById[playlist.id]?.let { candidate ->
                runCatching { context.contentResolver.openInputStream(candidate.uri)?.let(Ps1PlaylistVisibility::readText) }
                    .getOrNull()
            }
        }
        candidates.filter { it.game.path in hiddenPlaylistDiscs }.forEach {
            safDocumentKey(it.uri)?.let(hiddenDocuments::add)
        }
        return candidates.asSequence()
            .filterNot { it.game.path in hiddenPlaylistDiscs }
            .map(SafCandidate::game)
            .toList()
    }

    private fun safDocumentKey(uri: Uri): String? = runCatching {
        "${uri.authority}\u0000${DocumentsContract.getDocumentId(uri)}"
    }.getOrNull()

    private fun looksLikeGame(name: String, length: Long): Boolean {
        val ext = Ps1SafText.extension(name)
        if (ext == "cue" || ext == Ps1Playlist.EXTENSION) return true
        if (ext !in SIZED_EXTS) return true
        // Some DocumentsProviders report SIZE_UNKNOWN as 0. Do not hide a real game merely
        // because metadata is absent; only reject a known, non-zero BIOS-sized impostor.
        return length <= 0L || length >= MIN_DISC_BYTES
    }

    /** Absolute paths of every `FILE "…"` line in [cue], resolved next to the cue itself.
     *  Lives in [Ps1DiscId] because identification needs the same answer — following the cue to
     *  its real data track is the FIRST step of reading a serial, and two parsers that disagree
     *  would list a game the identifier cannot then read. */
    fun cueReferencedPaths(cue: File): List<String> = Ps1DiscId.cueReferencedPaths(cue)
}
