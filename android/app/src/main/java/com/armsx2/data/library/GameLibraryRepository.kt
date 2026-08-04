package com.armsx2.data.library

import android.content.Context
import android.net.Uri
import androidx.core.content.edit
import androidx.core.net.toUri
import com.armsx2.FilenameParser
import com.armsx2.GameInfo
import com.armsx2.GamePlatform
import com.armsx2.core.Ps1Covers
import com.armsx2.core.Ps1DiscId
import com.armsx2.core.Ps1Folders
import com.armsx2.core.Ps1Game
import com.armsx2.core.Ps1Library
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The library, backed by the PS1 core.
 *
 * SAF trees remain SAF trees on Android 11+: converting a persisted tree grant to `/storage/...`
 * loses the grant at the POSIX boundary and produces an empty library under scoped storage. The
 * scanner therefore enumerates DocumentsProvider directly; launch preparation later exposes the
 * selected seekable document to the unchanged native core through a private descriptor symlink.
 *
 * ```
 *   MainActivityRuntime.romsDirs (persisted SAF trees and/or readable POSIX paths)
 *        └─ Ps1Library.scan()     DocumentsProvider + java.io.File discovery
 *             └─ GameInfo        content:// identity stays intact through cache and launch
 * ```
 *
 * POSIX games can still be identified immediately from their disc. SAF games use their filename
 * identity until launch because probing the native disc reader requires the same descriptor lease
 * as play; the running core supplies the definitive serial once booted.
 */
class GameLibraryRepository(private val context: Context) {

    // Recent-games export runs off the launch/UI thread; exportLock serialises the file
    // write so a quick play-then-remove can't interleave two writers on the same file.
    private val exportScope = CoroutineScope(Dispatchers.IO)
    private val exportLock = Any()

    /** Identity of a scan. SAF identity must remain the granted URI, not a guessed POSIX path. */
    fun cacheKey(directories: List<String>): String = directories.asSequence()
        .map(String::trim)
        .filter(String::isNotEmpty)
        .map { raw ->
            if (raw.startsWith("file:")) runCatching { raw.toUri().path }.getOrNull() ?: raw else raw
        }
        .distinct()
        .sorted()
        .joinToString("|")

    fun loadCached(): CachedLibrary {
        val cachedKey = MainActivityRuntime.prefs.getString("gamesCacheKey", null)
            ?: MainActivityRuntime.prefs.getString("gamesCacheDir", null)
        val json = MainActivityRuntime.prefs.getString("gamesCache", null)
            ?: return CachedLibrary(cachedKey, emptyList())
        val games = runCatching {
            val array = JSONArray(json)
            buildList {
                repeat(array.length()) { index ->
                    val item = array.getJSONObject(index)
                    val uri = item.getString("uri")
                    add(
                        GameInfo(
                            uri = uri.toUri(),
                            title = item.getString("title"),
                            serial = if (item.isNull("serial")) null else item.optString("serial").takeIf(String::isNotBlank),
                            compatibility = item.optInt("compat", 0),
                            extension = item.optString("ext").ifBlank {
                                uri.substringAfterLast('.', "").uppercase()
                            },
                            // Everything in this app is a PS1 disc; a cache written by an older
                            // build may still say "ps2", and fromKey folds that to PS1.
                            platform = GamePlatform.fromKey(item.optString("platform").takeIf(String::isNotBlank)),
                            titleSort = item.optString("titleSort"),
                            titleEn = item.optString("titleEn"),
                        ),
                    )
                }
            }
        }.getOrDefault(emptyList())
        return CachedLibrary(cachedKey, games)
    }

