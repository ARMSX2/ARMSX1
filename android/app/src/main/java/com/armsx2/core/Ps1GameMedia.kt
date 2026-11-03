package com.armsx2.core

import android.util.Log
import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.ConcurrentHashMap

/**
 * Screenshots and fan art for the dual-screen library view.
 *
 * Screenshots come from RetroArch's libretro-thumbnails set, which is keyed by the Redump/No-Intro
 * dump name — the same name most users' files already carry — so no lookup service is involved.
 * Fan art comes from SteamGridDB "heroes", only when the user has entered their own API key.
 */
object Ps1GameMedia {
    private const val TAG = "Ps1GameMedia"
    private const val THUMBNAILS =
        "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master"
    private const val STEAMGRIDDB = "https://www.steamgriddb.com/api/v2"
    private const val KEY_STEAMGRIDDB = "ui.steamGridDbKey"
    private const val KEY_SNAPS = "ui.dualScreenMediaSnaps"
    private const val KEY_TITLES = "ui.dualScreenMediaTitles"
    private const val KEY_FAN_ART = "ui.dualScreenMediaFanArt"
    private const val MAX_FAN_ART = 3

    /** The user's SteamGridDB API key; blank means no fan art is fetched at all. */
    val steamGridDbKey = mutableStateOf("")

    /** Shown under the key field: null when fine or untried. */
    val steamGridDbError = mutableStateOf<String?>(null)

    /** Which kinds of media the slideshow uses; each can be switched off on the Dual Screen tab. */
    val showSnaps = mutableStateOf(true)
    val showTitles = mutableStateOf(true)
    val showFanArt = mutableStateOf(true)

    private val fanArtCache = ConcurrentHashMap<String, List<String>>()

    fun load() {
        val prefs = runCatching { MainActivityRuntime.prefs }.getOrNull() ?: return
        steamGridDbKey.value = runCatching { prefs.getString(KEY_STEAMGRIDDB, "").orEmpty() }.getOrDefault("")
        showSnaps.value = runCatching { prefs.getBoolean(KEY_SNAPS, true) }.getOrDefault(true)
        showTitles.value = runCatching { prefs.getBoolean(KEY_TITLES, true) }.getOrDefault(true)
        showFanArt.value = runCatching { prefs.getBoolean(KEY_FAN_ART, true) }.getOrDefault(true)
    }

    fun setShowSnaps(show: Boolean) = store(showSnaps, KEY_SNAPS, show)
    fun setShowTitles(show: Boolean) = store(showTitles, KEY_TITLES, show)
    fun setShowFanArt(show: Boolean) = store(showFanArt, KEY_FAN_ART, show)

