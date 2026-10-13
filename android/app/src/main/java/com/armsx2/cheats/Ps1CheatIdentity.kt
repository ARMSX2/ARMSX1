package com.armsx2.cheats

import android.content.Context
import android.net.Uri
import com.armsx2.core.Ps1DiscId
import com.armsx2.core.Ps1Playlist
import com.armsx2.core.Ps1SafAccess
import java.io.File
import java.util.concurrent.ConcurrentHashMap

/** On-demand disc identity. Does not boot a game, guess from names or change settings keys. */
object Ps1CheatIdentity {
    private val verified = ConcurrentHashMap<String, String>()

    /** Blocking: call on IO. SAF data is staged only for this selected game and cleaned up. */
    fun resolve(context: Context, source: String, cancelled: () -> Boolean = { false }): String? {
        if (cancelled()) return null
        verified[source]?.let { return it }
        val serial = runCatching {
            val uri = Uri.parse(source)
            when (uri.scheme) {
                null, "file" -> probe(uri.path ?: source)
                "content" -> Ps1SafAccess.prepare(context, source, cancelled).use { session ->
                    if (cancelled()) null else probe(session.launchPath)
                }
                else -> null
            }
        }.getOrNull()
        val normalised = Ps1CheatDatabaseIndex.normaliseSerial(serial)
        if (cancelled()) return null
        if (normalised != null) verified[source] = normalised
        return normalised
    }

    private fun probe(path: String): String? {
        val file = if (Ps1Playlist.isPlaylist(path)) Ps1Playlist.bootDisc(File(path)) else File(path)
        return file?.let { Ps1DiscId.serialOf(it) }
    }
}
