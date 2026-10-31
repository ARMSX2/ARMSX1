package com.armsx2.achievements

import java.security.MessageDigest
import java.util.Locale
import org.json.JSONArray
import org.json.JSONObject

/** Library identity is the actual disc hash, never a filename/title/cover-art guess. */
internal object RaLibrarySync {
    const val CONSOLE_ID = 12
    private val md5 = Regex("[0-9a-f]{32}")
    enum class Readiness(val messageKey: String?) {
        READY(null), APP_NOT_READY("ra.library.appNotReady"), NATIVE_NOT_READY("ra.library.nativeNotReady"),
        MISSING_KEY("ra.library.missingKey"), MISSING_USER("ra.library.missingUser"), EMPTY_LIBRARY("ra.library.emptyLibrary"),
    }
    fun readiness(contextReady: Boolean, nativeReady: Boolean, key: String, user: String, games: Int): Readiness = when {
        !contextReady -> Readiness.APP_NOT_READY
        !nativeReady -> Readiness.NATIVE_NOT_READY
        key.isBlank() -> Readiness.MISSING_KEY
        user.isBlank() -> Readiness.MISSING_USER
        games <= 0 -> Readiness.EMPTY_LIBRARY
        else -> Readiness.READY
    }
    /** Recover the authenticated account even if the library hook ran before login finished. */
    fun accountForSync(stored: String, nativeSnapshot: String): String = runCatching {
        val snapshot = JSONObject(nativeSnapshot)
        if (snapshot.optBoolean("loggedIn")) snapshot.optString("userName").trim().takeIf(String::isNotBlank) else null
    }.getOrNull() ?: stored.trim()

    data class Entry(val path: String, val serial: String?) {
        fun progressKey(user: String): String = "ra.web.${digest(user.lowercase(Locale.ROOT))}.${digest(path)}"
    }
    data class Catalog(val hashToGame: Map<String, Int>, val gameToTotal: Map<Int, Int>)
    data class Progress(val softcore: Int, val hardcore: Int)
    data class HashResult(val hash: String? = null, val deferred: Boolean = false)
    data class Result(val matched: Int, val total: Int, val unreadable: Int, val unrecognised: Int, val deferred: Int) {
        val complete: Boolean get() = total > 0 && unreadable == 0 && deferred == 0
        fun description(): String = buildString {
            append("$matched of $total games matched")
            if (unreadable > 0) append(" · $unreadable could not be read")
            if (unrecognised > 0) append(" · $unrecognised hashes not in the achievement catalogue")
            if (deferred > 0) append(" · $deferred need a manual disc scan")
        }
    }
    fun normaliseHash(value: String?): String? = value?.trim()?.lowercase(Locale.ROOT)?.takeIf(md5::matches)
    /** Different images sharing a serial must never share an achievement hash. */
    fun hashCacheKey(path: String, size: Long, modified: Long): String =
        "ra.psxHash.v2.${digest("$path\n$size\n$modified")}"
    private fun digest(value: String): String = MessageDigest.getInstance("SHA-256")
        .digest(value.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }

    fun parseCatalog(body: String): Catalog {
        val array = try { JSONArray(body) } catch (_: Exception) {
            error("Invalid RetroAchievements game list. Check your web API key and try again.")
        }
        val hashes = HashMap<String, Int>()
        val totals = HashMap<Int, Int>()
        for (i in 0 until array.length()) {
            val game = array.optJSONObject(i) ?: continue
            if (game.optInt("ConsoleID", game.optInt("consoleId", CONSOLE_ID)) != CONSOLE_ID) continue
            val id = game.optInt("ID", game.optInt("id", 0))
            val count = game.optInt("NumAchievements", game.optInt("numAchievements", 0))
            if (id <= 0 || count <= 0) continue
            val supported = game.optJSONArray("Hashes") ?: game.optJSONArray("hashes") ?: continue
            for (h in 0 until supported.length()) normaliseHash(supported.optString(h))?.let { hashes[it] = id }
            totals[id] = count
        }
        check(hashes.isNotEmpty()) { "RetroAchievements returned no supported PlayStation hashes. Check your web API key and try again." }
        return Catalog(hashes, totals)
    }

    /** Reject errors/incomplete pages; an API failure must never become zero earned unlocks. */
    fun fetchProgress(fetchPage: (Int) -> String): Map<Int, Progress> {
        val progress = HashMap<Int, Progress>()
        var offset = 0
        repeat(1_000) {
            val body = fetchPage(offset)
            val page = try { JSONObject(body) } catch (_: Exception) {
                error("Could not read your RetroAchievements progress. Please try again.")
            }
            check(!page.has("Error") && !page.has("error") &&
                (!page.has("Success") || page.optBoolean("Success")) &&
                (!page.has("success") || page.optBoolean("success"))) {
                "RetroAchievements progress was unavailable. Check your web API key and account."
            }
            val rows = page.optJSONArray("Results") ?: page.optJSONArray("results")
                ?: error("RetroAchievements progress was unavailable. Check your web API key and account.")
            val total = page.optInt("Total", page.optInt("total", -1))
            check(total >= offset + rows.length()) { "RetroAchievements returned an incomplete progress page." }
            for (i in 0 until rows.length()) {
                val row = rows.optJSONObject(i) ?: error("RetroAchievements returned an invalid progress entry.")
                val console = row.optInt("ConsoleID", row.optInt("consoleId", -1))
                check(console >= 0) { "RetroAchievements progress is missing its system identity." }
                if (console != CONSOLE_ID) continue
                val id = row.optInt("GameID", row.optInt("gameId", 0))
                val soft = row.optInt("NumAwarded", row.optInt("numAwarded", -1))
                val hard = row.optInt("NumAwardedHardcore", row.optInt("numAwardedHardcore", -1))
                check(id > 0 && soft >= 0 && hard >= 0) { "RetroAchievements returned invalid unlock counts." }
                progress[id] = Progress(maxOf(soft, hard), hard)
            }
            offset += rows.length()
            if (offset >= total) return progress
            check(rows.length() > 0) { "RetroAchievements progress ended before all pages were received." }
        }
        error("RetroAchievements progress exceeded the page limit.")
    }

    fun match(
        games: List<Entry>, catalog: Catalog, progress: Map<Int, Progress>,
        hash: (Entry) -> HashResult, record: (Entry, Progress, Int) -> Unit,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
        onIdentified: (Entry, Int) -> Unit = { _, _ -> },
    ): Result {
        var matched = 0
        var unreadable = 0
        var unrecognised = 0
        var deferred = 0
        games.forEachIndexed { index, game ->
            onProgress(index, games.size)
            val result = hash(game)
            val value = normaliseHash(result.hash)
            val id = value?.let(catalog.hashToGame::get)
            val total = id?.let(catalog.gameToTotal::get) ?: 0
            when {
                result.deferred -> deferred++
                value == null -> unreadable++
                id == null || total <= 0 -> unrecognised++
                else -> {
                    onIdentified(game, id)
                    record(game, progress[id] ?: Progress(0, 0), total)
                    matched++
                }
            }
        }
        onProgress(games.size, games.size)
        return Result(matched, games.size, unreadable, unrecognised, deferred)
    }
}
