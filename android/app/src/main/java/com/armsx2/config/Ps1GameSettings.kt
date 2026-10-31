package com.armsx2.config

import androidx.compose.runtime.mutableIntStateOf
import com.armsx2.runtime.MainActivityRuntime

/**
 * SPARSE per-game overrides for the PS1 core's `settings.toml`.
 *
 * ## What this is
 *
 * The store holds, per game, ONLY the `settings.toml` keys the user explicitly changed while the
 * settings screen was in "This game" scope — never a full copy of the settings. That is the whole
 * point: a full copy freezes a game at the values it had the day it was touched, so a later change
 * to a global default can never reach it again, and the setting reads as dead. Everything the user
 * did NOT override keeps following the global layer forever.
 *
 * ## Identity
 *
 * Keyed by [com.armsx2.GameInfo.settingsKey] — the disc serial extracted by
 * [com.armsx2.core.Ps1DiscId] (with [com.armsx2.FilenameParser] as the container fallback), or a
 * normalised filename stem for serial-less homebrew. That is the SAME key the controller mappings,
 * per-game skins ([com.armsx2.ControllerSkinStore]) and custom names already use — there is exactly
 * one idea of "which game is this" in the app and this does not add a second one.
 *
 * ## Shape
 *
 * Values are stored in `settings.toml` KEY space (`"video.internal_scale"` -> `"4"`), not in Kotlin
 * field space, and the raw value text is exactly what [Ps1SettingsStore.serialize] would have
 * emitted. That is deliberate:
 *
 *  - a field added to [Ps1Settings] by anyone becomes overridable the moment it has a serialize
 *    line, with no list here to keep in sync — nothing can be silently dropped;
 *  - a key that is later REMOVED or RENAMED simply stops being applied (the merge only applies keys
 *    the current build still emits — see [Ps1SettingsStore.PER_GAME_KEYS]) while the stored line is
 *    retained, so nothing is destroyed if it comes back, and the settings screen lists it as an
 *    unrecognised override the user can clear.
 *
 * Persisted in [MainActivityRuntime.prefs] under `ps1.settings.game.<key>`, following the
 * `PREFIX + serial` convention [com.armsx2.ControllerSkinStore] established.
 */
object Ps1GameSettings {

    /** `ps1.settings.game.<settingsKey>` -> the game's override lines. */
    private const val KEY_GAME_PREFIX = "ps1.settings.game."

    /**
     * Bumped on every edit. Read inside [overridesFor] so a composable that resolves settings
     * through [Ps1SettingsStore.load] recomposes when an override is added or cleared — the
     * overrides themselves live in prefs, which Compose cannot observe on its own. Same trick
     * [com.armsx2.CustomNames] uses.
     */
    val version = mutableIntStateOf(0)

    private fun prefs() = runCatching { MainActivityRuntime.prefs }.getOrNull()

    private fun normalise(gameKey: String?): String? = gameKey?.takeIf { it.isNotBlank() }

    /**
     * The stored overrides for [gameKey] as `"section.key"` -> raw TOML value text.
     *
     * Returns everything on disk, INCLUDING keys this build no longer knows: the settings screen
     * needs to be able to show and clear those. [Ps1SettingsStore] is what filters them out of the
     * merge.
     */
    fun overridesFor(gameKey: String?): Map<String, String> {
        version.intValue // subscribe: see [version]
        val key = normalise(gameKey) ?: return emptyMap()
        val raw = runCatching { prefs()?.getString(KEY_GAME_PREFIX + key, null) }.getOrNull()
            ?: return emptyMap()
        return decode(raw)
    }

    /** True when [gameKey] overrides anything at all. */
    fun hasOverrides(gameKey: String?): Boolean = overridesFor(gameKey).isNotEmpty()

    fun count(gameKey: String?): Int = overridesFor(gameKey).size

    /**
     * Merge [delta] into [gameKey]'s overrides.
     *
     * Additive on purpose: [delta] is the set of keys ONE edit changed, so keys the user has not
     * touched this time keep whatever state they already had (overridden or following global).
     * An entry is recorded even when its value happens to equal the global one — "this game pins
     * this value" and "this game follows the global" are different answers, and only storing the
     * former explicitly lets the UI say which layer is winning.
     */
    fun putOverrides(gameKey: String?, delta: Map<String, String>) {
        val key = normalise(gameKey) ?: return
        if (delta.isEmpty()) return
        val merged = LinkedHashMap(overridesFor(key))
        merged.putAll(delta)
        write(key, merged)
    }

    /** Drop ONE key so it follows the global layer again. */
    fun clearOverride(gameKey: String?, tomlKey: String) {
        val key = normalise(gameKey) ?: return
        val merged = LinkedHashMap(overridesFor(key))
        if (merged.remove(tomlKey) == null) return
        write(key, merged)
    }

    /** Drop a set of keys in one write (a row that owns two keys, or a whole tab's reset). */
    fun clearOverrides(gameKey: String?, tomlKeys: Collection<String>) {
        val key = normalise(gameKey) ?: return
        val merged = LinkedHashMap(overridesFor(key))
        var changed = false
        for (k in tomlKeys) if (merged.remove(k) != null) changed = true
        if (changed) write(key, merged)
    }

    /** Drop EVERY override for [gameKey] — the game falls all the way back to global. */
    fun clearAll(gameKey: String?) {
        val key = normalise(gameKey) ?: return
        prefs()?.edit()?.remove(KEY_GAME_PREFIX + key)?.apply()
        version.intValue++
    }

    /** Game keys that currently override something (for a "which games are customised" list). */
    fun gamesWithOverrides(): List<String> {
        version.intValue
        val all = runCatching { prefs()?.all }.getOrNull() ?: return emptyList()
        return all.keys
            .filter { it.startsWith(KEY_GAME_PREFIX) }
            .map { it.removePrefix(KEY_GAME_PREFIX) }
            .filter { overridesFor(it).isNotEmpty() }
            .sorted()
    }

    private fun write(key: String, map: Map<String, String>) {
        val p = prefs() ?: return
        if (map.isEmpty()) p.edit().remove(KEY_GAME_PREFIX + key).apply()
        else p.edit().putString(KEY_GAME_PREFIX + key, encode(map)).apply()
        version.intValue++
    }

    // ---- encoding -------------------------------------------------------------------------
    // One `section.key = value` per line, which is exactly the shape Ps1SettingsStore.serialize
    // emits, so an override is readable in a bug report without a decoder. Values never contain a
    // newline (serialize writes one line per key); any that somehow did is escaped so a hand-edited
    // path can't split one override into two.

    private fun encode(map: Map<String, String>): String =
        map.entries.joinToString("\n") { (k, v) -> "$k = " + v.replace("\\", "\\\\").replace("\n", "\\n") }

    private fun decode(raw: String): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (line in raw.lineSequence()) {
            val trimmed = line.trim()
            if (trimmed.isEmpty()) continue
            val eq = trimmed.indexOf('=')
            if (eq <= 0) continue
            val k = trimmed.substring(0, eq).trim()
            if (k.isEmpty()) continue
            out[k] = trimmed.substring(eq + 1).trim().replace("\\n", "\n").replace("\\\\", "\\")
        }
        return out
    }
}
