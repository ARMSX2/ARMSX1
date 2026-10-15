package com.armsx2.core

import java.io.File
import java.io.InputStream
import java.net.URI
import java.util.Locale

/** Library presentation only: never rename, delete, stage or probe disc images. */
internal object Ps1PlaylistVisibility {
    data class Entry(val id: String, val location: String, val name: String,
        val ignoreReferenceCase: Boolean = false)
    private val discExtensions = setOf("cue", "bin", "iso", "img", "chd", "pbp", "zip", "ccd")
    private const val MAX_TEXT = 256 * 1024
    private val discToken = """(?:disc|disk|cd)\s*[._-]?\s*\d{1,2}(?:\s*(?:of|/)\s*\d{1,2})?"""
    private val bracketDisc = Regex("""(?i)\(\s*$discToken\s*\)|\[\s*$discToken\s*]""")
    private val looseDisc = Regex("""(?i)(?<![\p{L}\p{N}])$discToken(?![\p{L}\p{N}])""")
    private val separators = Regex("""[\s._-]+""")

    /** Bounded UTF-8 text, even for short provider reads. Ownership includes closing input. */
    fun readText(input: InputStream): String? = runCatching {
        input.bufferedReader(Charsets.UTF_8).use { reader ->
            val result = StringBuilder()
            val buffer = CharArray(4096)
            while (result.length <= MAX_TEXT) {
                val count = reader.read(buffer, 0, minOf(buffer.size, MAX_TEXT + 1 - result.length))
                if (count < 0) break
                if (count == 0) continue
                result.append(buffer, 0, count)
            }
            result.toString().takeIf { it.length <= MAX_TEXT }
        }
    }.getOrNull()

    fun hiddenIds(entries: List<Entry>, readMetadata: (Entry) -> String?): Set<String> {
        val playlists = entries.filter { Ps1SafText.extension(it.name) == "m3u" }
        val cues = entries.filter { Ps1SafText.extension(it.name) == "cue" }
        if (playlists.isEmpty() && cues.isEmpty()) return emptySet()
        val byLocation = entries.mapNotNull { entry -> normalize(entry.location)?.let { it to entry } }
            .groupBy({ it.first }, { it.second })
            .mapNotNull { (path, matches) -> matches.singleOrNull()?.let { path to it } }.toMap()
        val foldedLocations = entries.mapNotNull { entry ->
            normalize(entry.location)?.lowercase(Locale.US)?.let { it to entry }
        }.groupBy({ it.first }, { it.second })
        fun referenced(owner: Entry, reference: String): Entry? {
            val path = resolve(owner.location, reference) ?: return null
            return byLocation[path] ?: if (owner.ignoreReferenceCase)
                foldedLocations[path.lowercase(Locale.US)]?.singleOrNull() else null
        }
        // Indexed once: a large library must not become playlists × every image.
        val numberedFamilies = entries.filter { isDisc(it) && hasDiscNumber(it.name) }
            .groupBy { parent(it.location) to family(it.name) }
        val hidden = HashSet<String>()
        // Compute track membership before hiding any CUE behind an M3U, including
        // relative track subfolders. Never turn a hidden CUE into orphan BIN rows.
        for (cue in cues) {
            val text = runCatching { readMetadata(cue) }.getOrNull() ?: continue
            if (text.length > MAX_TEXT) continue
            Ps1SafText.cueReferences(text).forEach { reference ->
                referenced(cue, reference)?.takeIf {
                    Ps1SafText.extension(it.name) !in setOf("cue", "m3u")
                }?.let { hidden += it.id }
            }
        }
        for (playlist in playlists) {
            val text = runCatching { readMetadata(playlist) }.getOrNull() ?: continue
            if (text.length > MAX_TEXT) continue
            val members = Ps1SafText.playlistEntries(text).mapNotNull { reference ->
                referenced(playlist, reference)?.takeIf(::isDisc)
            }
            // An empty/broken/unreadable playlist must not make working games disappear.
            if (members.isEmpty()) continue
            members.forEach { hidden += it.id }
            val game = family(playlist.name)
            if (game.isNotEmpty() && members.any { family(it.name) == game }) {
                numberedFamilies[parent(playlist.location) to game].orEmpty().forEach { hidden += it.id }
            }
        }
        return hidden
    }

    private fun isDisc(entry: Entry) = Ps1SafText.extension(entry.name) in discExtensions
    private fun hasDiscNumber(name: String) = bracketDisc.containsMatchIn(name) || looseDisc.containsMatchIn(name)
    private fun family(name: String): String {
        val stem = name.substringBeforeLast('.', name)
        return separators.replace(looseDisc.replace(bracketDisc.replace(stem, " "), " "), " ")
            .trim().lowercase(Locale.US) // Region, revision and language qualifiers stay intact.
    }
    private fun parent(location: String): String {
        val path = normalize(location) ?: return ""
        return if (path.lastIndexOf('/') == 0) "/" else path.substringBeforeLast('/', "")
    }
    private fun resolve(playlist: String, raw: String): String? {
        if (raw.indexOf('\u0000') >= 0) return null
        val reference = if (raw.startsWith("file:", true)) {
            runCatching { File(URI(raw)).path }.getOrNull() ?: return null
        } else raw.replace('\\', '/')
        if (reference.contains("://")) return null
        val absolute = reference.startsWith('/') || Regex("^[A-Za-z]:[/\\\\]").containsMatchIn(reference)
        return normalize(if (absolute) reference else parent(playlist).let {
            if (it.isEmpty()) reference else "$it/$reference"
        })
    }
    private fun normalize(path: String): String? {
        val normalized = path.replace('\\', '/')
        val parts = ArrayList<String>()
        for (part in normalized.split('/')) when (part) {
            "", "." -> Unit
            ".." -> if (parts.isEmpty()) return null else parts.removeAt(parts.lastIndex)
            else -> parts += part
        }
        return (if (normalized.startsWith('/')) "/" else "") + parts.joinToString("/")
    }
}
