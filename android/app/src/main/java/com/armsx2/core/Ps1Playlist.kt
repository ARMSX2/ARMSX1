package com.armsx2.core

import java.io.File
import java.util.Locale

/**
 * `.m3u` multi-disc playlists.
 *
 * A large share of the PlayStation library ships on more than one disc, and swapping between them
 * is not optional — Final Fantasy VIII, Metal Gear Solid, Chrono Cross and many others *require*
 * it partway through. Before this, ARMSX1 had no concept of a multi-disc game at all: each disc
 * appeared in the library as a separate, unrelated entry, and changing discs meant leaving the
 * game to hunt for a file.
 *
 * The format is deliberately plain text, one image path per line:
 *
 * ```
 * # Final Fantasy VIII
 * FF8 (Disc 1).cue
 * FF8 (Disc 2).cue
 * FF8 (Disc 3).cue
 * FF8 (Disc 4).cue
 * ```
 *
 * Paths are resolved RELATIVE TO THE PLAYLIST, which is how every other emulator writes them and
 * the only form that survives moving a game folder or copying it to another device. Absolute
 * paths are honoured when given, because some tools emit them.
 *
 * Nothing here reads the discs themselves. A playlist is a list of paths; whether those paths
 * point at anything bootable is the disc layer's problem, and reporting a missing entry honestly
 * beats silently dropping it — a four-disc game that quietly lists three is far more confusing
 * than one that says disc 3 is missing.
 */
object Ps1Playlist {

    const val EXTENSION = "m3u"

    /** One disc in a playlist. [exists] is resolved at parse time for the picker to grey out. */
    data class Disc(
        val index: Int,
        val label: String,
        val file: File,
        val exists: Boolean,
    )

    fun isPlaylist(path: String): Boolean =
        path.substringAfterLast('.', "").lowercase(Locale.US) == EXTENSION

    fun isPlaylist(file: File): Boolean = isPlaylist(file.name)

    /**
     * Parses [playlist] into its discs. Returns an empty list if the file is unreadable or holds
     * no usable entries — callers treat that as "not a playlist" and fall back to launching the
     * file directly, which is the safe outcome for a malformed or empty .m3u.
     */
    fun parse(playlist: File): List<Disc> {
        val lines = runCatching { playlist.readLines() }.getOrNull() ?: return emptyList()
        val parent = playlist.parentFile
        val discs = mutableListOf<Disc>()

        for (raw in lines) {
            val line = raw.trim()

            // '#' is a comment in .m3u, and '#EXTM3U' / '#EXTINF' headers are common in playlists
            // written by media tools. Skipping every '#' line covers all of them.
            if (line.isEmpty() || line.startsWith("#")) continue

            val entry = if (File(line).isAbsolute) File(line) else File(parent, line)

            discs += Disc(
                index = discs.size,
                label = discLabel(entry.name, discs.size),
                file = entry,
                exists = runCatching { entry.isFile }.getOrDefault(false),
            )
        }

        return discs
    }

    /**
     * A short label for the picker. Prefers a "(Disc 2)" / "Disc 2" / "CD2" marker already in the
     * filename, because that is the number printed on the physical disc the player is being asked
     * to insert — falling straight to a positional "Disc 3" would mislabel a playlist whose
     * entries are out of order or which omits a disc.
     */
    private fun discLabel(fileName: String, position: Int): String {
        val match = Regex("""(?i)\b(?:disc|disk|cd)\s*[._-]?\s*(\d{1,2})\b""").find(fileName)
        val number = match?.groupValues?.get(1)?.toIntOrNull()

        return if (number != null) "Disc $number" else "Disc ${position + 1}"
    }

    /**
     * The image a playlist should BOOT. First existing entry rather than simply the first: a
     * playlist whose disc 1 is missing should still start on disc 2 rather than failing outright.
     * Returns null when nothing in the list exists.
     */
    fun bootDisc(playlist: File): File? {
        val discs = parse(playlist)

        return discs.firstOrNull { it.exists }?.file
    }

    /**
     * Resolves what to actually hand the core for [path]. A playlist resolves to its boot disc;
     * anything else is returned unchanged. This is the single call site a launcher needs, so a
     * `.m3u` can be treated as an ordinary library entry everywhere else.
     */
    fun resolveForLaunch(path: String): String {
        if (!isPlaylist(path)) return path

        val file = File(path)

        return bootDisc(file)?.absolutePath ?: path
    }
}
