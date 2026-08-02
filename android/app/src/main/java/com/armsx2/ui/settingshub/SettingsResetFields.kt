package com.armsx2.ui.settingshub

import com.armsx2.config.Settings
import com.armsx2.navigation.SettingsCategory
import org.json.JSONObject

/**
 * Which [Settings] fields each settings tab owns, so Reset can scope to the tab you're
 * looking at instead of wiping everything.
 *
 * EMPTY IN THE PS1 PORT. Every entry here named a PCSX2 field (VU clamps, DEV9, GS hacks, SPU2)
 * that no tab surfaces any more — the emulator tabs edit [com.armsx2.config.Ps1Settings] and write
 * `settings.toml` instead. Leaving the old lists in place would have put a Reset button back on the
 * top bar of tabs whose visible rows it could not touch, so the map is empty and each PS1 tab
 * carries its own "Restore defaults" row. The machinery below is kept intact for the Controls tab
 * (which resets through ControllerMappings) and for whenever a Settings-backed tab returns.
 *
 * Entries are the JSON keys from [Settings.toJson], which IS the persistence format
 * (ConfigStore.saveGlobal stores `toJson().toString()` and loads it back through
 * `Settings.fromJson`). That round-trip is known-complete, which is what lets [resetCategory]
 * swap individual values against a default Settings() safely.
 */
internal val SETTINGS_CATEGORY_FIELDS: Map<SettingsCategory, List<String>> = emptyMap()

/** Default values for just this category's fields, leaving every other tab untouched. */
internal fun Settings.resetCategory(category: SettingsCategory): Settings {
    val fields = SETTINGS_CATEGORY_FIELDS[category] ?: return this
    val current = toJson()
    val defaults = Settings().toJson()
    for (key in fields) {
        if (defaults.has(key)) current.put(key, defaults.get(key)) else current.remove(key)
    }
    return Settings.fromJson(current)
}

/** The per-game override keys belonging to [category], for a scoped per-game reset. */
internal fun categoryOverrideKeys(category: SettingsCategory): List<String> =
    SETTINGS_CATEGORY_FIELDS[category].orEmpty()

/** True when this tab has anything the Reset button could restore. */
internal fun categoryHasResettableSettings(category: SettingsCategory): Boolean =
    !SETTINGS_CATEGORY_FIELDS[category].isNullOrEmpty()

/** Strip [keys] from a per-game override blob; null when nothing is left to store. */
internal fun pruneOverrides(overrides: JSONObject, keys: List<String>): JSONObject? {
    for (key in keys) overrides.remove(key)
    return if (overrides.length() == 0) null else overrides
}
