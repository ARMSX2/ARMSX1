package com.armsx2.ui.settings

/** Cheat selections are intentionally game-local, not an unexpected global override. */
internal object Ps1OverrideNoticePolicy {
    fun isCheatSetting(key: String): Boolean = key.startsWith("cheats.")

    // Preserve legacy config data without resurfacing the removed, inactive option in the UI.
    fun visibleSettings(overrides: Map<String, String>): Map<String, String> =
        overrides.filterKeys { it != "input.analog_mode_default" && it != "bios.override_file" && it != "bios.resolved_file" }

    fun visibleOverrides(overrides: Map<String, String>): Map<String, String> =
        visibleSettings(overrides).filterKeys { !isCheatSetting(it) }
}
