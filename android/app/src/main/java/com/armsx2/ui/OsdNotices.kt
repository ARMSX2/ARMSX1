package com.armsx2.ui

import androidx.compose.runtime.mutableStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * Core events shown as in-game OSD notes: memory card saves, the cheats in effect, and a one-line
 * summary of the settings a game launched with. Each kind has its own switch; all of them also
 * follow the existing "On-screen notifications" setting, which gates every note.
 */
object OsdNotices {
    private const val KEY_MEMORY_CARD = "osd.notice.memoryCard"
    private const val KEY_CHEATS = "osd.notice.cheats"
    private const val KEY_SETTINGS = "osd.notice.settings"

    // All off by default: each one is opt-in from the in-game OSD menu.
    val memoryCard = mutableStateOf(false)
    val cheats = mutableStateOf(false)
    val settings = mutableStateOf(false)

    fun load() {
        val prefs = runCatching { MainActivityRuntime.prefs }.getOrNull() ?: return
        memoryCard.value = runCatching { prefs.getBoolean(KEY_MEMORY_CARD, false) }.getOrDefault(false)
        cheats.value = runCatching { prefs.getBoolean(KEY_CHEATS, false) }.getOrDefault(false)
        settings.value = runCatching { prefs.getBoolean(KEY_SETTINGS, false) }.getOrDefault(false)
    }

    fun setMemoryCard(show: Boolean) = store(memoryCard, KEY_MEMORY_CARD, show)
    fun setCheats(show: Boolean) = store(cheats, KEY_CHEATS, show)
    fun setSettings(show: Boolean) = store(settings, KEY_SETTINGS, show)

    private fun store(state: androidx.compose.runtime.MutableState<Boolean>, key: String, value: Boolean) {
        state.value = value
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(key, value).apply() }
    }

    /** Entry point for the core's OSD listener. Any thread. */
    fun post(kind: String, detail: String) {
        val enabled = when {
            kind.startsWith("memory_card.") -> memoryCard.value
            kind == "cheats" -> cheats.value
            kind == "settings" -> settings.value
            else -> false
        }
        if (!enabled) return
        val note = format(kind, detail) ?: return
        GameOsd.notice(note.key, note.text, note.durationMs)
    }

    data class Note(val key: String, val text: String, val durationMs: Long)

    /** Pure formatting, so it is testable without Android. [text] looks up a translated string. */
    fun format(kind: String, detail: String, text: (String) -> String = com.armsx2.i18n.I18n::get): Note? {
        val fields = detail.split('\u001f')
        return when (kind) {
            "memory_card.saving", "memory_card.saved", "memory_card.failed" -> {
                val slot = fields.firstOrNull()?.toIntOrNull()?.takeIf { it in 1..2 } ?: return null
                // One key per card: "saving…" is replaced by "saved" or the error.
                val key = "memory_card.$slot"
                when (kind) {
                    "memory_card.saving" -> Note(key, text("osd.memoryCard.saving").replace("{slot}", "$slot"), SAVING_MS)
                    "memory_card.saved" -> Note(key, text("osd.memoryCard.saved").replace("{slot}", "$slot"), SAVED_MS)
                    else -> Note(
                        key,
                        text("osd.memoryCard.failed").replace("{slot}", "$slot")
                            .replace("{error}", fields.getOrNull(1).orEmpty().ifBlank { "?" }),
                        FAILED_MS,
                    )
                }
            }
            "cheats" -> {
                val names = fields.map(String::trim).filter(String::isNotEmpty)
                if (names.isEmpty()) Note("cheats", text("osd.cheats.none"), CHEATS_MS)
                else Note(
                    "cheats",
                    text("osd.cheats.active").replace("{count}", "${names.size}")
                        .replace("{names}", cheatList(names, text("osd.cheats.more"))),
                    CHEATS_MS,
                )
            }
            "settings" -> detail.trim().takeIf(String::isNotEmpty)?.let { Note("settings", it, SETTINGS_MS) }
            else -> null
        }
    }

    /** As many names as fit on one OSD line, then "+N more". */
    internal fun cheatList(names: List<String>, moreTemplate: String): String {
        val shown = ArrayList<String>()
        var length = 0
        for (name in names) {
            if (shown.isNotEmpty() && length + 2 + name.length > CHEAT_LINE_CHARS) break
            shown += name
            length += (if (shown.size > 1) 2 else 0) + name.length
        }
        val rest = names.size - shown.size
        return shown.joinToString(", ") + if (rest > 0) " " + moreTemplate.replace("{count}", "$rest") else ""
    }

    private const val CHEAT_LINE_CHARS = 60
    private const val SAVING_MS = 4_000L
    private const val SAVED_MS = 2_500L
    private const val FAILED_MS = 8_000L
    private const val CHEATS_MS = 5_000L
    private const val SETTINGS_MS = 5_000L
}
