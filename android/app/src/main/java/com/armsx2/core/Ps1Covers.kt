package com.armsx2.core

import java.io.File
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

/**
 * Resolves box art for a PS1 game, in priority order:
 *  1. A user-supplied sibling image next to the ROM (`<name>.jpg/png/webp`) — always wins.
 *  2. A cover downloaded from the psx-covers project, keyed by the disc serial. Coil handles the
 *     network fetch + disk cache; we only supply the URL. (Needs INTERNET.)
 *  3. null → the caller draws a gradient placeholder tile.
 *
 * Serial extraction ([Ps1DiscId]) reads a chunk off disk, so results are memoised per path.
 */
object Ps1Covers {

    private const val BASE = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default"
    private val COVER_EXTS = listOf("jpg", "jpeg", "png", "webp")
    private val serialCache = ConcurrentHashMap<String, String>()  // path -> serial ("" = none)
    private val siblingCache = ConcurrentHashMap<String, String>() // path -> cover path ("" = none)

    fun coverUrl(serial: String): String = "$BASE/${serial.uppercase(Locale.US)}.jpg"

    /** A Coil-loadable model (File / URL String) or null. Safe to call on an IO dispatcher. */
    fun resolveModel(game: Ps1Game): Any? {
        siblingCover(game.file)?.let { return it }
        val serial = serialFor(game)
        if (serial != null) return coverUrl(serial)
        return null
    }

    fun serialFor(game: Ps1Game): String? = serialForPath(game.path)

    /**
     * [serialFor] keyed by absolute path — what the library repository has (it builds its own
     * [com.armsx2.GameInfo] rows rather than holding onto [Ps1Game]s). Memoised per path, so the
     * up-to-16 MB scan happens once per game per process.
     *
     * **Blocking.** Call from an IO dispatcher.
     */
    fun serialForPath(romPath: String): String? {
        serialCache[romPath]?.let { return it.ifEmpty { null } }

        /*
            A .m3u is a PLAYLIST, not a disc — scanning it for a serial finds nothing, because
            there is nothing in it but file names. Multi-disc entries therefore showed no serial
            at all, which costs them their cover art AND their per-game settings key, since both
            are keyed on it.

            Read the serial off the playlist's FIRST EXISTING DISC instead. Every disc of a game
            carries its own serial (SLUS-00001, -00002 ...), and disc 1's is the one the cover
            repository and RetroAchievements are keyed by, so the playlist inherits it.
        */
        val target = if (Ps1Playlist.isPlaylist(romPath)) {
            Ps1Playlist.bootDisc(File(romPath)) ?: File(romPath)
        } else {
            File(romPath)
        }

        val serial = Ps1DiscId.serialOf(target)
        serialCache[romPath] = serial ?: ""
        return serial
    }

    /**
     * Seed the serial cache from a persisted library scan. Serials never change for a given file,
     * so a warm start must not re-read megabytes off disk for games it already identified.
     */
    fun prime(romPath: String, serial: String?) {
        if (romPath.isBlank() || serial.isNullOrBlank()) return
        serialCache[romPath] = serial
    }

    /**
     * Absolute path of a user-supplied cover sitting next to the ROM (`<name>.jpg/png/webp`), or
     * null. Memoised so the cover getter — which runs during composition — costs at most one stat
     * sweep per game for the life of the process. The library scan warms this on its IO thread.
     */
    fun siblingCoverPath(romPath: String): String? {
        siblingCache[romPath]?.let { return it.ifEmpty { null } }
        val found = siblingCover(File(romPath))?.absolutePath
        siblingCache[romPath] = found ?: ""
        return found
    }

    private fun siblingCover(rom: File): File? {
        val base = rom.absolutePath.substringBeforeLast('.')
        for (ext in COVER_EXTS) {
            val f = File("$base.$ext")
            if (f.isFile) return f
        }
        return null
    }
}
