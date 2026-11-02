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
    fun source(saved: String, identity: String, serials: Map<String, String>): String? {
        val sources = serials.keys.toList()
        sources.firstOrNull { BackendStateStore.normalize(it) == BackendStateStore.normalize(saved) }?.let { return it }
        val serial = BackendStateStore.serial(identity)
        if (serial.isNotBlank()) serials.filterValues { it.equals(serial, ignoreCase = true) }.keys.singleOrNull()?.let { return it }
        return source(saved, sources)
    }
    fun companions(name: String) = listOf("$name.png")
}
