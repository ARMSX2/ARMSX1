package com.armsx2.textures

import java.io.DataInputStream
import java.io.File
import java.io.InputStream
import java.security.MessageDigest
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.zip.CRC32
import java.util.zip.ZipInputStream

/** Normalized SwanStation VRAM-write PNG packs, not old psxe or newer incompatible formats. */
object TexturePackArchive {
    private val TEXTURE = Regex("replacements/vram-write-[a-f0-9]{32}\\.png")
    private val EXTRA = setOf("LICENSE.txt", "NOTICE.txt")
    private const val MAX_IMAGE = 64L * 1024 * 1024

    fun digest(file: File, checkCancelled: () -> Unit = {}): String {
        val hash = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                checkCancelled()
                val n = input.read(buffer)
                if (n < 0) break
                hash.update(buffer, 0, n)
            }
        }
        return hash.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
    }

    fun extract(zipFile: File, destination: File, pack: TexturePack, checkCancelled: () -> Unit = {}): Int {
        require(zipFile.length() == pack.downloadBytes && digest(zipFile, checkCancelled) == pack.sha256) { "Texture checksum mismatch" }
        require(destination.isDirectory && destination.listFiles()?.isEmpty() == true)
        val names = hashSetOf<String>()
        var total = 0L
        var images = 0
        ZipInputStream(zipFile.inputStream().buffered()).use { zip ->
            while (true) {
                checkCancelled()
                val entry = zip.nextEntry ?: break
                require(names.add(entry.name) && names.size <= 100_002) { "Duplicate or excessive archive entries" }
                if (entry.isDirectory) {
                    require(entry.name == "replacements/")
                    continue
                }
                require(TEXTURE.matches(entry.name) || entry.name in EXTRA) { "Unsupported texture archive path" }
                val target = File(destination, entry.name)
                require(target.canonicalPath.startsWith(destination.canonicalPath + File.separator))
                require(target.parentFile!!.isDirectory || target.parentFile!!.mkdirs())
                var size = 0L
                val limit = if (entry.name in EXTRA) 128L * 1024 else MAX_IMAGE
                target.outputStream().buffered().use { out ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        checkCancelled()
                        val n = zip.read(buffer)
                        if (n < 0) break
                        size += n; total += n
                        require(size <= limit && total <= pack.expandedBytes) { "Texture archive exceeds size limit" }
                        out.write(buffer, 0, n)
                    }
                }
                require(size > 0)
                if (entry.name in EXTRA) {
                    val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(target.readBytes())).toString()
                    require(text.isNotBlank() && '\u0000' !in text)
                }
                if (TEXTURE.matches(entry.name)) {
                    target.inputStream().buffered().use { validatePng(it, checkCancelled) }
                    images++
                }
            }
        }
        require(images > 0 && "LICENSE.txt" in names && total == pack.expandedBytes) { "Incomplete texture pack" }
        return images
    }

    /** Bounded dimensions and complete chunk CRCs before handing PNGs to the native decoder. */
    fun validatePng(stream: InputStream, checkCancelled: () -> Unit = {}) {
        val input = DataInputStream(stream)
        val signature = ByteArray(8).also(input::readFully)
        require(signature.contentEquals(byteArrayOf(137.toByte(), 80, 78, 71, 13, 10, 26, 10)))
        var header = false
        var pixels = false
        var chunks = 0
        while (true) {
            checkCancelled()
            require(++chunks <= 100_000)
            val length = input.readInt()
            require(length in 0..64 * 1024 * 1024)
            val kind = ByteArray(4).also(input::readFully)
            val name = String(kind, Charsets.US_ASCII)
            require(kind.all { it.toInt().toChar() in 'A'..'Z' || it.toInt().toChar() in 'a'..'z' })
            if (!header) require(name == "IHDR" && length == 13)
            else require(name != "IHDR")
            val crc = CRC32().apply { update(kind) }
            if (name == "IHDR") {
                val data = ByteArray(13).also(input::readFully)
                crc.update(data)
                val fields = DataInputStream(data.inputStream())
                val width = fields.readInt(); val height = fields.readInt()
                require(width in 1..8192 && height in 1..8192 && width.toLong() * height <= 16_777_216)
                val depth = fields.readUnsignedByte(); val color = fields.readUnsignedByte()
                val depths = when (color) { 0 -> setOf(1, 2, 4, 8, 16); 2, 4, 6 -> setOf(8, 16); 3 -> setOf(1, 2, 4, 8); else -> emptySet() }
                require(depth in depths && fields.readUnsignedByte() == 0 && fields.readUnsignedByte() == 0 && fields.readUnsignedByte() in 0..1)
                header = true
            } else {
                val buffer = ByteArray(16 * 1024)
                var remaining = length
                while (remaining > 0) {
                    checkCancelled()
                    val n = minOf(remaining, buffer.size)
                    input.readFully(buffer, 0, n); crc.update(buffer, 0, n); remaining -= n
                }
                if (name == "IDAT" && length > 0) pixels = true
            }
            require(input.readInt().toLong() and 0xffffffffL == crc.value) { "Invalid PNG checksum" }
            if (name == "IEND") {
                require(length == 0 && pixels && input.read() == -1)
                return
            }
        }
    }
}
