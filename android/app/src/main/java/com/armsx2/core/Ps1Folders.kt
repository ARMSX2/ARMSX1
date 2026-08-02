package com.armsx2.core

import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime

/**
 * The user's game library folders (absolute paths). Persisted in prefs AND mirrored into
 * settings.toml `[library].folders` so the native core sees the same list. Requires all-files
 * access (see [Ps1Storage]) for the paths to be readable/launchable.
 */
object Ps1Folders {
    private const val KEY = "library.folders"

    /** Observable folder set (absolute paths). */
    val folders = mutableStateOf<List<String>>(emptyList())

    @Volatile
    private var loaded = false

    fun load() {
        val stored = MainActivityRuntime.prefs.getStringSet(KEY, emptySet()) ?: emptySet()
        folders.value = stored.sorted()
        loaded = true
    }

    /** Hydrate once, on demand. Nothing in the ported front-end calls [load] at startup, and the
     *  library repository reads this list from a background scan — so it hydrates itself rather
     *  than silently scanning an empty folder set. Prefs are lateinit, hence the runCatching. */
    fun ensureLoaded() {
        if (!loaded) runCatching { load() }
    }

    fun add(path: String) {
        ensureLoaded()
        val next = (folders.value + path).distinct().sorted()
        folders.value = next
        persist(next)
        // Also register with the front-end's own ROM-folder list, which is what the library
        // screen passes to the repository (and what gates its "scan at all" check). Additive and
        // de-duplicated: existing content:// entries keep their SAF grants untouched.
        mirrorToRomsDirs(path)
    }

    fun remove(path: String) {
        ensureLoaded()
        val next = folders.value.filterNot { it == path }
        folders.value = next
        persist(next)
    }

    /**
     * Adopt [paths] as THE folder set, without touching `romsDirs`.
     *
     * The library repository's direction of travel is romsDirs → here: it resolves the front-end's
     * ROM folders (SAF tree URIs included) down to absolute paths the PS1 core can fopen and hands
     * them over, so mirroring back would be a loop. Authoritative rather than additive so removing
     * a folder in the front-end actually stops it being scanned — but an EMPTY list is ignored,
     * because "nothing resolved this pass" (no all-files grant yet, SD card unmounted) must not
     * silently wipe the user's library folders out of settings.toml.
     *
     * No-ops when nothing changes, so a rescan doesn't rewrite settings.toml every time.
     */
    fun syncFromLibrary(paths: List<String>) {
        ensureLoaded()
        val next = paths.filter { it.isNotBlank() }.distinct().sorted()
        if (next.isEmpty() || next == folders.value) return
        folders.value = next
        persist(next)
    }

    private fun mirrorToRomsDirs(path: String) {
        runCatching {
            val current = MainActivityRuntime.romsDirs.value
            if (path in current) return@runCatching
            MainActivityRuntime.setRomsDirs(current + path)
        }
    }

    private fun persist(list: List<String>) {
        MainActivityRuntime.prefs.edit { putStringSet(KEY, list.toSet()) }
        // Mirror into settings.toml so the native core's own library sees the same folders.
        MainActivityRuntime.instance?.applicationContext?.let { ctx ->
            runCatching {
                val cur = com.armsx2.config.Ps1SettingsStore.load(ctx)
                com.armsx2.config.Ps1SettingsStore.save(ctx, cur.copy(folders = list))
            }
        }
    }
}