    private fun store(state: androidx.compose.runtime.MutableState<Boolean>, key: String, value: Boolean) {
        state.value = value
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(key, value).apply() }
    }

    fun setSteamGridDbKey(key: String) {
        val trimmed = key.trim()
        steamGridDbKey.value = trimmed
        steamGridDbError.value = null
        fanArtCache.clear()
        runCatching { MainActivityRuntime.prefs.edit().putString(KEY_STEAMGRIDDB, trimmed).apply() }
    }

    /**
     * The libretro-thumbnails file name for a game file: the dump name without its extension, with
     * each character libretro cannot use in a file name (ampersand, asterisk, slash, colon,
     * backtick, angle brackets, question mark, backslash, pipe, double quote) turned into `_`.
     */
    fun libretroName(fileName: String): String =
        fileName.substringBeforeLast('.', fileName).replace(Regex("[&*/:`<>?\\\\|\"]"), "_").trim()

    /** In-game snaps first, then the title screen, for the dump name and its disc-less form. */
    fun snapUrls(fileName: String, snaps: Boolean = true, titles: Boolean = true): List<String> {
        val name = libretroName(fileName).takeIf { it.isNotEmpty() } ?: return emptyList()
        // libretro paths are case-sensitive and use Redump capitalisation; a lowercased file name
        // ("007 - the world is not enough (usa)") also tries its title-cased forms.
        val names = listOf(name, titleCase(name, smallWords = false), titleCase(name, smallWords = true))
            .flatMap { listOf(it, it.replace(Regex("\\s*\\(Disc \\d+\\)"), "")) }
            .distinct()
        val kinds = listOfNotNull("Named_Snaps".takeIf { snaps }, "Named_Titles".takeIf { titles })
        return kinds.flatMap { kind ->
            names.map { "$THUMBNAILS/$kind/${encodeSegment(it)}.png" }
        }
    }

    private val SMALL_WORDS = setOf("a", "an", "and", "at", "by", "for", "in", "of", "on", "or", "the", "to", "vs")
    private val REGION_TAGS = mapOf("usa" to "USA", "eu" to "EU", "uk" to "UK")

    /**
     * Redump-style capitalisation for a name that arrived lowercased: every word capitalised, tags
     * in parentheses capitalised too ("(usa)" → "(USA)"). With [smallWords], articles and short
     * prepositions after the first word stay lower-case, as Redump writes "Lord of the Rings".
     */
    internal fun titleCase(name: String, smallWords: Boolean): String {
        var first = true
        return name.split(' ').joinToString(" ") { word ->
            val bare = word.trimStart('(').trimEnd(')').lowercase()
            val out = when {
                REGION_TAGS.containsKey(bare) -> word.replace(bare, REGION_TAGS.getValue(bare), ignoreCase = true)
                smallWords && !first && bare in SMALL_WORDS && !word.startsWith("(") -> word.lowercase()
                else -> word.replaceFirstChar { if (it.isLowerCase()) it.titlecase() else it.toString() }
                    .let { w -> if (w.startsWith("(") && w.length > 1) "(" + w[1].uppercase() + w.substring(2) else w }
            }
            if (word.isNotBlank() && word != "-") first = false
            out
        }
    }

    /** Search term for SteamGridDB: the title without region / disc / revision tags. */
    fun searchTerm(title: String): String = title.replace(Regex("\\s*\\([^)]*\\)"), "").trim()

    /**
     * SteamGridDB hero image URLs for [title], or empty without a key or on any failure.
     * **Blocking.** Call from an IO dispatcher.
     */
    fun fanArt(title: String): List<String> {
        val key = steamGridDbKey.value
        val term = searchTerm(title)
        if (!showFanArt.value || key.isBlank() || term.isEmpty()) return emptyList()
        fanArtCache[term]?.let { return it }
        val urls = runCatching {
            val search = org.json.JSONObject(get("$STEAMGRIDDB/search/autocomplete/${encodeSegment(term)}", key))
            val gameId = search.optJSONArray("data")?.optJSONObject(0)?.optLong("id", -1L) ?: -1L
            if (gameId < 0) return@runCatching emptyList()
            val heroes = org.json.JSONObject(get("$STEAMGRIDDB/heroes/game/$gameId", key)).optJSONArray("data")
            (0 until (heroes?.length() ?: 0)).mapNotNull { heroes?.optJSONObject(it)?.optString("url") }
                .filter { it.startsWith("https://") }
                .take(MAX_FAN_ART)
        }.onFailure { failure ->
            Log.w(TAG, "SteamGridDB lookup failed: ${failure.message}")
            if (failure is KeyRejected) steamGridDbError.value = "SteamGridDB rejected this key."
        }.getOrDefault(emptyList())
        fanArtCache[term] = urls
        return urls
    }

    private class KeyRejected : Exception("HTTP 401")

    private fun get(url: String, key: String): String {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10_000
            readTimeout = 10_000
            setRequestProperty("Authorization", "Bearer $key")
        }
        try {
            val code = connection.responseCode
            if (code == 401 || code == 403) throw KeyRejected()
            if (code != 200) throw IllegalStateException("HTTP $code")
            steamGridDbError.value = null
            return connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private fun encodeSegment(value: String): String =
        URLEncoder.encode(value, "UTF-8").replace("+", "%20")
}
