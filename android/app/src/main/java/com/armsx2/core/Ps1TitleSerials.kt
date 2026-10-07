package com.armsx2.core

import java.util.Locale
import java.io.Reader

/**
 * Last-resort **cover-art only** serial lookup, by No-Intro / Redump dump name.
 *
 * [Ps1DiscId] reads the serial off the disc itself and that is the answer wherever it works. This
 * lookup exists so a disc it *cannot* read — SAF metadata-only entries, a `.zip`, or an image
 * whose filesystem is damaged — can still display art without opening/staging the ROM.
 * A bundled Libretro/Redump catalogue provides broad regional coverage; the small curated
 * table below remains a fallback if the catalogue is unavailable.
 *
 * POSIX compressed images can be identified through the core's disc reader. SAF library
 * scanning deliberately avoids native disc probing, so those rows use the filename catalogue
 * until the selected game is launched and its actual disc identity becomes available.
 *
 * **Deliberately not fed into [com.armsx2.GameInfo.serial].** That field is the game's IDENTITY:
 * RetroAchievements hashes against it, per-game settings key off it, play time accrues under it.
 * A guess from a filename has no business there — a mis-detected title would silently attach one
 * game's achievements and settings to another. It is used by [com.armsx2.GameInfo.coverSerial] and
 * nothing else, so the worst a wrong entry here can do is show the wrong box art.
 *
 * The curated table is USA-only (or untagged filenames), with explicit regional exceptions below.
 * Other regional pressings are left unmatched rather than assigned an unrelated edition.
 */
object Ps1TitleSerials {

    @Volatile private var catalogue: Ps1CoverTitleIndex? = null

    /** Called off the UI thread. Existing disc identities and settings are never changed. */
    fun loadCatalogue(reader: Reader): Int {
        val index = Ps1CoverTitleIndex(reader)
        catalogue = index
        return index.size
    }

    /** Normalised title → serial per disc, disc 1 first. */
    private val BY_TITLE: Map<String, List<String>> = mapOf(
        "dragonwarriorvii" to listOf("SLUS-01206", "SLUS-01346"),
        "dragonwarrior7" to listOf("SLUS-01206", "SLUS-01346"),
        "tekken3" to listOf("SLUS-00402"),
        "xenogears" to listOf("SLUS-00664", "SLUS-00669"),
        "finalfantasyvii" to listOf("SCUS-94163", "SCUS-94164", "SCUS-94165"),
        "finalfantasyviii" to listOf("SLUS-00892", "SLUS-00908", "SLUS-00909", "SLUS-00910"),
        "finalfantasyix" to listOf("SLUS-01251", "SLUS-01295", "SLUS-01296", "SLUS-01297"),
        "finalfantasytactics" to listOf("SCUS-94221"),
        "metalgearsolid" to listOf("SLUS-00594", "SLUS-00776"),
        "chronocross" to listOf("SLUS-01041", "SLUS-01080"),
        "thelegendofdragoon" to listOf("SCUS-94491", "SCUS-94584", "SCUS-94585", "SCUS-94586"),
        "legendofdragoon" to listOf("SCUS-94491", "SCUS-94584", "SCUS-94585", "SCUS-94586"),
        "residentevil2" to listOf("SLUS-00421", "SLUS-00592"),
        "residentevil3nemesis" to listOf("SLUS-00923"),
        "parasiteeve" to listOf("SLUS-00662", "SLUS-00668"),
        "silenthill" to listOf("SLUS-00707"),
        "castlevaniasymphonyofthenight" to listOf("SLUS-00067"),
        "suikodenii" to listOf("SLUS-00958"),
        "vagrantstory" to listOf("SLUS-01040"),
    )

    private val REGION_TAG = Regex("""[(\[]\s*(usa|us|ntsc-?u|europe|eur|pal|japan|jpn|jp|korea|asia)[^)\]]*[)\]]""", RegexOption.IGNORE_CASE)
    private val DISC_TAG = Regex("""(?i)\b(?:disc|disk|cd)\s*[.\-]?\s*(\d{1,2})\b""")
    private val TAGS = Regex("""[\[(][^\])]*[\])]""")
    private val NON_ALNUM = Regex("""[^a-z0-9]""")

    /**
     * A serial to fetch box art with, or null.
     *
     * [name] is the ROM's filename (with or without extension) — preferred, because the disc
     * number lives there. [title] is the library's cleaned-up display title, used when the
     * filename is unavailable.
     */
    fun coverSerialFor(title: String?, name: String?): String? {
        catalogue?.find(title, name)?.let { return it }
        val source = name?.takeIf { it.isNotBlank() } ?: title?.takeIf { it.isNotBlank() } ?: return null
        val stem = source.substringAfterLast('/').substringAfterLast(':').substringBeforeLast('.')
        // Explicit regional mappings for titles outside the USA fallback table.
        val key = normalise(stem)
        val region = REGION_TAG.find(stem)?.groupValues?.get(1)?.lowercase(Locale.US)
        if (key == "hamsterclubi" && (region == null || region in listOf("japan", "jpn", "jp")))
            return "SLPS-03266"
        if (key == "tekken3" && region in listOf("europe", "eur", "pal")) return "SCES-01237"
        if (!regionAllows(stem)) return null
        val discs = BY_TITLE[normalise(stem)]
            ?: title?.let { BY_TITLE[normalise(it)] }
            ?: return null
        val index = DISC_TAG.find(stem)?.groupValues?.get(1)?.toIntOrNull() ?: 1
        return discs.getOrNull(index - 1)
    }

    /** USA, or no region stated at all. Anything explicitly non-USA is left alone. */
    private fun regionAllows(name: String): Boolean {
        val tag = REGION_TAG.find(name)?.groupValues?.get(1)?.lowercase(Locale.US) ?: return true
        return tag == "usa" || tag == "us" || tag.startsWith("ntsc")
    }

    private fun normalise(raw: String): String =
        DISC_TAG.replace(TAGS.replace(raw, " "), " ").lowercase(Locale.US).replace(NON_ALNUM, "")
}
