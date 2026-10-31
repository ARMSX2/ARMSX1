package com.armsx2.core

import java.util.Locale

/** Conservative matching: an explicit serial must never fall back to a different region. */
internal object Ps1BulkCardMatcher {
    data class Game(val source: String, val serial: String?, val titles: List<String>)
    data class Match(val source: String, val slot: Int)
    private val serial = Regex("(?i)([A-Z]{4})[-_ .]?(\\d{3})[.]?(\\d{2})")
    private fun serialOf(text: String) = serial.find(text)?.let {
        "${it.groupValues[1].uppercase(Locale.ROOT)}-${it.groupValues[2]}${it.groupValues[3]}"
    }
    private fun normalized(text: String) = text.lowercase(Locale.ROOT)
        .replace(Regex("[^\\p{L}\\p{N}]+"), " ").trim()

    fun match(name: String, games: List<Game>): List<Match> {
        val stem = name.substringBeforeLast('.')
        val suffix = Regex("_([12])$").find(stem)
        val slot = suffix?.groupValues?.get(1)?.toInt() ?: 1
        val title = if (suffix == null) stem else stem.removeRange(suffix.range)
        val id = serialOf(title)
        if (id != null) return games.filter { serialOf(it.serial.orEmpty()) == id }.map { Match(it.source, slot) }
        val key = normalized(title)
        if (key.isBlank()) return emptyList()
        val matches = games.filter { game -> game.titles.any { normalized(it) == key } }
        // A title shared by different regions is not sufficient evidence.
        if (matches.map { it.serial?.uppercase(Locale.ROOT) ?: it.source }.distinct().size != 1) return emptyList()
        return matches.map { Match(it.source, slot) }
    }
}
