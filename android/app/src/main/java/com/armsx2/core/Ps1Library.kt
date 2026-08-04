package com.armsx2.core

import android.content.Context
import android.net.Uri
import androidx.core.net.toUri
import androidx.compose.runtime.mutableIntStateOf
import androidx.documentfile.provider.DocumentFile
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
 * dump is a `.bin` too — and [dropCueTracks] collapses a cue+bin(+audio) rip to its single cue.
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
        for (root in scanRoots(context, directories)) {
            if (!root.isDirectory) continue
            root.walkTopDown()
                .maxDepth(6)
                .filter { Ps1Native.isBootable(it) && looksLikeGame(it) }
                .forEach { f ->
                    if (seen.add(f.absolutePath)) {
                        out += Ps1Game(title = f.nameWithoutExtension, path = f.absolutePath)
                    }
                }
        }
        directories.asSequence()
            .filter { it.startsWith("content:") }
            .forEach { tree ->
                scanSafTree(context, tree).forEach { game ->
                    if (seen.add(game.path)) out += game
                }
            }
        return dropCueTracks(out).sortedBy { it.title.lowercase(Locale.US) }
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

    private val BLOCKED_DIRS = setOf("bios", "memcards", "savestates", "sstates", "covers", "cache")

    private data class SafCandidate(
        val game: Ps1Game,
        val parent: String,
        val uri: Uri,
    )

    /** Walk a persisted tree directly through DocumentsProvider. This is the supported Android
     *  11+ path and works for primary storage, SD cards, and USB drives without MANAGE_EXTERNAL_STORAGE. */
    private fun scanSafTree(context: Context, rawTreeUri: String): List<Ps1Game> {
        val treeUri = runCatching { Uri.parse(rawTreeUri) }.getOrNull() ?: return emptyList()
        val root = runCatching { DocumentFile.fromTreeUri(context, treeUri) }.getOrNull() ?: return emptyList()
        if (!root.isDirectory) return emptyList()

        data class Pending(val directory: DocumentFile, val depth: Int)
        val pending = ArrayDeque<Pending>()
        val candidates = ArrayList<SafCandidate>()
        pending += Pending(root, 0)

        while (pending.isNotEmpty()) {
            val (directory, depth) = pending.removeFirst()
            val children = runCatching { directory.listFiles() }.getOrDefault(emptyArray())
            for (child in children) {
                val name = child.name?.takeIf(String::isNotBlank) ?: continue
                if (child.isDirectory) {
                    if (depth < 6 && name.lowercase(Locale.US) !in BLOCKED_DIRS) {
                        pending += Pending(child, depth + 1)
                    }
                    continue
                }
                if (!child.isFile || !Ps1Native.isBootableName(name)) continue
                if (!looksLikeGame(name, runCatching { child.length() }.getOrDefault(0L))) continue
                val path = child.uri.toString()
                candidates += SafCandidate(
                    game = Ps1Game(name.substringBeforeLast('.'), path, name),
                    parent = directory.uri.toString(),
                    uri = child.uri,
                )
            }
        }

        // A CUE plus its BIN/audio tracks is one game. Read only the tiny CUE sheets and remove
        // exactly the siblings they name; unrelated BIN-only games in the same folder remain.
        val referencedTracks = HashSet<String>()
        candidates.filter { Ps1SafText.extension(it.game.name) == "cue" }.forEach { cue ->
            val text = runCatching {
                context.contentResolver.openInputStream(cue.uri)?.bufferedReader()?.use { reader ->
                    val buffer = CharArray(256 * 1024 + 1)
                    val count = reader.read(buffer)
                    if (count < 0 || count > 256 * 1024) "" else String(buffer, 0, count)
                }.orEmpty()
            }.getOrDefault("")
            Ps1SafText.cueReferences(text).forEach { reference ->
                val base = Ps1SafText.baseName(reference).lowercase(Locale.US)
                referencedTracks += "${cue.parent}\u0000$base"
            }
        }

        return candidates.asSequence()
            .filter { candidate ->
                Ps1SafText.extension(candidate.game.name) == "cue" ||
                    "${candidate.parent}\u0000${candidate.game.name.lowercase(Locale.US)}" !in referencedTracks
            }
            .map(SafCandidate::game)
            .toList()
    }

    private fun looksLikeGame(name: String, length: Long): Boolean {
        val ext = Ps1SafText.extension(name)
        if (ext == "cue" || ext == Ps1Playlist.EXTENSION) return true
        if (ext !in SIZED_EXTS) return true
        // Some DocumentsProviders report SIZE_UNKNOWN as 0. Do not hide a real game merely
        // because metadata is absent; only reject a known, non-zero BIOS-sized impostor.
        return length <= 0L || length >= MIN_DISC_BYTES
    }

    /**
     * A cue/bin rip is TWO bootable-looking files, and the audio tracks of a multi-track rip are
     * several more — listing each one turns one game into five. Where a `.cue` exists it is the
     * canonical entry, so drop every track file it references.
     */
    private fun dropCueTracks(games: List<Ps1Game>): List<Ps1Game> {
        val cues = games.filter { it.path.substringAfterLast('.', "").equals("cue", true) }
        if (cues.isEmpty()) return games
        val cuePaths = cues.mapTo(HashSet()) { it.path }
        val tracks = HashSet<String>()
        for (cue in cues) tracks += cueReferencedPaths(cue.file)
        tracks -= cuePaths // a cue never removes itself
        if (tracks.isEmpty()) return games
        return games.filter { it.path !in tracks }
    }

    /** Absolute paths of every `FILE "…"` line in [cue], resolved next to the cue itself.
     *  Lives in [Ps1DiscId] because identification needs the same answer — following the cue to
     *  its real data track is the FIRST step of reading a serial, and two parsers that disagree
     *  would list a game the identifier cannot then read. */
    fun cueReferencedPaths(cue: File): List<String> = Ps1DiscId.cueReferencedPaths(cue)
}
