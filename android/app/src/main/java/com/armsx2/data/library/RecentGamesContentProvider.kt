package com.armsx2.data.library

import android.content.ContentProvider
import android.content.ContentValues
import android.content.SharedPreferences
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.Bundle
import org.json.JSONArray

/** Read-only, opt-in access without requiring the main activity to be initialized. */
class RecentGamesContentProvider : ContentProvider() {

    companion object {
        private const val KEY_GAMES_CACHE = "gamesCache"
        private const val KEY_RECENT_URIS = "recentGameUris"
        private const val LAST_PLAYED_PREFIX = "playtime.last."
        private const val MAX_RECENT_LOOKUP = 12

        const val KEY_SHARE_ENABLED = "library.shareRecentGames"

        const val EXTRA_ACCESS_DENIED = "com.armsx2.extra.RECENT_GAMES_ACCESS_DENIED"

        const val EXTRA_CONSENT_DECLINED = "com.armsx2.extra.RECENT_GAMES_CONSENT_DECLINED"

        const val PATH_GAMES = "games"
        const val COLUMN_URI = "uri"
        const val COLUMN_TITLE = "title"
        const val COLUMN_SERIAL = "serial"
        const val COLUMN_EXT = "ext"
        const val COLUMN_PLATFORM = "platform"
        const val COLUMN_LAST_PLAYED = "lastPlayed"

    }

    override fun onCreate(): Boolean = true

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        requireGamesUri(uri)
        RecentGamesQuery.validateArguments(selection, selectionArgs, sortOrder)
        val columns = RecentGamesQuery.columns(projection)
        val cursor = MatrixCursor(columns)
        val prefs = RecentGamesAccess.prefs(requireNotNull(context))

        // Android verifies that callingPackage belongs to the Binder caller.
        val allowed = prefs.getBoolean(KEY_SHARE_ENABLED, false) ||
            RecentGamesAccess.isGranted(prefs, callingPackage)
        if (!allowed) {
            cursor.extras = Bundle().apply {
                putBoolean(EXTRA_ACCESS_DENIED, true)
                putBoolean(EXTRA_CONSENT_DECLINED, RecentGamesAccess.isDeclined(prefs, callingPackage))
            }
            return cursor
        }

        val recentUris = readRecentUris(prefs)
        if (recentUris.isEmpty()) {
            return cursor
        }

        val gamesByUri = readGamesCache(prefs)
        recentUris.take(MAX_RECENT_LOOKUP).forEach { uriString ->
            val game = gamesByUri[uriString] ?: return@forEach
            val lastPlayed = game.serial
                ?.let { serial -> prefs.getLong(LAST_PLAYED_PREFIX + serial, 0L) }
                ?.takeIf { it > 0L }

            cursor.addRow(columns.map { column ->
                when (column) {
                    COLUMN_URI -> game.uri
                    COLUMN_TITLE -> game.title
                    COLUMN_SERIAL -> game.serial
                    COLUMN_EXT -> game.ext
                    COLUMN_PLATFORM -> game.platform
                    COLUMN_LAST_PLAYED -> lastPlayed
                    else -> error("Unsupported recent games column")
                }
            })
        }

        return cursor
    }

    override fun getType(uri: Uri): String {
        requireGamesUri(uri)
        return "vnd.android.cursor.dir/vnd.${uri.authority}.games"
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri =
        throw UnsupportedOperationException("Recent games are read-only")

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int =
        throw UnsupportedOperationException("Recent games are read-only")

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int =
        throw UnsupportedOperationException("Recent games are read-only")

    private fun requireGamesUri(uri: Uri) {
        val packageName = requireNotNull(context).packageName
        require(RecentGamesQuery.matches(uri.scheme, uri.authority, uri.path, packageName)) {
            "Unsupported recent games URI"
        }
    }

    private data class CachedGame(val uri: String, val title: String, val serial: String?, val ext: String, val platform: String)

    private fun readRecentUris(prefs: SharedPreferences): List<String> {
        val raw = prefs.getString(KEY_RECENT_URIS, null) ?: return emptyList()
        return runCatching {
            val array = JSONArray(raw)
            List(array.length()) { array.getString(it) }
        }.getOrDefault(emptyList())
    }

    private fun readGamesCache(prefs: SharedPreferences): Map<String, CachedGame> {
        val raw = prefs.getString(KEY_GAMES_CACHE, null) ?: return emptyMap()
        return runCatching {
            val array = JSONArray(raw)
            buildMap {
                repeat(array.length()) { index ->
                    val item = array.getJSONObject(index)
                    val uriString = item.getString("uri")
                    put(
                        uriString,
                        CachedGame(
                            uri = uriString,
                            title = item.getString("title"),
                            serial = if (item.isNull("serial")) null else item.optString("serial").takeIf(String::isNotBlank),
                            ext = item.optString("ext").ifBlank { uriString.substringAfterLast('.', "").uppercase() },
                            platform = item.optString("platform"),
                        )
                    )
                }
            }
        }.getOrDefault(emptyMap())
    }
}
