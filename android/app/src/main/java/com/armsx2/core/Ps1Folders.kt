package com.armsx2.core

import androidx.compose.runtime.mutableStateOf
import androidx.core.content.edit
import com.armsx2.runtime.MainActivityRuntime

/**
 * POSIX game-library folders mirrored into `settings.toml` for the native front-end.
 *
 * Android SAF trees deliberately do not enter this object: their `content://` identity stays in
 * [MainActivityRuntime.romsDirs], [Ps1Library] enumerates them through DocumentsProvider, and
 * [Ps1SafAccess] leases the selected document to the native core only for the running session.
 */
object Ps1Folders {
    private const val KEY = "library.folders"

    /** Observable POSIX folder set. */
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
     * Adopt [paths] as the complete POSIX folder set, without touching `romsDirs`.
     *
     * An empty list is meaningful: it clears stale native-library paths after the user removes the
     * last raw folder or migrates entirely to SAF. SAF access is never inferred from this mirror.
     */
    fun syncFromLibrary(paths: List<String>) {
        ensureLoaded()
        val next = paths.filter { it.isNotBlank() }.distinct().sorted()
        if (next == folders.value) return
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
