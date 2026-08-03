package com.armsx2.core

import android.content.Context
import androidx.compose.runtime.mutableIntStateOf
import java.io.File
import java.util.Locale

/** A discovered, launchable game. */
data class Ps1Game(
    val title: String,
    val path: String,
) {
    val file: File get() = File(path)
}

/**
 * Game discovery: walk the user's chosen folders ([Ps1Folders]) plus the app's own external-files
 * dirs (always readable, no permission) for bootable PS1 images.
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

    /** Scan roots: the user's chosen library folders ([Ps1Folders], absolute paths — need all-files
     *  access) plus the app's own external-files dirs (always readable, no permission). */
    fun scanRoots(context: Context): List<File> = buildList {
        Ps1Folders.folders.value.forEach { add(File(it)) }
        context.getExternalFilesDir(null)?.let { add(File(it, "games")); add(it) }
        add(File(context.filesDir, "games"))
    }.distinct()

    fun scan(context: Context): List<Ps1Game> {
        Ps1Folders.ensureLoaded()
        val seen = HashSet<String>()
        val out = ArrayList<Ps1Game>()
        for (root in scanRoots(context)) {
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
        return dropCueTracks(out).sortedBy { it.title.lowercase() }
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
