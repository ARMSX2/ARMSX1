package com.armsx2.data.library

import android.content.Context
import android.net.Uri
import androidx.core.content.edit
import androidx.core.net.toUri
import com.armsx2.FilenameParser
import com.armsx2.GameInfo
import com.armsx2.GamePlatform
import com.armsx2.core.Ps1Covers
import com.armsx2.core.Ps1Folders
import com.armsx2.core.Ps1Game
import com.armsx2.core.Ps1Library
import com.armsx2.core.Ps1Storage
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * The library, backed by the PS1 core.
 *
 * This used to walk SAF document trees and ask PCSX2's GameDB (over JNI) for each disc's serial,
 * title, region and compatibility. None of that survives the PS1 port: there is no PCSX2 core to
 * ask (`NativeApp` is a shim whose native methods return null/0), and the ARMSX core boots games by
 * **absolute filesystem path** — it cannot open a `content://` document. So the whole scan now runs
 * through the PS1 helpers:
 *
 * ```
 *   MainActivityRuntime.romsDirs      (front-end's folders — SAF tree URIs and/or POSIX paths)
 *        └─ posixRoots()              resolve every entry down to /storage/…
 *             └─ Ps1Folders.syncFromLibrary()  persist + mirror into settings.toml [library].folders
 *                  └─ Ps1Library.scan()          walk for bootable PS1 files
 *                       └─ Ps1DiscId / Ps1Covers  serial from the disc's `cdrom:\SLUS_005.94`
 *                            └─ GameInfo(platform = PS1, uri = file://…)
 * ```
 *
 * The public API is byte-for-byte what the home screen and its view model already call — only the
 * source of the rows changed. Every [GameInfo.uri] is a `file://` URI, so `HomeViewModel.launch`'s
 * existing `uri.path` unwrap hands the core the absolute path it needs.
 */
class GameLibraryRepository(private val context: Context) {

    // Recent-games export runs off the launch/UI thread; exportLock serialises the file
    // write so a quick play-then-remove can't interleave two writers on the same file.
    private val exportScope = CoroutineScope(Dispatchers.IO)
    private val exportLock = Any()

    /** Identity of a scan, so a warm start can tell whether the cached list still applies.
     *  Keyed on the RESOLVED roots, not the raw entries: the same folder reached as a tree URI
     *  and as a POSIX path is one folder, and re-picking it must not force a full rescan. */
    fun cacheKey(directories: List<String>): String = posixRoots(directories).joinToString("|")

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
        // Push the resolved roots down to the PS1 side: Ps1Library scans them, and Ps1Folders
        // mirrors them into settings.toml so the native core's own library agrees with ours.
        runCatching { Ps1Folders.syncFromLibrary(roots) }
        // Serials are read out of the disc image itself (up to 16 MB per game). Re-seed the memo
        // from the previous scan so only genuinely new files pay that cost.
        runCatching {
            loadCached().games.forEach { g -> pathOf(g.uri)?.let { Ps1Covers.prime(it, g.serial) } }
        }

        val collected = linkedMapOf<String, GameInfo>()
        Ps1Library.scan(context).forEach { game ->
            val uri = Uri.fromFile(File(game.path))
            collected.putIfAbsent(uri.toString(), createGame(uri, game))
        }
        collected.values.sortedBy { it.title.lowercase() }.also { saveCache(directories, it) }
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
     * Resolve whatever the front-end stored as a ROM folder — a SAF tree URI, a `file://` URI, or
     * an already-absolute path — to the `/storage/…` path the PS1 core can open. Entries that
     * can't be resolved are dropped rather than passed through, so we never hand the core a
     * `content://` string it would fail to fopen.
     */
    private fun posixRoots(directories: List<String>): List<String> = directories
        .mapNotNull { raw ->
            when {
                raw.isBlank() -> null
                raw.startsWith("content:") -> Ps1Storage.resolveTreeUriToPosix(raw)
                raw.startsWith("file:") -> runCatching { raw.toUri().path }.getOrNull()
                else -> raw
            }
        }
        .filter { it.isNotBlank() }
        .distinct()
        .sorted()

    /** The absolute path behind a library URI (always `file://` since the PS1 rewrite). */
    private fun pathOf(uri: Uri): String? =
        if (uri.scheme == null || uri.scheme == "file") uri.path?.takeIf { it.isNotBlank() } else null

    private fun createGame(uri: Uri, game: Ps1Game): GameInfo {
        val name = File(game.path).name
        val extension = name.substringAfterLast('.', "").lowercase()
        val (fileTitle, fileSerial) = FilenameParser.parse(name)
        // The disc's own boot line beats the filename: a renamed dump still boots the same disc,
        // and psx-covers is keyed by the real serial. Null for .chd/.zip/.exe (Ps1DiscId can't
        // read inside a compressed container) — those fall back to a serial in the filename, and
        // failing that to no serial at all, which is a filename title + placeholder cover.
        val serial = runCatching { Ps1Covers.serialForPath(game.path) }.getOrNull() ?: fileSerial
        // Warm the sibling-cover probe here, on IO, so GameInfo.coverUrl is a pure map lookup
        // by the time the grid composes it.
        runCatching { Ps1Covers.siblingCoverPath(game.path) }
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
        MainActivityRuntime.prefs.edit {
            putString("gamesCacheKey", cacheKey(directories))
                .putString("gamesCache", array.toString())
        }
    }

    data class CachedLibrary(val key: String?, val games: List<GameInfo>)
}
