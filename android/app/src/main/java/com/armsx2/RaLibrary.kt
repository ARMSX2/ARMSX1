package com.armsx2

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import com.armsx2.achievements.RaLibrarySync
import com.armsx2.achievements.RaGameDetails
import com.armsx2.core.Ps1Playlist
import com.armsx2.core.Ps1SafAccess
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Library-wide progress, matched by rcheevos' PlayStation executable hash, not title/serial.
 * Cache the system catalogue for a week, then fetch the user's complete paginated progress.
 * Manual sync stages SAF documents like launch, one game at a time. Automatic sync only uses
 * cached SAF hashes, so opening the library never copies a ROM collection. Temporary copies
 * are closed/deleted after hashing; original games and their identities are never modified.
 */
object RaLibrary {
    private const val API = "https://retroachievements.org/API"
    private const val CATALOG_FILE = "ra_psx_catalog.json"
    private const val PROGRESS_PAGE = 500
    private const val KEY_WEB_API_KEY = "ra.webApiKey"
    private const val KEY_USER = "ra.webUserName"
    // Old builds recorded failed/zero-readable scans as successful. Ignore that cooldown.
    private const val KEY_LAST_SYNC = "ra.lastLibrarySync.v2"
    private const val KEY_LAST_ATTEMPT = "ra.lastLibraryAttempt.v2"
    private const val KEY_CATALOG_FETCHED = "ra.catalogFetchedAt"
    private const val SYNC_MIN_INTERVAL_MS = 24L * 60 * 60 * 1000
    private const val AUTO_RETRY_INTERVAL_MS = 5L * 60 * 1000
    private const val CATALOG_TTL_MS = 7L * 24 * 60 * 60 * 1000
    private const val DETAILS_TTL_MS = 5L * 60 * 1000
    private val running = AtomicBoolean()

    val syncing = androidx.compose.runtime.mutableStateOf(false)
    val lastResult = androidx.compose.runtime.mutableStateOf("")
    val syncProgress = androidx.compose.runtime.mutableStateOf("")

    var webApiKey: String
        get() = runCatching { MainActivityRuntime.prefs.getString(KEY_WEB_API_KEY, "").orEmpty() }.getOrDefault("")
        set(value) {
            val trimmed = value.trim()
            if (trimmed == webApiKey) return
            MainActivityRuntime.prefs.edit().putString(KEY_WEB_API_KEY, trimmed)
                .remove(KEY_LAST_SYNC).remove(KEY_LAST_ATTEMPT).apply()
        }

    var userName: String
        get() = runCatching { MainActivityRuntime.prefs.getString(KEY_USER, "").orEmpty() }.getOrDefault("")
        set(value) {
            val trimmed = value.trim()
            if (trimmed.isEmpty() || trimmed == userName) return
            MainActivityRuntime.prefs.edit().putString(KEY_USER, trimmed)
                .remove(KEY_LAST_SYNC).remove(KEY_LAST_ATTEMPT).apply()
        }

    fun configured(): Boolean = webApiKey.isNotEmpty() && userName.isNotEmpty()
    @Volatile private var lastKnownGames: List<RaLibrarySync.Entry> = emptyList()

    fun onLibraryLoaded(games: List<GameInfo>, force: Boolean = false) {
        // SAF scanning defers serial detection; those games must still be eligible for sync.
        lastKnownGames = games.mapNotNull { game ->
            DiscIdentity.nativePath(game.uri).takeIf(String::isNotBlank)
                ?.let { RaLibrarySync.Entry(it, game.serial) }
        }.distinctBy { it.path }
        if (!configured() || lastKnownGames.isEmpty()) return
        val last = MainActivityRuntime.prefs.getLong(KEY_LAST_SYNC, 0L)
        if (!force && System.currentTimeMillis() - last < SYNC_MIN_INTERVAL_MS) return
        val attempt = MainActivityRuntime.prefs.getLong(KEY_LAST_ATTEMPT, 0L)
        if (!force && System.currentTimeMillis() - attempt < AUTO_RETRY_INTERVAL_MS) return
        syncInBackground(readDocuments = force)
    }

