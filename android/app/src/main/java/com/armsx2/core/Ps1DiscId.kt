package com.armsx2.core

import java.io.File
import java.io.RandomAccessFile
import java.util.Locale

/**
 * Best-effort PS1 disc-serial extraction, entirely in Kotlin (no core/JNI needed).
 *
 * Every PS1 disc has a `SYSTEM.CNF` near the start of the data track with a line like
 * `BOOT = cdrom:\SLUS_005.94;1`. Rather than parse ISO9660 (which differs between 2048-byte ISO and
 * 2352-byte raw BIN sectors), we scan the first chunk of the data for the ASCII `cdrom:` token and
 * read the serial that follows — format-agnostic across .iso/.bin/.img and the .bin referenced by a
 * .cue. Compressed containers (.chd/.zip) are skipped (returns null → cover falls back).
 *
 * The serial is normalised to psx-covers' filename form: `SLUS_005.94` → `SLUS-00594`.
 */
object Ps1DiscId {

    private const val SCAN_BYTES = 16 * 1024 * 1024   // SYSTEM.CNF lives early in the track
    private const val CHUNK = 1 * 1024 * 1024
    private const val OVERLAP = 64

    private val BOOT_REGEX = Regex("""cdrom:\\?([A-Za-z]{4})_?(\d{3})\.?(\d{2})""")

    fun serialOf(rom: File): String? {
        val dataFile = resolveDataFile(rom) ?: return null
        return try {
            scanForSerial(dataFile)
        } catch (_: Exception) {
            null
        }
    }

    /** For a .cue, follow its first FILE line to the real .bin; otherwise scan the file itself. */
    private fun resolveDataFile(rom: File): File? {
        val ext = rom.extension.lowercase(Locale.US)
        return when (ext) {
            "cue" -> cueBinFile(rom) ?: rom
            "bin", "iso", "img" -> rom
            else -> null // chd/zip/exe: skip
        }
    }

    private fun cueBinFile(cue: File): File? {
        return try {
            val line = cue.useLines { lines ->
                lines.firstOrNull { it.trimStart().startsWith("FILE", ignoreCase = true) }
            } ?: return null
            val name = Regex("""FILE\s+"([^"]+)"""", RegexOption.IGNORE_CASE).find(line)?.groupValues?.get(1)
                ?: line.substringAfter("FILE").trim().substringBefore(' ').trim('"')
            File(cue.parentFile, name).takeIf { it.isFile }
        } catch (_: Exception) {
            null
        }
    }

    private fun scanForSerial(file: File): String? {
        RandomAccessFile(file, "r").use { raf ->
            val limit = minOf(file.length(), SCAN_BYTES.toLong())
            var pos = 0L
            val buf = ByteArray(CHUNK + OVERLAP)
            var carry = ByteArray(0)
            while (pos < limit) {
                raf.seek(pos)
                val toRead = minOf(CHUNK.toLong(), limit - pos).toInt()
                val read = raf.read(buf, 0, toRead)
                if (read <= 0) break
                // Prepend carry-over so a token spanning the boundary is still found.
                val window = if (carry.isEmpty()) String(buf, 0, read, Charsets.ISO_8859_1)
                else String(carry + buf.copyOf(read), Charsets.ISO_8859_1)
                BOOT_REGEX.find(window)?.let { m ->
                    return "${m.groupValues[1].uppercase()}-${m.groupValues[2]}${m.groupValues[3]}"
                }
                carry = buf.copyOfRange(maxOf(0, read - OVERLAP), read)
                pos += read
            }
        }
        return null
    }
}
