package com.armsx2.core

import java.io.File
import java.io.IOException
import java.io.InputStream

/** Keep optional subchannel files beside the renamed image handed to SwanStation. */
internal object Ps1Sbi {
    private const val MAX_BYTES = 1024 * 1024
    data class Sidecar(val source: String, val destination: String)

    fun sidecars(imageName: String, localName: String, siblings: Collection<String>): List<Sidecar> {
        val stem = Ps1SafText.baseName(imageName).substringBeforeLast('.', imageName)
        val localStem = localName.substringBeforeLast('.')
        // PBP uses _1 through _5 for embedded discs, not the playlist's basename.
        val suffixes = if (Ps1SafText.extension(imageName) == "pbp")
            listOf("") + (1..5).map { "_$it" } else listOf("")
        return suffixes.mapNotNull { suffix ->
            val expected = "$stem$suffix.sbi"
            val matches = siblings.filter { it.equals(expected, ignoreCase = true) }
            val source = matches.firstOrNull { it == expected } ?: matches.singleOrNull()
            if (source == null && matches.isNotEmpty()) throw IOException("Ambiguous SBI files for $expected")
            source?.let { Sidecar(it, "$localStem$suffix.sbi") }
        }
    }

    /** Validate the format this core actually supports; existence alone isn't a loaded SBI. */
    fun validate(bytes: ByteArray) {
        fun invalid(): Nothing = throw IOException("Invalid or incomplete SBI file; use the SBI matching this disc and region")
        if (bytes.size > MAX_BYTES || bytes.size < 18 || (bytes.size - 4) % 14 != 0 ||
            bytes[0] != 'S'.code.toByte() || bytes[1] != 'B'.code.toByte() ||
            bytes[2] != 'I'.code.toByte() || bytes[3] != 0.toByte()) invalid()
        fun bcd(offset: Int): Int {
            val value = bytes[offset].toInt() and 255
            if ((value and 15) > 9 || (value shr 4) > 9) invalid()
            return (value shr 4) * 10 + (value and 15)
        }
        for (offset in 4 until bytes.size step 14) {
            bcd(offset)
            if (bcd(offset + 1) >= 60 || bcd(offset + 2) >= 75 || bytes[offset + 3] != 1.toByte()) invalid()
        }
    }

    fun stage(
        imageName: String,
        localImage: File,
        siblings: Collection<String>,
        open: (String) -> InputStream,
        cancelled: () -> Boolean,
    ): Int {
        val sidecars = sidecars(imageName, localImage.name, siblings)
        for (sidecar in sidecars) {
            fun checkCancelled() {
                if (cancelled() || Thread.currentThread().isInterrupted) throw IOException("Game preparation cancelled")
            }
            checkCancelled()
            val bytes = open(sidecar.source).use { input ->
                val output = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(4096)
                while (true) {
                    checkCancelled()
                    val count = input.read(buffer)
                    if (count < 0) break
                    if (output.size() + count > MAX_BYTES) throw IOException("SBI file is unexpectedly large")
                    output.write(buffer, 0, count)
                }
                output.toByteArray()
            }
            try { validate(bytes) } catch (failure: IOException) {
                throw IOException("${sidecar.source}: ${failure.message}", failure)
            }
            val target = File(localImage.parentFile, sidecar.destination)
            val partial = File(localImage.parentFile, "${sidecar.destination}.part")
            try {
                checkCancelled()
                partial.writeBytes(bytes)
                checkCancelled()
                if (!partial.renameTo(target)) throw IOException("Unable to prepare the SBI file")
            } finally { partial.delete() }
        }
        return sidecars.size
    }
}
