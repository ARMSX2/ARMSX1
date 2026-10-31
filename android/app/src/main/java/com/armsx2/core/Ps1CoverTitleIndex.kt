package com.armsx2.core

import java.io.Reader
import java.text.Normalizer
import java.util.Locale

/** Filename metadata for artwork only. Never supplies an emulator/achievement identity. */
class Ps1CoverTitleIndex(reader: Reader) {
    private data class Key(val title: String, val region: String, val disc: Int)
    private data class Candidate(val serial: String, val rank: Int)
    private val entries = HashMap<Key, MutableList<Candidate>>()
    private val keysBySerial = HashMap<String, MutableSet<Key>>()
    private val regions = HashSet<String>()
    val size: Int get() = entries.size

    init {
        reader.buffered().forEachLine { line ->
            if (!line.startsWith('#')) {
                val fields = line.split('\t')
                if (fields.size == 3 && SERIAL.matches(fields[2])) {
                    val (name, region, serial) = fields
                    val title = normaliseTitle(name)
                    val disc = discNumber(name)
                    val recordRegions = (listOf(region) + TAGS.findAll(name)
                        .flatMap { it.value.trim('(', ')', '[', ']').split(',').asSequence() }
                        .filter { canonicalRegion(it) in KNOWN_REGIONS }.toList())
                        .map(::canonicalRegion).distinct()
                    // Prefer an original pressing over a reissue/revision of the same title.
                    val rank = (if (REISSUE.containsMatchIn(name)) 10 else 0) +
                        (if (region == "Japan" && !serial.startsWith("SLPS") && !serial.startsWith("SLPM")) 1 else 0)
                    for (r in recordRegions) {
                        regions.add(r)
                        val key = Key(title, r, disc)
                        val editions = entries.getOrPut(key) { ArrayList() }
                        if (editions.none { it.serial == serial }) editions.add(Candidate(serial, rank))
                        keysBySerial.getOrPut(serial) { LinkedHashSet() }.add(key)
                    }
                }
            }
        }
        entries.values.forEach { it.sortWith(compareBy<Candidate> { c -> c.rank }.thenBy { c -> c.serial }) }
    }

    fun find(title: String?, name: String?): String? = findAll(title, name).firstOrNull()

    /** Keep alternate pressings of the SAME title/region/disc for missing-art fallback. */
    fun findAll(title: String?, name: String?): List<String> {
        val source = name?.takeIf(String::isNotBlank) ?: title?.takeIf(String::isNotBlank) ?: return emptyList()
        val stem = source.substringAfterLast('/').substringAfterLast(':').replace(EXTENSION, "")
        val region = TAGS.findAll(stem).flatMap {
            it.value.trim('(', ')', '[', ']').split(',').asSequence()
        }.map(::canonicalRegion).firstOrNull { it in KNOWN_REGIONS || it in regions }
        val disc = discNumber(stem)
        val titles = listOfNotNull(normaliseTitle(stem), title?.let(::normaliseTitle)).distinct()
        for (key in titles) {
            if (region != null) {
                entries[Key(key, region, disc)]?.let { return it.map(Candidate::serial) }
            } else {
                entries[Key(key, "usa", disc)]?.let { return it.map(Candidate::serial) }
                // An untagged Japan-only game can resolve; ambiguous regional editions cannot.
                val matches = regions.mapNotNull { entries[Key(key, it, disc)] }
                val preferred = matches.mapNotNull { it.firstOrNull()?.serial }.distinct()
                if (preferred.size == 1) return matches.flatten().map(Candidate::serial).distinct()
            }
        }
        return emptyList()
    }

    fun alternatives(serial: String): List<String> = buildList {
        val keys = keysBySerial[serial].orEmpty()
        keys.forEach { key -> entries[key]?.forEach { add(it.serial) } }
        // Disc 1 often has the only box image. Only use it when the catalogue establishes
        // that the identified disc belongs to that same title and region.
        keys.filter { it.disc > 1 }.forEach { key ->
            entries[key.copy(disc = 1)]?.forEach { add(it.serial) }
        }
    }.distinct().filter { it != serial }.take(3)

    companion object {
        private val SERIAL = Regex("[A-Z]{4}-\\d{5}")
        private val TAGS = Regex("""[\[(][^\])]*[\])]""")
        private val DISC_TAG = Regex("""(?i)\b(?:disc|disk|cd)\s*[.\-]?\s*(\d{1,2})\b""")
        private val EXTENSION = Regex("(?i)\\.(?:chd|cue|bin|iso|img|pbp|m3u|zip|7z)$")
        private val REISSUE = Regex("(?i)\\b(?:rev|edition|best|hits)\\b")
        private val TRAILING_ARTICLE = Regex("(?i),\\s*(?:the|an|a)(?=\\s*-|$)")
        private val LEADING_ARTICLE = Regex("(?i)^\\s*(?:the|an|a)\\s+")
        private val ACCENT_MARKS = Regex("\\p{M}+")
        private val NON_ALNUM = Regex("[^a-z0-9]")
        private val SERIAL_TOKEN = Regex("(?i)(?<![a-z0-9])[a-z]{4}[ _-]?\\d{3}\\.?\\d{2}(?!\\d)")
        private val VERSION_TOKEN = Regex("(?i)\\b(?:v\\d+(?:\\.\\d+)+|rev(?:ision)?[ ._-]*\\d+)\\b")
        private val KNOWN_REGIONS = setOf("usa", "europe", "japan", "asia", "korea", "australia",
            "france", "germany", "italy", "spain", "netherlands", "sweden", "russia", "canada",
            "china", "taiwan", "hong kong", "brazil", "portugal", "denmark", "uk")

        private fun canonicalRegion(raw: String): String = when (val r = raw.trim().lowercase(Locale.US)) {
            "us", "ntsc-u", "ntscu" -> "usa"
            "eur", "pal" -> "europe"
            "jpn", "jp", "ntsc-j", "ntscj" -> "japan"
            else -> r
        }

        private fun discNumber(raw: String): Int =
            DISC_TAG.find(raw)?.groupValues?.get(1)?.toIntOrNull() ?: 1

        fun normaliseTitle(raw: String): String {
            val clean = DISC_TAG.replace(TAGS.replace(raw.replace(EXTENSION, ""), " "), " ")
                .replace(SERIAL_TOKEN, " ").replace(VERSION_TOKEN, " ").replace('_', ' ')
                .replace(TRAILING_ARTICLE, "")
                .replace(LEADING_ARTICLE, "")
            val key = Normalizer.normalize(clean, Normalizer.Form.NFD)
                .replace(ACCENT_MARKS, "").lowercase(Locale.US).replace(NON_ALNUM, "")
            return when (key) {
                "jurassicparkthelostworld", "jurassicparklostworld" -> "lostworldjurassicpark"
                else -> key
            }
        }
    }
}
