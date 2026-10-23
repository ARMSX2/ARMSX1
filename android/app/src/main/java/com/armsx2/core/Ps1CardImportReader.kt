package com.armsx2.core

import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.Locale

/** Counts expanded bytes, even for skipped ZIP entries; never trusts ZIP size metadata. */
internal class Ps1CardImportReader(
    private val maxBytes: Long = 96L * 1024 * 1024,
    private val maxEntries: Int = 4096,
) {
    private var entries = 0
    private var bytes = 0L
    fun read(name: String, input: InputStream): ByteArray? {
        check(++entries <= maxEntries) { "Too many files (limit $maxEntries)." }
        val parts = name.replace('\\', '/').split('/')
        val safe = parts.none { it == ".." } && !name.startsWith('/') && ':' !in name
        val accepted = safe && parts.lastOrNull().orEmpty().substringAfterLast('.', "")
            .lowercase(Locale.ROOT) in setOf("mcd", "mcr", "mc", "srm")
        val output = if (accepted) ByteArrayOutputStream() else null
        val buffer = ByteArray(8192)
        var entryBytes = 0L
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            bytes += n; entryBytes += n
            check(bytes <= maxBytes) { "Import exceeds the expanded-data limit ($maxBytes bytes)." }
            if (entryBytes <= 128 * 1024) output?.write(buffer, 0, n)
        }
        val data = output?.toByteArray() ?: return null
        return data.takeIf { entryBytes == 128L * 1024 && it.size >= 2 &&
            it[0] == 'M'.code.toByte() && it[1] == 'C'.code.toByte() }
    }
}
