package com.armsx2.ui

import com.armsx2.ui.InGameOverlay.OsdMode

/** Small, testable preference boundary. Missing game overrides inherit; missing global = Off. */
internal class OsdModePreferences(
    private val read: (String) -> String?,
    private val write: (String, String?) -> Unit,
) {
    companion object {
        // Retain the old key so explicit choices from previous versions survive an update.
        const val GLOBAL_KEY = "ui.osdMode"
        private const val GAME_PREFIX = "ui.osdMode.game."
    }

    private fun parse(raw: String?) = OsdMode.entries.firstOrNull { it.name == raw }
    fun global(): OsdMode = parse(read(GLOBAL_KEY)) ?: OsdMode.Off
    fun override(game: String?): OsdMode? = game?.takeIf(String::isNotBlank)
        ?.let { parse(read(GAME_PREFIX + it)) }
    fun effective(game: String?): OsdMode = override(game) ?: global()
    fun setGlobal(mode: OsdMode) = write(GLOBAL_KEY, mode.name)
    fun setGame(game: String, mode: OsdMode) {
        require(game.isNotBlank())
        write(GAME_PREFIX + game, mode.name)
    }
    fun clearGame(game: String) = write(GAME_PREFIX + game, null)
}

internal object OsdModePolicy {
    fun includes(mode: OsdMode, customEnabled: Boolean, minimal: Boolean = false): Boolean = when (mode) {
        OsdMode.Off -> false
        OsdMode.Min -> minimal
        OsdMode.Full -> true
        OsdMode.Custom -> customEnabled
    }
}
