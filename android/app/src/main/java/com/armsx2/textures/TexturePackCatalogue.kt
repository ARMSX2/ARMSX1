package com.armsx2.textures

import com.armsx2.cheats.Ps1CheatDatabaseIndex
import java.net.URI

data class TexturePack(
    val id: String, val title: String, val serials: Set<String>, val version: String,
    val author: String, val license: String, val sourceUrl: String, val downloadUrl: String,
    val sha256: String, val downloadBytes: Long, val expandedBytes: Long,
) {
    val storageKey: String get() = "$id:$sha256"
    fun matches(serial: String?): Boolean = Ps1CheatDatabaseIndex.normaliseSerial(serial)?.let { it in serials } == true
    fun row(): String = listOf(id, title, serials.sorted().joinToString(","), version, author, license,
        sourceUrl, downloadUrl, sha256, downloadBytes.toString(), expandedBytes.toString()).joinToString("\t")
}

/** Explicit serials only: never infer pack compatibility from a title, filename or region tag. */
object TexturePackCatalogue {
    const val HEADER = "# EmuCorePSX texture packs v1"
    const val MAX_DOWNLOAD = 2L * 1024 * 1024 * 1024
    const val MAX_EXPANDED = 8L * 1024 * 1024 * 1024
    private val ID = Regex("[a-z0-9][a-z0-9-]{0,63}")
    private val HASH = Regex("[a-f0-9]{64}")
    private val DOWNLOAD_HOSTS = setOf("github.com", "raw.githubusercontent.com", "objects.githubusercontent.com",
        "release-assets.githubusercontent.com")

    fun https(url: String, download: Boolean = false): URI {
        val uri = URI(url)
        require(uri.scheme == "https" && !uri.host.isNullOrBlank() && uri.rawUserInfo == null && uri.fragment == null)
        require(uri.port == -1 || uri.port == 443)
        if (download) require(uri.host.lowercase() in DOWNLOAD_HOSTS) { "Unsupported texture download host" }
        return uri
    }

    fun parse(text: String): List<TexturePack> {
        require(text.toByteArray(Charsets.UTF_8).size <= 1024 * 1024 && '\u0000' !in text)
        require(text.lineSequence().firstOrNull() == HEADER) { "Unsupported texture catalogue version" }
        val rows = text.lineSequence().drop(1).filter { it.isNotBlank() && !it.startsWith('#') }.toList()
        require(rows.size <= 2000)
        val packs = rows.map { row ->
            val f = row.split('\t')
            require(f.size == 11 && f.all { it.isNotBlank() && it.length <= 2048 && '\r' !in it })
            require(ID.matches(f[0]) && HASH.matches(f[8]))
            val serials = f[2].split(',')
            require(serials.size in 1..128 && serials.all { Ps1CheatDatabaseIndex.normaliseSerial(it) == it })
            require(serials.toSet().size == serials.size)
            https(f[6]); https(f[7], download = true)
            val bytes = f[9].toLongOrNull() ?: error("Invalid download size")
            val expanded = f[10].toLongOrNull() ?: error("Invalid unpacked size")
            require(bytes in 1..MAX_DOWNLOAD && expanded in 1..MAX_EXPANDED)
            TexturePack(f[0], f[1], serials.toSet(), f[3], f[4], f[5], f[6], f[7], f[8], bytes, expanded)
        }
        require(packs.map { it.id }.toSet().size == packs.size) { "Duplicate texture pack ID" }
        return packs
    }
}
