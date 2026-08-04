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

    /** The memoised probe for [key] ([prime]d or already probed this process), or null. No I/O.
     *  SAF documents key the memo by their content URI string, since they have no POSIX path. */
    fun cachedProbe(key: String): Ps1DiscId.Probe? = probeCache[key]

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

    /* ---------------------------------------------------------------------------------------
       Persistent, pre-fetched covers.

       Coil already fetches a cover the moment a tile scrolls into view, and caches it — but that
       is LAZY: nothing exists until the tile has been on screen, art never appears offline, and a
       cache clear silently loses the lot. Downloading to `<DataRoot>/covers/<SERIAL>.jpg` makes
       covers real files the user owns: visible in the data folder, survive a cache wipe, work
       with no network, and can be hand-replaced.

       `covers` is already in Ps1Library.BLOCKED_DIRS, so these never get scanned back in as games.
       --------------------------------------------------------------------------------------- */

    /** `<DataRoot>/covers`, created on demand. Null when no data root is configured yet. */
    fun coversDir(): File? {
        val root = com.armsx2.runtime.MainActivityRuntime.systemDirPosix() ?: return null
        return File(root, "covers").apply { runCatching { mkdirs() } }
    }

    /** The downloaded cover for [serial], or null if it has not been fetched. */
    fun downloadedCover(serial: String): File? =
        coversDir()?.let { File(it, "${serial.uppercase(Locale.US)}.jpg") }?.takeIf { it.isFile && it.length() > 0 }

    /**
     * Fetch one cover into [coversDir]. Returns true if the file is present afterwards.
     * Already-downloaded covers are a no-op, so this is safe to call repeatedly.
     *
     * **Blocking.** Call from an IO dispatcher.
     */
    fun downloadCover(serial: String): Boolean {
        if (serial.isBlank()) return false
        downloadedCover(serial)?.let { return true }
        val dir = coversDir() ?: return false
        val target = File(dir, "${serial.uppercase(Locale.US)}.jpg")
        // Write to a temp name first: a half-written file that already has the final name would
        // be treated as a valid cover forever after.
        val temp = File(dir, ".${target.name}.part")
        return runCatching {
            val connection = (java.net.URL(coverUrl(serial)).openConnection() as java.net.HttpURLConnection).apply {
                connectTimeout = 15_000
                readTimeout = 15_000
                instanceFollowRedirects = true
            }
            try {
                if (connection.responseCode != 200) return false
                connection.inputStream.use { input -> temp.outputStream().use(input::copyTo) }
            } finally {
                connection.disconnect()
            }
            if (temp.length() <= 0) {
                temp.delete()
                false
            } else {
                temp.renameTo(target) || run { temp.delete(); false }
            }
        }.getOrElse {
            temp.delete()
            false
        }
    }

    /**
     * Download every cover that is missing, one at a time. [serials] is deduplicated and blanks
     * are dropped. [onProgress] fires after each attempt with (done, total) so a UI can show
     * where it is. Returns how many covers were newly fetched.
     *
     * **Blocking.** Call from an IO dispatcher.
     */
    fun downloadMissing(serials: Collection<String>, onProgress: (Int, Int) -> Unit = { _, _ -> }): Int {
        val wanted = serials.mapNotNull { it.takeIf(String::isNotBlank)?.uppercase(Locale.US) }.distinct()
        var fetched = 0
        wanted.forEachIndexed { index, serial ->
            if (downloadedCover(serial) == null && downloadCover(serial)) fetched++
            onProgress(index + 1, wanted.size)
        }
        return fetched
    }
}
