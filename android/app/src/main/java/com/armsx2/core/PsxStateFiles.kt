package com.armsx2.core

import java.io.File
import dev.psxlab.BackendStateStore

/** Match old staged states only by the full document URI digest, never by a game title. */
internal object PsxStateFiles {
    fun source(saved: String, sources: List<String>): String? {
        sources.firstOrNull { BackendStateStore.normalize(it) == BackendStateStore.normalize(saved) }?.let { return it }
        if (!saved.contains("/cache/saf-launch/")) return saved.takeIf { it.isNotBlank() }
        val name = File(saved).name
        return sources.firstOrNull { source ->
            source.startsWith("content://") &&
                name.substringBeforeLast('.') == Ps1SafText.launchFileName(source, "game.bin").substringBeforeLast('.')
        }
    }
    fun companions(name: String) = listOf("$name.png")
}
