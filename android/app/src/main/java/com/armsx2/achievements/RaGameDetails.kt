package com.armsx2.achievements

import java.net.URI
import org.json.JSONObject

/** Display-only Web API data. Never loads a VM or copies achievement logic into the core. */
internal object RaGameDetails {
    data class Achievement(
        val id: Int, val title: String, val description: String, val points: Int,
        val badgeUrl: String?, val softcore: Boolean, val hardcore: Boolean,
        val order: Int, val type: Int,
    )
    data class Game(val id: Int, val title: String, val artUrl: String?, val items: List<Achievement>)
    data class Loaded(val game: Game, val saved: Boolean = false, val stale: Boolean = false)
    enum class Filter { ALL, UNLOCKED, LOCKED }

    fun parse(body: String, expectedId: Int): Game {
        val root = try { JSONObject(body) } catch (_: Exception) { error("RetroAchievements returned invalid game details.") }
        check(!root.has("Error") && !root.has("error") &&
            (!root.has("Success") || root.optBoolean("Success")) &&
            (!root.has("success") || root.optBoolean("success"))) { "Could not load this game's achievements. Check your connection and web API key." }
        val id = root.optInt("ID", root.optInt("id", 0))
        check(id > 0 && id == expectedId) { "RetroAchievements returned details for a different game." }
        check(root.optInt("ConsoleID", root.optInt("consoleId", -1)) == RaLibrarySync.CONSOLE_ID) {
            "RetroAchievements returned details for a different system."
        }
        val achievements = root.optJSONObject("Achievements") ?: root.optJSONObject("achievements")
            ?: error("RetroAchievements did not return an achievement list.")
        val items = achievements.keys().asSequence().map { key ->
            val item = achievements.optJSONObject(key) ?: error("RetroAchievements returned an invalid achievement.")
            val itemId = item.optInt("ID", item.optInt("id", key.toIntOrNull() ?: 0))
            check(itemId > 0) { "RetroAchievements returned an invalid achievement ID." }
            val hardcore = earned(item, "DateEarnedHardcore", "dateEarnedHardcore")
            val softcore = hardcore || earned(item, "DateEarned", "dateEarned")
            val badge = item.optString("BadgeName", item.optString("badgeName")).trim()
            Achievement(itemId, item.optString("Title", item.optString("title")),
                item.optString("Description", item.optString("description")),
                item.optInt("Points", item.optInt("points", 0)).coerceAtLeast(0),
                if (Regex("[0-9]+").matches(badge)) "https://media.retroachievements.org/Badge/$badge.png" else null,
                softcore, hardcore, item.optInt("DisplayOrder", item.optInt("displayOrder", 0)),
                when (item.optString("type", item.optString("Type"))) {
                    "missable" -> 1; "progression" -> 2; "win_condition" -> 3; else -> 0
                })
        }.sortedWith(compareBy<Achievement> { it.order }.thenBy { it.id }).toList()
        check(items.isNotEmpty() || root.optInt("NumAchievements", root.optInt("numAchievements", 0)) == 0) {
            "RetroAchievements returned an incomplete achievement list."
        }
        return Game(id, root.optString("Title", root.optString("title")),
            art(root.optString("ImageBoxArt", root.optString("imageBoxArt")))
                ?: art(root.optString("ImageIcon", root.optString("imageIcon"))), items)
    }

    private fun earned(item: JSONObject, upper: String, lower: String): Boolean {
        val value = item.optString(upper, item.optString(lower)).trim()
        return value.isNotEmpty() && value != "null" && value != "0" && !value.startsWith("0000-")
    }

    /** Only official HTTPS artwork paths are usable as remote image models. */
    fun art(value: String?): String? = runCatching {
        val text = value?.trim()?.takeIf { it.isNotEmpty() && it != "null" } ?: return null
        val uri = URI(if (text.startsWith('/')) "https://media.retroachievements.org$text" else text)
        if (uri.scheme != "https" || uri.host !in setOf("retroachievements.org", "media.retroachievements.org", "static.retroachievements.org") ||
            uri.rawUserInfo != null || uri.port != -1 || uri.rawQuery != null || uri.rawFragment != null ||
            !Regex("/(?:Images|Badge)/[a-zA-Z0-9_-]+\\.(?:png|jpg|jpeg|webp)").matches(uri.path)) return null
        uri.toString()
    }.getOrNull()

    fun select(items: List<Achievement>, query: String, filter: Filter): List<Achievement> {
        val text = query.trim()
        return items.filter {
            (filter == Filter.ALL || (filter == Filter.UNLOCKED) == it.softcore) &&
                (text.isEmpty() || it.title.contains(text, true) || it.description.contains(text, true))
        }
    }
}
