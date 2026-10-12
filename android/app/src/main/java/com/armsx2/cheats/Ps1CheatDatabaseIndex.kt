package com.armsx2.cheats

import java.io.Reader
import java.text.Normalizer
import java.util.Locale

data class Ps1CheatDatabaseItem(
    val id: String,
    val title: String,
    val automaticSerials: Set<String>,
    val relatedSerials: Set<String>,
    val cheatCount: Int,
    val omittedCount: Int,
)

/** A cheat lookup, not a ROM identity lookup. Unknown region/revision is never auto-selected. */
class Ps1CheatDatabaseIndex(reader: Reader) {
    val items: List<Ps1CheatDatabaseItem> = reader.buffered().lineSequence().mapNotNull { line ->
        val fields = line.split('\t')
        if (fields.size != 6 || !fields[0].matches(Regex("[a-f0-9]{20}"))) null else {
            val count = fields[4].toIntOrNull() ?: 0
            if (count <= 0) null else Ps1CheatDatabaseItem(
                fields[0], fields[1], serialSet(fields[2]), serialSet(fields[3]),
                count, fields[5].toIntOrNull() ?: 0,
            )
        }
    }.toList()

    fun automatic(serial: String?): Ps1CheatDatabaseItem? {
        val identity = normaliseSerial(serial) ?: return null
        val matches = items.filter { identity in it.automaticSerials }
        val preferred = matches.filter { it.title.contains("(GameShark)", ignoreCase = true) }
        // Multiple lists for the same device may be different versions; don't guess.
        return (preferred.ifEmpty { matches }).singleOrNull()
    }

    fun suggestions(serial: String?, title: String?): List<Ps1CheatDatabaseItem> {
        val identity = normaliseSerial(serial)
        val key = title?.let(::titleKey).orEmpty()
        return items.filter { (identity != null && identity in it.relatedSerials) ||
            (key.isNotEmpty() && titleKey(it.title) == key) }
            .sortedByDescending { identity != null && identity in it.automaticSerials }
    }

    fun search(query: String): List<Ps1CheatDatabaseItem> {
        val words = query.lowercase(Locale.ROOT).trim().split(Regex("\\s+")).filter(String::isNotEmpty)
        return items.filter { item ->
            val text = (item.title + " " + item.relatedSerials.joinToString(" ")).lowercase(Locale.ROOT)
            words.all { it in text }
        }
    }

    companion object {
        fun normaliseSerial(raw: String?): String? {
            val match = Regex("([A-Z]{4})[-_ ]?(\\d{3})\\.?(\\d{2})")
                .matchEntire(raw?.trim()?.uppercase(Locale.ROOT) ?: return null) ?: return null
            return "${match.groupValues[1]}-${match.groupValues[2]}${match.groupValues[3]}"
        }

        private fun serialSet(raw: String): Set<String> =
            raw.split(',').mapNotNull(::normaliseSerial).toSet()

        private fun titleKey(raw: String): String =
            Normalizer.normalize(raw.replace(Regex("[\\[(][^\\])]*[\\])]"), " "), Normalizer.Form.NFKD)
                .lowercase(Locale.ROOT).replace(Regex("[^a-z0-9]"), "")
    }
}

/** Append-only merging: existing custom codes and identically named selections always win. */
object Ps1CheatCatalogue {
    data class Merge(val text: String, val added: Int, val addedNames: List<String> = emptyList()) {
        fun selectionAfterImport(selected: List<String>): List<String> = selected.filterNot { name ->
            addedNames.any { it.equals(name.trim(), ignoreCase = true) }
        }
    }
    private val HEADER = Regex("^[\\t \\uFEFF]*\\[([^]\\r\\n]+)][^\\r\\n]*", RegexOption.MULTILINE)

    fun merge(existing: String, incoming: String): Merge {
        val names = HEADER.findAll(existing).map { it.groupValues[1].trim().lowercase(Locale.ROOT) }.toMutableSet()
        val headers = HEADER.findAll(incoming).toList()
        val addedNames = ArrayList<String>()
        val added = headers.mapIndexedNotNull { index, header ->
            if (!names.add(header.groupValues[1].trim().lowercase(Locale.ROOT))) null
            else {
                addedNames += header.groupValues[1].trim()
                incoming.substring(header.range.first, headers.getOrNull(index + 1)?.range?.first ?: incoming.length)
            }
        }
        if (added.isEmpty()) return Merge(existing, 0)
        return Merge(existing + (if (existing.isEmpty() || existing.endsWith('\n')) "" else "\n") +
            added.joinToString("\n"), added.size, addedNames)
    }
}
