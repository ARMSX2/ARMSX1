package com.armsx2.core

import java.io.File

/** Mounted-media metadata, not filename guessing or a PBP decompressor. */
object Ps1EmbeddedDiscs {
    data class Snapshot(val currentIndex: Int, val discs: List<Ps1Playlist.Disc>)

    fun parse(wire: String, source: String, expectedSession: Long): Snapshot? {
        if (wire.length > 4096 || source.isBlank() || expectedSession <= 0) return null
        val lines = wire.split('\n')
        if (lines.size !in 3..6) return null // PBP supports two to five embedded discs.
        val header = lines[0].split('\t')
        if (header.size != 2 || header[0].toLongOrNull() != expectedSession) return null
        val current = header[1].toIntOrNull() ?: return null
        val labels = lines.drop(1)
        if (current !in labels.indices || labels.any { it.isBlank() || it.length > 512 || it.any { c -> c.code < 32 } }) return null
        return Snapshot(current, labels.mapIndexed { index, label ->
            Ps1Playlist.Disc(index, label, File(source), true, source,
                embeddedIndex = index, embeddedSession = expectedSession)
        })
    }
}