    /** False only when credentials, native initialization or the library are not ready. */
    fun syncNow(): Boolean {
        val context = MainActivityRuntime.instance?.applicationContext
            ?: return notReady(RaLibrarySync.Readiness.APP_NOT_READY)
        if (MainActivityRuntime.nativeReady.value) {
            val snapshot = runCatching { NativeApp.getAchievementsJSON() }.getOrNull().orEmpty()
            userName = RaLibrarySync.accountForSync(userName, snapshot)
        }
        if (lastKnownGames.isEmpty()) {
            lastKnownGames = runCatching {
                com.armsx2.data.library.GameLibraryRepository(context).loadCached().games.map { game ->
                    RaLibrarySync.Entry(DiscIdentity.nativePath(game.uri), game.serial)
                }.filter { it.path.isNotBlank() }.distinctBy { it.path }
            }.getOrDefault(emptyList())
        }
        return syncInBackground(readDocuments = true)
    }

    private fun notReady(reason: RaLibrarySync.Readiness): Boolean {
        reason.messageKey?.let { lastResult.value = com.armsx2.i18n.I18n.get(it) }
        return false
    }

    /** URI results work before a SAF game's actual serial has been discovered. */
    fun progressFor(game: GameInfo): PlayTime.Progress? {
        return syncedProgressFor(game) ?: PlayTime.achievements(game.serial)
    }

    /** Verified, account/URI-specific results; never infer a match from a shared serial. */
    fun syncedProgressFor(game: GameInfo, account: String = userName): PlayTime.Progress? {
        if (account.isBlank()) return null
        val entry = RaLibrarySync.Entry(DiscIdentity.nativePath(game.uri), game.serial)
        return PlayTime.achievements(entry.progressKey(account))
    }

    /** Keep URI-based web results current when the running game earns another achievement. */
    fun recordLiveProgress(serial: String?, softcore: Int, hardcore: Int, total: Int, account: String? = null) {
        PlayTime.recordAchievements(serial, softcore, hardcore, total)
        val game = MainActivityRuntime.currentGame.value ?: return
        if (!serial.isNullOrBlank() && !game.serial.isNullOrBlank() && serial != game.serial) return
        val user = account?.takeIf(String::isNotBlank) ?: userName.takeIf(String::isNotBlank) ?: return
        val entry = RaLibrarySync.Entry(DiscIdentity.nativePath(game.uri), game.serial)
        PlayTime.recordAchievements(entry.progressKey(user), softcore, hardcore, total)
    }

    private fun syncInBackground(readDocuments: Boolean): Boolean {
        val context = MainActivityRuntime.instance?.applicationContext
        val readiness = RaLibrarySync.readiness(context != null, MainActivityRuntime.nativeReady.value,
            webApiKey, userName, lastKnownGames.size)
        if (readiness != RaLibrarySync.Readiness.READY) return notReady(readiness)
        if (context == null) return notReady(RaLibrarySync.Readiness.APP_NOT_READY)
        if (!running.compareAndSet(false, true)) return true
        val games = lastKnownGames
        val user = userName
        val key = webApiKey
        MainActivityRuntime.prefs.edit().putLong(KEY_LAST_ATTEMPT, System.currentTimeMillis()).apply()
        syncing.value = true
        syncProgress.value = "Fetching RetroAchievements catalogue and progress…"
        kotlin.concurrent.thread(isDaemon = true, name = "ra-library-sync") {
            try {
                val catalog = loadOrFetchCatalog(context, user, key)
                val progress = RaLibrarySync.fetchProgress { offset ->
                    get("$API/API_GetUserCompletionProgress.php?u=${enc(user)}&c=$PROGRESS_PAGE&o=$offset&y=${enc(key)}", "user progress")
                }
                check(user == userName && key == webApiKey) { "Account changed. Please sync again." }
                val result = RaLibrarySync.match(games, catalog, progress,
                    hash = { game ->
                        check(user == userName && key == webApiKey) { "Account changed. Please sync again." }
                        readHash(context, game, readDocuments)
                    },
                    record = { game, earned, total ->
                        if (user == userName && key == webApiKey)
                            PlayTime.recordAchievements(game.progressKey(user), earned.softcore, earned.hardcore, total)
                    },
                    onProgress = { done, total -> syncProgress.value = "Checking discs: $done / $total" },
                    onIdentified = { game, id ->
                        if (user == userName && key == webApiKey) MainActivityRuntime.prefs.edit()
                            .putInt("ra.gameId.${game.progressKey(user)}", id).apply()
                    },
                )
                if (user == userName && key == webApiKey) {
                    if (result.complete) MainActivityRuntime.prefs.edit()
                        .putLong(KEY_LAST_SYNC, System.currentTimeMillis()).apply()
                    lastResult.value = result.description()
                } else lastResult.value = "Account changed. Please sync again."
            } catch (failure: Exception) {
                lastResult.value = failure.message ?: "RetroAchievements sync failed. Please try again."
            } finally {
                syncProgress.value = ""
                syncing.value = false
                running.set(false)
            }
        }
        return true
    }