    suspend fun scan(directories: List<String>): List<GameInfo> = withContext(Dispatchers.IO) {
        val roots = posixRoots(directories)
        // Mirror only genuine POSIX roots into settings.toml. SAF tree identity must stay as a
        // content URI; the Compose scanner and per-session descriptor lease own those entries.
        runCatching { Ps1Folders.syncFromLibrary(roots) }
        // Serials are read out of the disc image itself (up to 16 MB per game). Re-seed the memo
        // from the previous scan so only genuinely new files pay that cost.
        runCatching {
            loadCached().games.forEach { g -> pathOf(g.uri)?.let { Ps1Covers.prime(it, g.serial) } }
        }

        val collected = linkedMapOf<String, GameInfo>()
        val probeLog = ArrayList<String>()
        Ps1Library.scan(context, directories).forEach { game ->
            val uri = if (game.path.startsWith("content:")) Uri.parse(game.path)
                else Uri.fromFile(File(game.path))
            if (collected.containsKey(uri.toString())) return@forEach
            collected[uri.toString()] = createGame(uri, game, probeLog)
        }
        writeProbeLog("full scan", probeLog, append = false)
        collected.values.sortedBy { it.title.lowercase() }.also { saveCache(directories, it) }
    }

    /**
     * Re-identify the games the last scan could not put a serial on, and rewrite the cache if any
     * of them now resolve.
     *
     * `serial` is a STORED field: a warm start reads the game list straight out of
     * SharedPreferences and never touches the discs again unless the set of library folders
     * changed. That makes a failed identification permanent — a disc that came back null once
     * stays null through every launch, and through every update that fixes the extractor, because
     * nothing ever asks again. The user's library would keep the blank tile until they thought to
     * hit Refresh.
     *
     * So a null serial is treated as "not determined yet", not as "known to have none". It costs a
     * few kilobytes of disc reads per unidentified game, only for games that HAVE no serial, and
     * only until they get one. Returns the updated list, or null when nothing changed.
     */
    suspend fun retryMissingSerials(games: List<GameInfo>): List<GameInfo>? = withContext(Dispatchers.IO) {
        if (games.none { it.serial.isNullOrBlank() }) return@withContext null
        var changed = false
        val probeLog = ArrayList<String>()
        val updated = games.map { game ->
            if (!game.serial.isNullOrBlank()) return@map game
            val path = pathOf(game.uri)?.takeIf { runCatching { File(it).isFile }.getOrDefault(false) }
                ?: return@map game
            // reprobe, not probe: the process-lifetime memo may already hold this scan's failure.
            val probe = runCatching { Ps1Covers.reprobeForPath(path) }.getOrNull() ?: return@map game
            probeLog += describe(File(path).name, probe)
            val serial = probe.serial?.takeIf { it.isNotBlank() } ?: return@map game
            changed = true
            game.copy(serial = serial)
        }
        writeProbeLog("retry of unidentified discs", probeLog, append = true)
        if (!changed) return@withContext null
        // Only the rows change; the cache KEY still describes the same folders, so it is left
        // exactly as the scan that wrote it left it.
        saveGamesCache(updated)
        updated
    }

    fun recentGames(allGames: List<GameInfo>): List<GameInfo> {
        val raw = MainActivityRuntime.prefs.getString("recentGameUris", null) ?: return emptyList()
        val order = runCatching {
            val array = JSONArray(raw)
            List(array.length()) { array.getString(it) }
        }.getOrDefault(emptyList())
        val byUri = allGames.associateBy { it.uri.toString() }
        return order.mapNotNull(byUri::get)
    }

    fun markPlayed(game: GameInfo) {
        val uri = game.uri.toString()
        val current = runCatching {
            MainActivityRuntime.prefs.getString("recentGameUris", null)?.let(::JSONArray)?.let { array ->
                MutableList(array.length()) { array.getString(it) }
            }
        }.getOrNull() ?: mutableListOf()
        current.remove(uri)
        current.add(0, uri)
        while (current.size > 12) current.removeAt(current.lastIndex)
        MainActivityRuntime.prefs.edit {
            putString(
                "recentGameUris",
                JSONArray(current).toString()
            )
        }
        val snapshot = current.toList()
        exportScope.launch { exportRecentGamesPublic(snapshot, game) }
    }

    /**
     * Drop a single game from Recently Played without touching the library or the
     * global "Show Recently Played" toggle. It naturally returns to the top of the
     * list the next time it's launched (markPlayed re-adds it).
     */
    fun removeFromRecent(game: GameInfo) {
        val uri = game.uri.toString()
        val current = runCatching {
            MainActivityRuntime.prefs.getString("recentGameUris", null)?.let(::JSONArray)?.let { array ->
                MutableList(array.length()) { array.getString(it) }
            }
        }.getOrNull() ?: return
        if (!current.remove(uri)) return
        MainActivityRuntime.prefs.edit {
            putString(
                "recentGameUris",
                JSONArray(current).toString()
            )
        }
        val snapshot = current.toList()
        exportScope.launch { exportRecentGamesPublic(snapshot) }
    }

