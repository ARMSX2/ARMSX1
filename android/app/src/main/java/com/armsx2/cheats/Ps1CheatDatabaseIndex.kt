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
class Ps1CheatDatabaseIndex(reader: Reader, metadata: Reader? = null) {
    private val discNames = mutableMapOf<String, MutableSet<String>>()
    private val discRegions = mutableMapOf<String, MutableSet<String>>()
    init {
        metadata?.buffered()?.lineSequence()?.forEach { line ->
            val fields = line.split('\t')
            if (fields.size == 3 && !line.startsWith('#')) {
                normaliseSerial(fields[2])?.let { serial ->
                    discNames.getOrPut(serial) { linkedSetOf() }.add(fields[0])
                    discRegions.getOrPut(serial) { linkedSetOf() }.add(fields[1].lowercase(Locale.ROOT))
                }
            }
        }
    }
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
    private val automaticBySerial = items.flatMap { item -> item.automaticSerials.map { it to item } }
        .groupBy({ it.first }, { it.second })
    private val relatedBySerial = items.flatMap { item -> item.relatedSerials.map { it to item } }
        .groupBy({ it.first }, { it.second })
    private val regionsByItem = items.associate { it.id to regionTags(it.title) }
    private val kindsByItem = items.associate { it.id to releaseKind(it.title) }
    private val kindsBySerial = discNames.mapValues { (_, names) -> names.map(::releaseKind).toSet() }
    private val searchByItem = items.associate { item ->
        val titles = listOf(item.title) + item.relatedSerials.flatMap { discNames[it].orEmpty() }
        item.id to (searchText(titles.joinToString(" ") + " " + item.relatedSerials.joinToString(" ")) +
            " " + titles.joinToString(" ", transform = ::titleKey))
    }
    private val byTitle = items.flatMap { item ->
        val keys = setOf(titleKey(item.title)) + item.relatedSerials.flatMap { serial -> discNames[serial].orEmpty().map(::titleKey) }
        keys.map { it to item }
    }.groupBy({ it.first }, { it.second })
    val coveredGameSerialCount: Int get() = relatedBySerial.size
    val knownGameSerialCount: Int get() = discNames.size

    fun automatic(serial: String?): Ps1CheatDatabaseItem? {
        val identity = normaliseSerial(serial) ?: return null
        val matches = automaticBySerial[identity].orEmpty().filter { !isIncompatible(it, identity) }
        val preferred = matches.filter { it.title.contains("(GameShark)", ignoreCase = true) }
        // Multiple lists for the same device may be different versions; don't guess.
        return (preferred.ifEmpty { matches }).singleOrNull()
    }

    /** All verified formats, without guessing between competing versions of one format. */
    fun automaticCatalogues(serial: String?): List<Ps1CheatDatabaseItem> {
        val identity = normaliseSerial(serial) ?: return emptyList()
        return automaticBySerial[identity].orEmpty().filter { !isIncompatible(it, identity) }
            .groupBy { formatLabel(it.title).lowercase(Locale.ROOT) }
            .values.mapNotNull { it.singleOrNull() }
    }

    fun suggestions(serial: String?, title: String?): List<Ps1CheatDatabaseItem> {
        val identity = normaliseSerial(serial)
        val key = title?.let(::titleKey).orEmpty()
        val exact = relatedBySerial[identity].orEmpty()
        val titled = if (key.isEmpty()) emptyList() else byTitle[key].orEmpty()
            .filter { kindsByItem[it.id] == releaseKind(title.orEmpty()) }
        return (exact + titled).distinctBy { it.id }.filter { !isIncompatible(it, identity) }
            .sortedByDescending { identity != null && identity in it.automaticSerials }
    }

    /** An explicitly different region must never be substituted for a known game. */
    fun isRegionIncompatible(item: Ps1CheatDatabaseItem, serial: String?): Boolean {
        val identity = normaliseSerial(serial) ?: return false
        if (identity in item.relatedSerials || identity in item.automaticSerials) return false
        val source = regionsByItem[item.id].orEmpty()
        if (source.isEmpty()) return false // World/untagged remains confirmation-only, not proven compatible.
        val actual = discRegions[identity] ?: when (identity.take(4)) {
            "SLUS", "SCUS" -> setOf("usa", "canada")
            "SLPS", "SCPS", "SLPM" -> setOf("japan", "asia")
            "SLES", "SCES" -> EUROPE_REGIONS
            else -> return false
        }
        return actual.intersect(source).isEmpty()
    }

