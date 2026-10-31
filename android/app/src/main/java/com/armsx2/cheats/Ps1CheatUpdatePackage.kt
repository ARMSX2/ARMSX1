package com.armsx2.cheats

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/** Data only: no arbitrary paths, extraction, ROMs, executable libraries or settings. */
class Ps1CheatUpdatePackage private constructor(
    val index: Ps1CheatDatabaseIndex,
    val bodies: Map<String, String>,
    val digest: String,
    val notice: String,
) {
    companion object {
        const val MAX_DOWNLOAD = 12 * 1024 * 1024
        private const val MAX_LIST = 2_000_000
        val FILES = setOf("ps1-cheats.zip", "ps1-cheats-index.tsv", "ps1-cheats-NOTICE.txt",
            "ps1-cheats-LICENSE.txt", "ps1-cover-titles.tsv", "ps1-cover-titles-NOTICE.txt",
            "ps1-cover-titles-LICENSE.txt")

        fun sha256(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
            .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }

        fun readBounded(input: InputStream, limit: Int, checkCancelled: () -> Unit = {}): ByteArray {
            val out = ByteArrayOutputStream()
            val buffer = ByteArray(16 * 1024)
            while (true) {
                checkCancelled()
                val count = input.read(buffer)
                if (count < 0) break
                require(count <= limit - out.size()) { "Cheat download exceeds size limit" }
                out.write(buffer, 0, count)
            }
            return out.toByteArray()
        }

        private fun utf8(bytes: ByteArray): String = Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes)).toString().also { require('\u0000' !in it) }

        fun parse(bytes: ByteArray): Ps1CheatUpdatePackage {
            require(bytes.size in 1..MAX_DOWNLOAD)
            val files = mutableMapOf<String, ByteArray>()
            var total = 0
            ZipInputStream(ByteArrayInputStream(bytes)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    require(!entry.isDirectory && entry.name in FILES + setOf("format-version.txt", "hashes.tsv"))
                    require(entry.name !in files) { "Duplicate package file" }
                    val limit = when (entry.name) {
                        "ps1-cheats.zip" -> 8 * 1024 * 1024
                        "ps1-cover-titles.tsv", "ps1-cheats-index.tsv" -> 4 * 1024 * 1024
                        else -> 128 * 1024
                    }
                    val body = readBounded(zip, limit)
                    total += body.size
                    require(total <= 20 * 1024 * 1024)
                    files[entry.name] = body
                }
            }
            require(files.keys == FILES + setOf("format-version.txt", "hashes.tsv")) { "Incomplete cheat package" }
            require(utf8(files.getValue("format-version.txt")).trim() == "1") { "Unsupported cheat package version" }
            val hashRows = utf8(files.getValue("hashes.tsv")).lineSequence().filter { it.isNotBlank() }.map {
                val fields = it.split('\t')
                require(fields.size == 2 && fields[0] in FILES && fields[1].matches(Regex("[a-f0-9]{64}")))
                fields[0] to fields[1]
            }.toList()
            require(hashRows.size == FILES.size && hashRows.map { it.first }.toSet() == FILES)
            for ((name, hash) in hashRows) require(sha256(files.getValue(name)) == hash) { "Cheat package checksum mismatch" }

            val indexText = utf8(files.getValue("ps1-cheats-index.tsv"))
            val rows = indexText.lineSequence().filter { it.isNotBlank() && !it.startsWith('#') }.toList()
            require(rows.size in 1..10_000)
            for (row in rows) {
                val fields = row.split('\t')
                require(fields.size == 6 && fields[0].matches(Regex("[a-f0-9]{20}")))
                require(fields[1].isNotBlank() && fields[1].length <= 512)
                for (serials in fields.slice(2..3)) require(serials.isEmpty() || serials.split(',').all {
                    Ps1CheatDatabaseIndex.normaliseSerial(it) == it
                })
                require((fields[4].toIntOrNull() ?: 0) in 1..100_000)
                require((fields[5].toIntOrNull() ?: -1) >= 0)
            }
            val metadata = utf8(files.getValue("ps1-cover-titles.tsv"))
            require(metadata.lineSequence().filter { it.isNotBlank() && !it.startsWith('#') }.all {
                val fields = it.split('\t')
                fields.size == 3 && fields[0].isNotBlank() && fields[1].isNotBlank() &&
                    Ps1CheatDatabaseIndex.normaliseSerial(fields[2]) == fields[2]
            })
            val index = Ps1CheatDatabaseIndex(indexText.reader(), metadata.reader())
            require(index.items.size == rows.size && index.items.map { it.id }.toSet().size == rows.size)
            val wanted = index.items.associateBy { it.id + ".cht" }
            val bodies = mutableMapOf<String, String>()
            total = 0
            ZipInputStream(ByteArrayInputStream(files.getValue("ps1-cheats.zip"))).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    val item = wanted[entry.name] ?: error("Unexpected cheat archive entry")
                    require(!entry.isDirectory && item.id !in bodies)
                    val body = readBounded(zip, MAX_LIST)
                    total += body.size
                    require(total <= 32 * 1024 * 1024)
                    val text = utf8(body)
                    require(Ps1CheatCatalogue.merge("", text).added == item.cheatCount) { "Cheat count mismatch" }
                    bodies[item.id] = text
                }
            }
            require(bodies.size == wanted.size) { "Missing cheat lists" }
            for (name in FILES.filter { it.endsWith(".txt") }) require(utf8(files.getValue(name)).isNotBlank())
            return Ps1CheatUpdatePackage(index, bodies, sha256(bytes),
                utf8(files.getValue("ps1-cheats-NOTICE.txt")) + "\n" + utf8(files.getValue("ps1-cover-titles-NOTICE.txt")))
        }
    }
}
