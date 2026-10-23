package com.armsx2.core

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import com.armsx2.GameInfo
import java.io.File
import java.io.InputStream
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/** Bounded, non-destructive bulk import. All entry points run under the card-session lock. */
internal object Ps1BulkCards {
    private const val MAX_ENTRIES = 4096
    private const val MAX_CARDS = 512
    private const val MAX_BYTES = 96L * 1024 * 1024
    private val extensions = setOf("mcd", "mcr", "mc", "srm")
    data class Result(val cards: Int, val assigned: Int, val unmatched: Int, val skipped: Int) {
        override fun toString() = "$cards cards imported; $assigned game slots assigned. " +
            "$unmatched cards unassigned; $skipped invalid or unsupported files skipped. " +
            "Existing card files were not overwritten. Unassigned cards remain in the card folder."
    }
    private data class Card(val name: String, val staged: File)

    fun import(context: Context, uri: Uri, folder: Boolean, games: List<GameInfo>): Result {
        val staging = java.nio.file.Files.createTempDirectory(context.cacheDir.toPath(), "bulk-cards-").toFile()
        try {
            val cards = ArrayList<Card>()
            var skipped = 0
            val reader = Ps1CardImportReader(MAX_BYTES, MAX_ENTRIES)
            fun read(name: String, input: InputStream) {
                val data = reader.read(name, input)
                if (data == null) { skipped++; return }
                check(cards.size < MAX_CARDS) { "Too many memory cards (limit $MAX_CARDS)." }
                val file = File(staging, "${cards.size}.mcd")
                file.outputStream().use { it.write(data); it.fd.sync() }
                cards += Card(name.replace('\\', '/').substringAfterLast('/'), file)
            }
            if (folder) {
                val root = DocumentFile.fromTreeUri(context, uri) ?: throw IOException("Cannot open the selected folder.")
                val visited = HashSet<String>()
                var nodes = 0
                fun visit(file: DocumentFile, depth: Int) {
                    check(depth <= 12 && ++nodes <= MAX_ENTRIES) { "The selected folder is too large or too deeply nested." }
                    if (!visited.add(file.uri.toString())) return
                    if (file.isDirectory) file.listFiles().forEach { visit(it, depth + 1) }
                    else if (file.name.orEmpty().substringAfterLast('.', "").lowercase() in extensions) {
                        context.contentResolver.openInputStream(file.uri)?.use { read(file.name.orEmpty(), it) }
                            ?: throw IOException("Cannot read ${file.name}.")
                    } else skipped++
                }
                visit(root, 0)
            } else {
                context.contentResolver.openInputStream(uri)?.use { stream -> ZipInputStream(stream).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        read(entry.name, zip)
                        zip.closeEntry()
                    }
                } } ?: throw IOException("Cannot open the selected ZIP.")
            }
            check(cards.isNotEmpty()) { "No raw 128 KiB PS1 cards found. Save states and individual-save files are not memory-card images." }
            val identities = games.map { game -> Ps1BulkCardMatcher.Game(game.uri.toString(), game.serial,
                listOf(game.title, game.uri.lastPathSegment.orEmpty().substringAfterLast('/').substringBeforeLast('.'))) }
            val root = Ps1MemoryCards.cardDirectory(context).apply { mkdirs() }
            val pending = linkedMapOf<Pair<String, Int>, MutableList<File>>()
            var unmatched = 0
            for (card in cards) {
                // Unique names keep both a previous import and all existing game cards intact.
                val clean = card.name.substringBeforeLast('.').replace(Regex("[^\\p{L}\\p{N} ._()-]"), "_").take(80)
                val hash = MessageDigest.getInstance("SHA-256").digest(card.staged.readBytes())
                    .take(6).joinToString("") { "%02x".format(it.toInt() and 255) }
                var destination = File(root, "Imported-$clean-$hash.mcd")
                var counter = 1
                while (destination.exists()) destination = File(root, "Imported-$clean-$hash-${counter++}.mcd")
                val temporary = File.createTempFile(".bulk-import-", ".tmp", root)
                try {
                    temporary.outputStream().use { out ->
                        card.staged.inputStream().use { it.copyTo(out) }; out.fd.sync()
                    }
                    java.nio.file.Files.move(temporary.toPath(), destination.toPath())
                } finally { temporary.delete() }
                val matches = Ps1BulkCardMatcher.match(card.name, identities)
                if (matches.isEmpty()) unmatched++
                matches.forEach { match -> pending.getOrPut(match.source to match.slot) { arrayListOf() }.add(destination) }
            }
            var assigned = 0
            pending.entries.groupBy { it.key.first }.forEach { (source, slots) ->
                val unique = slots.filter { it.value.size == 1 }.associate { it.key.second to it.value.single() }
                if (unique.isNotEmpty()) { Ps1CardBindings.rememberImported(root, source, unique); assigned += unique.size }
                unmatched += slots.count { it.value.size > 1 }
            }
            return Result(cards.size, assigned, unmatched, skipped)
        } finally {
            // Only files created by this invocation in its validated cache directory.
            staging.listFiles()?.forEach { it.delete() }
            staging.delete()
        }
    }
}