    fun isIncompatible(item: Ps1CheatDatabaseItem, serial: String?): Boolean {
        if (isRegionIncompatible(item, serial)) return true
        val names = discNames[normaliseSerial(serial)].orEmpty()
        if (names.isEmpty()) return false
        if (kindsByItem[item.id] !in kindsBySerial[normaliseSerial(serial)].orEmpty()) return true
        val sourceDisc = DISC.find(item.title)?.groupValues?.get(1) ?: return false
        val knownDiscs = names.mapNotNull { DISC.find(it)?.groupValues?.get(1) }
        return knownDiscs.isNotEmpty() && sourceDisc !in knownDiscs
    }

    fun search(query: String): List<Ps1CheatDatabaseItem> {
        val words = searchText(query).split(Regex("\\s+")).filter(String::isNotEmpty)
        return items.filter { item ->
            val text = searchByItem[item.id].orEmpty()
            words.all { it in text }
        }
    }

    companion object {
        private val FORMAT = Regex("\\((GameShark|Game\\s*Buster|Xploder|Action\\s*Replay)\\)$", RegexOption.IGNORE_CASE)
        fun formatLabel(title: String): String = FORMAT.find(title)?.groupValues?.get(1)
            ?.replace(Regex("\\s+"), " ") ?: "Other"
        private val SERIAL = Regex("([A-Z]{4})[-_. ]?(\\d{3})[-_. ]?(\\d{2})(?:;1)?")
        private val SERIAL_IN_TEXT = Regex("([A-Za-z]{4})[-_. ]?(\\d{3})[-_.]?(\\d{2})")
        private val PARTIAL_SERIAL = Regex("^S[A-Z]{3}[-_. ]*\\d{0,5}[-_.]?\\d{0,2}$", RegexOption.IGNORE_CASE)
        private val DISC = Regex("[\\[(]Disc\\s+(\\d+)[\\])]", RegexOption.IGNORE_CASE)
        private val RELEASE_KINDS = listOf("demo", "prototype", "beta", "sample", "trial", "taikenban", "otameshi", "cancelled")
            .associateWith { Regex("\\b$it\\b") }
        private val ROMAN = mapOf("ii" to "2", "iii" to "3", "iv" to "4", "vi" to "6", "vii" to "7",
            "viii" to "8", "ix" to "9", "xi" to "11", "xii" to "12")
        fun normaliseSerial(raw: String?): String? {
            val match = SERIAL.matchEntire(raw?.trim()?.uppercase(Locale.ROOT) ?: return null) ?: return null
            return "${match.groupValues[1]}-${match.groupValues[2]}${match.groupValues[3]}"
        }

        private fun serialSet(raw: String): Set<String> =
            raw.split(',').mapNotNull(::normaliseSerial).toSet()

        private val EUROPE_REGIONS = setOf("europe", "uk", "australia", "germany", "france", "italy", "spain",
            "netherlands", "sweden", "austria", "belgium", "denmark", "finland", "norway", "portugal")
        private val REGIONS = EUROPE_REGIONS + setOf("usa", "canada", "japan", "asia", "korea")
        private val TAGS = Regex("[\\[(]([^\\])]+)[\\])]")
        private val REGION_SUFFIX = Regex("\\s+(USA|Europe|Japan|NTSC-U|NTSC-J|PAL)$", RegexOption.IGNORE_CASE)
        private fun regionTags(raw: String): Set<String> {
            val tags = TAGS.findAll(raw).flatMap { it.groupValues[1].lowercase(Locale.ROOT).split(',').asSequence() }
                .map(String::trim).filter { it in REGIONS }.toMutableSet()
            REGION_SUFFIX.find(TAGS.replace(raw, " ").trim())?.let {
                tags += when (val suffix = it.groupValues[1].lowercase(Locale.ROOT)) {
                    "ntsc-u" -> "usa"; "ntsc-j" -> "japan"; "pal" -> "europe"; else -> suffix
                }
            }
            return tags
        }
        private fun releaseKind(raw: String): String {
            val tags = TAGS.findAll(raw).joinToString(" ") { it.groupValues[1] }.lowercase(Locale.ROOT)
            return RELEASE_KINDS.entries.firstOrNull { it.value.containsMatchIn(tags) }?.key ?: "retail"
        }
        private fun searchText(raw: String): String {
            val serials = SERIAL_IN_TEXT.replace(raw) {
                "${it.groupValues[1]}-${it.groupValues[2]}${it.groupValues[3]}"
            }
            val cleaned = if (PARTIAL_SERIAL.matches(serials.trim())) {
                serials.replace('_', '-').replace('.', '-')
            } else serials.replace("&", " and ").replace("_", " and ")
            return Normalizer.normalize(cleaned, Normalizer.Form.NFKD).lowercase(Locale.ROOT)
                .replace(Regex("\\p{M}+"), "").replace(Regex("[^a-z0-9]+"), " ").trim()
        }
        // Title variants are suggestions only; the automatic lookup remains strict serial-based.
        private fun titleKey(raw: String): String {
            val title = REGION_SUFFIX.replace(TAGS.replace(raw, " ").trim(), "").replace("&", " and ")
            return searchText(title).split(' ').joinToString("") { ROMAN[it] ?: it }
        }
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
    private val CODE = Regex("^([0-9a-fA-F]{8})\\s*[:,]?\\s*([0-9a-fA-F]{1,8})(?:\\s*(?:#|;|//).*)?$")

    /** Preserve distinct variants across formats and deduplicate identical code bodies. */
    fun mergeCatalogues(existing: String, catalogues: List<Pair<String, String>>): Merge {
        fun signature(body: String): String = body.lineSequence().mapNotNull { line ->
            CODE.matchEntire(line.trim())?.let {
                it.groupValues[1].uppercase(Locale.ROOT) + " " +
                    it.groupValues[2].uppercase(Locale.ROOT).trimStart('0').padStart(4, '0')
            }
        }.joinToString("\n")
        val names = mutableSetOf<String>()
        val signatures = mutableSetOf<String>()
        fun groups(text: String): List<Pair<String, String>> {
            val headers = HEADER.findAll(text).toList()
            return headers.mapIndexed { index, header ->
                header.groupValues[1].trim() to text.substring(header.range.first,
                    headers.getOrNull(index + 1)?.range?.first ?: text.length)
            }
        }
        groups(existing).forEach { (name, body) ->
            names += name.lowercase(Locale.ROOT)
            signature(body).takeIf(String::isNotEmpty)?.let(signatures::add)
        }
        val addedNames = mutableListOf<String>()
        val additions = mutableListOf<String>()
        for ((source, text) in catalogues) for ((name, body) in groups(text)) {
            val codes = signature(body)
            if (codes.isNotEmpty() && !signatures.add(codes)) continue
            val safeSource = source.replace(Regex("[\\[\\]\\r\\n]"), " ").trim().ifBlank { "Database" }
            var unique = name
            var variant = 1
            while (unique.lowercase(Locale.ROOT) in names) {
                val suffix = " ($safeSource" + (if (variant == 1) "" else " $variant") + ")"
                var stem = name
                while ((stem + suffix).toByteArray(Charsets.UTF_8).size > 150 && stem.isNotEmpty()) stem = stem.dropLast(1)
                unique = stem + suffix
                variant++
            }
            names += unique.lowercase(Locale.ROOT)
            addedNames += unique
            additions += body.replaceRange(requireNotNull(HEADER.find(body)).range, "[$unique]")
        }
        if (additions.isEmpty()) return Merge(existing, 0)
        return Merge(existing + (if (existing.isEmpty() || existing.endsWith('\n')) "" else "\n") +
            additions.joinToString("\n"), addedNames.size, addedNames)
    }

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