    private fun loadOrFetchCatalog(context: Context, user: String, key: String): RaLibrarySync.Catalog {
        val file = File(MainActivityRuntime.assetCopyRoot(context), CATALOG_FILE)
        val cached = runCatching { RaLibrarySync.parseCatalog(file.readText()) }.getOrNull()
        val fetched = MainActivityRuntime.prefs.getLong(KEY_CATALOG_FETCHED, 0L)
        if (cached != null && System.currentTimeMillis() - fetched < CATALOG_TTL_MS) return cached
        val body = try {
            get("$API/API_GetGameList.php?i=${RaLibrarySync.CONSOLE_ID}&f=1&h=1&z=${enc(user)}&y=${enc(key)}", "game catalogue")
        } catch (failure: IOException) {
            return cached ?: throw failure
        }
        val catalog = RaLibrarySync.parseCatalog(body)
        runCatching {
            val temp = File(file.parentFile, "${file.name}.part")
            temp.writeText(body)
            if (temp.renameTo(file)) MainActivityRuntime.prefs.edit()
                .putLong(KEY_CATALOG_FETCHED, System.currentTimeMillis()).apply()
            else temp.delete()
        }
        return catalog
    }

    private fun readHash(context: Context, game: RaLibrarySync.Entry, readDocuments: Boolean): RaLibrarySync.HashResult {
        val cacheKey = runCatching { hashCacheKey(context, game.path) }.getOrNull()
        val cached = cacheKey?.let { RaLibrarySync.normaliseHash(MainActivityRuntime.prefs.getString(it, null)) }
        if (cached != null) return RaLibrarySync.HashResult(cached)
        if (game.path.startsWith("content://") && !readDocuments) return RaLibrarySync.HashResult(deferred = true)
        val hash = runCatching {
            // The core cannot open content:// directly. Keep launch's safe temporary files alive
            // until the synchronous, independent CDImage reader has finished the hash.
            Ps1SafAccess.prepareForHash(context, game.path).use { session ->
                val path = Ps1Playlist.resolveForLaunch(session.launchPath)
                RaLibrarySync.normaliseHash(NativeApp.getAchievementsHashForPath(path))
            }
        }.getOrNull()
        if (hash != null && cacheKey != null) MainActivityRuntime.prefs.edit().putString(cacheKey, hash).apply()
        return RaLibrarySync.HashResult(hash)
    }