    /**
     * Empty Recently Played. Same contract as [removeFromRecent], just for every entry: the
     * library and the "Show Recently Played" toggle are untouched, and games reappear as they
     * are launched again. The public export is refreshed so the shelf doesn't come back from
     * the exported copy.
     */
    fun clearRecent() {
        MainActivityRuntime.prefs.edit { remove("recentGameUris") }
        exportScope.launch { exportRecentGamesPublic(emptyList()) }
    }

    /**
     * Mirrors the recently-played list to a plain `recent_games.json` under the app's data
     * root (the shared-storage folder the user picked, next to memcards/; or the app-private
     * externalFilesDir when none was chosen). `recentGameUris` lives in app-private
     * SharedPreferences no other app can read, so this hands companion tools (launchers) the
     * same "recently played" data they already read from that folder. Runs on exportScope (IO)
     * so the cache parse + write never touch the launch/UI thread; exportLock serialises the
     * write. Feature contributed by misantronic (PR #391), reworked here to run off-thread and
     * to also fire on removal.
     */
    private fun exportRecentGamesPublic(orderedUris: List<String>, justPlayed: GameInfo? = null) {
        val root = MainActivityRuntime.systemDirPosix()
            ?: context.getExternalFilesDir(null)?.absolutePath
            ?: return
        val cached = loadCached().games
        val byUri = (if (justPlayed != null) cached + justPlayed else cached).associateBy { it.uri.toString() }
        val array = JSONArray()
        orderedUris.forEach { uriString ->
            val g = byUri[uriString] ?: return@forEach
            array.put(JSONObject().apply {
                put("uri", g.uri.toString())
                put("path", pathOf(g.uri).orEmpty())
                put("title", g.title)
                put("serial", g.serial ?: JSONObject.NULL)
                put("ext", g.extension)
                put("platform", g.platform.key)
            })
        }
        synchronized(exportLock) {
            runCatching { File(root, "recent_games.json").writeText(array.toString()) }
        }
    }

    /**
     * POSIX folders mirrored to settings.toml. SAF roots intentionally stay out of this list:
     * the Compose library owns their persisted grants and the native library scanner cannot use
     * a content URI without a per-session descriptor lease.
     */
    private fun posixRoots(directories: List<String>): List<String> = directories
        .mapNotNull { raw ->
            when {
                raw.isBlank() -> null
                raw.startsWith("content:") -> null
                raw.startsWith("file:") -> runCatching { raw.toUri().path }.getOrNull()
                else -> raw
            }
        }
        .filter { it.isNotBlank() }
        .distinct()
        .sorted()

    /** The absolute path behind a POSIX library URI; SAF documents deliberately return null. */
    private fun pathOf(uri: Uri): String? =
        if (uri.scheme == null || uri.scheme == "file") uri.path?.takeIf { it.isNotBlank() } else null

    private fun createGame(uri: Uri, game: Ps1Game, probeLog: MutableList<String>): GameInfo {
        val name = game.name
        val extension = name.substringAfterLast('.', "").lowercase()
        val (fileTitle, fileSerial) = FilenameParser.parse(name)
        // The disc's own boot line beats the filename: a renamed dump still boots the same disc,
        // and psx-covers is keyed by the real serial. A .chd answers too — Ps1DiscId hands the
        // compressed container to the core's disc reader rather than giving up on it. What is
        // still null is a .zip/.exe and an image whose filesystem is unreadable; those fall back
        // to a serial in the filename, then to a dump-name lookup for the cover only
        // (GameInfo.coverSerial), and failing that to a placeholder tile.
        val posixPath = pathOf(uri)
        val probe = posixPath?.let { runCatching { Ps1Covers.probeForPath(it) }.getOrNull() }
        probe?.let { probeLog += describe(name, it) }
        val serial = probe?.serial ?: fileSerial
        // Warm the sibling-cover probe here, on IO, so GameInfo.coverUrl is a pure map lookup
        // by the time the grid composes it.
        posixPath?.let { runCatching { Ps1Covers.siblingCoverPath(it) } }
        return GameInfo(
            uri = uri,
            // FilenameParser strips dump cruft ("(USA) [!]"); Ps1Game.title is the raw stem and
            // is the fallback for a name that was nothing BUT cruft.
            title = fileTitle.ifBlank { game.title }.ifBlank { name },
            serial = serial,
            // No GameDB in the PS1 port — nothing rates compatibility, so no stars are filled.
            compatibility = 0,
            extension = extension.uppercase(),
            platform = GamePlatform.PS1,
            // Both are GameDB fields (name-sort / name-en). There is no database here, so a
            // filename-derived title sorts and searches as itself.
            titleSort = "",
            titleEn = "",
        )
    }

