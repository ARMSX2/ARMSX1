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
    private val probeCache = ConcurrentHashMap<String, Ps1DiscId.Probe>() // path -> identification
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
     * disc read happens once per game per process.
     *
     * **Blocking.** Call from an IO dispatcher.
     */
    fun serialForPath(romPath: String): String? = probeForPath(romPath).serial

    /**
     * [serialForPath] with the trace of how identification went — which sector layout was
     * detected, where SYSTEM.CNF lives, what its BOOT line said. The library scan writes these to
     * `serial_probe.log` so a disc that comes back with no serial says why, instead of leaving a
     * blank tile and no evidence.
     */
    fun probeForPath(romPath: String): Ps1DiscId.Probe {
        probeCache[romPath]?.let { return it }

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

        val probe = Ps1DiscId.probe(target)
        probeCache[romPath] = probe
        return probe
    }

    /**
     * Identify [romPath] again, discarding any memoised result.
     *
     * A FAILED identification must never be treated as final. The memo above is a
     * process-lifetime cache of "we already looked", which is right for a serial that was found
     * (it cannot change) but wrong for one that was not: a disc can fail to identify because the
     * storage volume was not mounted yet, because the extractor had a bug, or — the case this was
     * written for — because a build shipped an extractor that could not read that particular
     * layout. Those all become permanent if "no serial" is cached as "known to have none".
     */
    fun reprobeForPath(romPath: String): Ps1DiscId.Probe {
        probeCache.remove(romPath)
        return probeForPath(romPath)
    }

    /**
     * Seed the serial cache from a persisted library scan. Serials never change for a given file,
     * so a warm start must not re-read the disc for games it already identified.
     *
     * A null/blank serial is deliberately NOT seeded: seeding it would turn "we failed to identify
     * this once" into "this disc has no serial" for the rest of the process, which is exactly the
     * trap described on [reprobeForPath].
     */
    fun prime(romPath: String, serial: String?) {
        if (romPath.isBlank() || serial.isNullOrBlank()) return
        probeCache[romPath] = Ps1DiscId.Probe(serial, "cache", "seeded from the previous scan")
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
