package com.armsx2.ui.settings

/** Cheat selections are intentionally game-local, not an unexpected global override. */
internal object Ps1OverrideNoticePolicy {
    fun isCheatSetting(key: String): Boolean = key.startsWith("cheats.")

    fun visibleOverrides(overrides: Map<String, String>): Map<String, String> =
        overrides.filterKeys { !isCheatSetting(it) }
}