    private fun saveCache(directories: List<String>, games: List<GameInfo>) {
        MainActivityRuntime.prefs.edit {
            putString("gamesCacheKey", cacheKey(directories))
                .putString("gamesCache", serialise(games))
        }
    }

    /** Rewrite the cached ROWS without touching `gamesCacheKey`. Used by
     *  [retryMissingSerials], which improves the rows for the SAME set of folders — rewriting the
     *  key there would be claiming a scan that never ran. */
    private fun saveGamesCache(games: List<GameInfo>) {
        MainActivityRuntime.prefs.edit { putString("gamesCache", serialise(games)) }
    }

    private fun serialise(games: List<GameInfo>): String {
        val array = JSONArray()
        games.forEach { game ->
            array.put(JSONObject().apply {
                put("uri", game.uri.toString())
                put("title", game.title)
                put("serial", game.serial ?: JSONObject.NULL)
                put("compat", game.compatibility)
                put("ext", game.extension)
                put("platform", game.platform.key)
                put("titleSort", game.titleSort)
                put("titleEn", game.titleEn)
            })
        }
        return array.toString()
    }

    /** One log entry: what the disc resolved to, and the trace of how. */
    private fun describe(name: String, probe: Ps1DiscId.Probe): String = buildString {
        append(name).append(" -> ").append(probe.serial ?: "NO SERIAL")
        if (probe.method.isNotEmpty()) append(" [").append(probe.method).append(']')
        probe.detail.lineSequence().forEach { line ->
            if (line.isNotBlank()) append("\n    ").append(line.trim())
        }
    }

    /**
     * Mirror the identification trace to `serial_probe.log` next to `recent_games.json`.
     *
     * A game with no serial loses its cover, its RetroAchievements identity, its play-time record
     * and its per-game settings key — and before this there was nothing anywhere saying which step
     * of the read failed, so "no cover" was indistinguishable from "never looked". A few KB of
     * plain text, rewritten on each scan, is the difference between diagnosing that and guessing.
     */
    private fun writeProbeLog(reason: String, lines: List<String>, append: Boolean) {
        if (lines.isEmpty()) return
        val root = MainActivityRuntime.systemDirPosix()
            ?: context.getExternalFilesDir(null)?.absolutePath
            ?: return
        val stamp = runCatching {
            SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        }.getOrDefault("")
        val text = buildString {
            append("=== ARMSX disc identification (").append(reason).append(") ")
            append(stamp).append(" ===\n")
            lines.take(MAX_PROBE_LOG_ENTRIES).forEach { append(it).append('\n') }
            if (lines.size > MAX_PROBE_LOG_ENTRIES) {
                append("… ").append(lines.size - MAX_PROBE_LOG_ENTRIES).append(" more\n")
            }
        }
        synchronized(exportLock) {
            runCatching {
                val file = File(root, "serial_probe.log")
                if (append && file.isFile && file.length() < MAX_PROBE_LOG_BYTES) {
                    file.appendText(text)
                } else {
                    file.writeText(text)
                }
            }
        }
    }

    data class CachedLibrary(val key: String?, val games: List<GameInfo>)

    private companion object {
        const val MAX_PROBE_LOG_ENTRIES = 400
        const val MAX_PROBE_LOG_BYTES = 256L * 1024
    }
}