    private fun hashCacheKey(context: Context, path: String): String? {
        if (!path.startsWith("content://")) {
            val file = File(Ps1Playlist.resolveForLaunch(path))
            if (!file.isFile || file.extension.equals("cue", true)) return null
            return RaLibrarySync.hashCacheKey("$path\n${file.canonicalPath}", file.length(), file.lastModified())
        }
        return context.contentResolver.query(Uri.parse(path),
            arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED),
            null, null, null)?.use { cursor ->
            if (!cursor.moveToFirst()) return@use null
            val nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE)
            val modifiedColumn = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_LAST_MODIFIED)
            val ext = if (nameColumn >= 0) cursor.getString(nameColumn)?.substringAfterLast('.')?.lowercase() else null
            // Missing provider revisions or CUE/M3U references cannot safely validate a cache.
            if (ext == null || ext in setOf("cue", "m3u") || sizeColumn < 0 || modifiedColumn < 0 ||
                cursor.isNull(sizeColumn) || cursor.isNull(modifiedColumn)) return@use null
            val size = cursor.getLong(sizeColumn)
            val modified = cursor.getLong(modifiedColumn)
            if (size <= 0 || modified <= 0) null else RaLibrarySync.hashCacheKey(path, size, modified)
        }
    }

    private fun enc(value: String): String = java.net.URLEncoder.encode(value, "UTF-8")

    /** Blocking, selected-game request only. Old syncs recover their ID from the exact cached hash. */
    internal fun loadGameDetails(context: Context, game: GameInfo, account: String, refresh: Boolean): RaGameDetails.Loaded {
        val key = webApiKey
        check(account.isNotBlank() && account == userName) { "Account changed. Reopen this game to load its achievements." }
        check(key.isNotBlank()) { "Enter your RetroAchievements web API key to view achievement details." }
        val entry = RaLibrarySync.Entry(DiscIdentity.nativePath(game.uri), game.serial)
        val identityKey = "ra.gameId.${entry.progressKey(account)}"
        var id = MainActivityRuntime.prefs.getInt(identityKey, 0)
        if (id <= 0) {
            // A title/serial guess is never an achievement ID. Uncached older CUE/M3U syncs may
            // require a one-time selected-image read, but no game is started and no full resync runs.
            val hash = readHash(context, entry, readDocuments = true).hash
                ?: error("Could not identify this image. Check that its ROM folder is still accessible.")
            id = loadOrFetchCatalog(context, account, key).hashToGame[hash]
                ?: error("This image's exact hash is not in the PlayStation achievement catalogue.")
            MainActivityRuntime.prefs.edit().putInt(identityKey, id).apply()
        }
        val folder = File(context.filesDir, "ra-game-details/${RaLibrarySync.hashCacheKey(account, 0, 0)}")
        val file = File(folder, "$id.json")
        val cached = runCatching { RaGameDetails.parse(file.readText(), id) }.getOrNull()
        if (!refresh && cached != null && System.currentTimeMillis() - file.lastModified() < DETAILS_TTL_MS)
            return RaGameDetails.Loaded(cached, saved = true)
        val body = try {
            get("$API/API_GetGameInfoAndUserProgress.php?g=$id&u=${enc(account)}&y=${enc(key)}", "game achievements")
        } catch (failure: IOException) {
            if (cached != null) return RaGameDetails.Loaded(cached, saved = true, stale = true)
            throw failure
        }
        val parsed = RaGameDetails.parse(body, id)
        check(account == userName && key == webApiKey) { "Account changed. Reopen this game's achievements." }
        runCatching {
            folder.mkdirs()
            val temp = File(folder, "$id.json.part")
            temp.writeText(body)
            if (!temp.renameTo(file)) temp.delete()
        }
        return RaGameDetails.Loaded(parsed)
    }

    private fun get(url: String, operation: String): String {
        val connection = URL(url).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = "GET"
            connection.connectTimeout = 15_000
            connection.readTimeout = 30_000
            connection.setRequestProperty("Accept", "application/json")
            val code = connection.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw IOException(when (code) {
                401, 403 -> "RetroAchievements rejected the request. Check your web API key."
                429 -> "RetroAchievements rate limit reached. Please try again later."
                else -> "RetroAchievements $operation request failed (HTTP $code)."
            })
            return connection.inputStream.bufferedReader().use { reader ->
                val body = StringBuilder()
                val buffer = CharArray(16 * 1024)
                while (true) {
                    val count = reader.read(buffer)
                    if (count < 0) break
                    if (body.length + count > 64 * 1024 * 1024) throw IOException("RetroAchievements response exceeded the size limit.")
                    body.append(buffer, 0, count)
                }
                body.toString()
            }
        } catch (failure: IOException) {
            // HttpURLConnection errors can contain the URL and API key; do not expose those.
            if (failure.message?.startsWith("RetroAchievements ") == true) throw failure
            throw IOException("RetroAchievements $operation network error. Please check your connection and try again.")
        } finally {
            connection.disconnect()
        }
    }
}
