package com.armsx2

import com.armsx2.runtime.MainActivityRuntime

import android.content.Context
import android.net.Uri
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import java.io.File

/**
 * Box-art style for the library: 2D flat scans (the "default" mirror, JPG) or
 * 3D rendered cases (the "3d" mirror, PNG). Both come from the same xlenore
 * repos. Persisted in MainActivityRuntime.prefs and read by [GameInfo.coverUrl], so flipping
 * it recomposes the grid and re-downloads covers in the chosen style.
 */
object CoverArtStyle {
    private const val KEY = "library.coverArt3d"
    val use3d = mutableStateOf(false)
    fun load() { use3d.value = MainActivityRuntime.prefs.getBoolean(KEY, false) }
    fun set(value: Boolean) {
        use3d.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY, value).apply()
    }
}

/**
 * Show the romanised title for games whose real title isn't English (issue #338).
 *
 * Default OFF = show each game under its own name, which for a Japanese release is the
 * Japanese one. That's the GameDB's curated title and it matches desktop, whose
 * `GetTitle(force_en = false)` does the same. On, the library reads the `name-en` the
 * database carries for exactly these games.
 *
 * Only affects titles the DATABASE provides — a game that isn't in the DB keeps its
 * filename-derived name either way, because there's nothing to translate it to.
 */
object EnglishTitles {
    private const val KEY = "library.englishTitles"
    val enabled = mutableStateOf(false)
    fun load() { enabled.value = MainActivityRuntime.prefs.getBoolean(KEY, false) }
    fun set(value: Boolean) {
        enabled.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY, value).apply()
    }
}

/**
 * Per-game display-name overrides. Modded discs routinely report a garbage internal title —
 * "UN6 A35" for a Naruto Ultimate Ninja 5 mod, "5" for a Tekken 6 one — and the GameDB can't
 * correct them, because the serial still belongs to the base game it was built from. The user
 * pins a readable name here and the library shows it wherever the parsed title would appear.
 *
 * Keyed by [GameInfo.settingsKey], the same identity per-game settings use, so a renamed game
 * keeps its name across a re-scan. [enabled] gates the whole feature, so the real titles can be
 * brought back for a look without discarding any overrides.
 */
object CustomNames {
    private const val KEY_ENABLED = "library.customNames"
    private const val KEY_PREFIX = "library.customName."

    val enabled = mutableStateOf(true)

    /** Bumped on every edit. Read inside [nameFor] so a rename recomposes the library:
     *  the names themselves live in prefs, which Compose cannot observe on its own. */
    val version = mutableIntStateOf(0)

    fun load() { enabled.value = MainActivityRuntime.prefs.getBoolean(KEY_ENABLED, true) }

    fun set(value: Boolean) {
        enabled.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY_ENABLED, value).apply()
        version.intValue++
    }

    /** The override to DISPLAY for [key] — null when unset, blank, or the feature is off. */
    fun nameFor(key: String?): String? {
        version.intValue // subscribe: see [version]
        if (!enabled.value || key.isNullOrBlank()) return null
        return stored(key)
    }

    /** The stored override regardless of [enabled] — the editor must show what's saved even
     *  while the feature is toggled off, or turning it off would look like data loss. */
    fun stored(key: String?): String? {
        if (key.isNullOrBlank()) return null
        return MainActivityRuntime.prefs.getString(KEY_PREFIX + key, null)?.takeIf { it.isNotBlank() }
    }

    /** Blank or null clears the override and restores the parsed title. */
    fun setName(key: String?, name: String?) {
        if (key.isNullOrBlank()) return
        val trimmed = name?.trim()
        MainActivityRuntime.prefs.edit().apply {
            if (trimmed.isNullOrEmpty()) remove(KEY_PREFIX + key) else putString(KEY_PREFIX + key, trimmed)
        }.apply()
        version.intValue++
    }
}

/** Show the game title under every cover in the main library grid (the old-UI behaviour),
 *  not only where a name is otherwise shown. Toggled from the library 3-dot overflow menu. */
object GridLabels {
    private const val KEY = "library.showGridNames"
    val show = mutableStateOf(false)
    fun load() { show.value = MainActivityRuntime.prefs.getBoolean(KEY, false) }
    fun set(value: Boolean) {
        show.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY, value).apply()
    }
}

/**
 * Games the user has marked hidden from the library (long-press → Hide). Persisted by game URI so
 * it survives rescans. Also the intended way to get rid of stray non-game files that show up in the
 * list (e.g. BIOS dumps kept in a scanned subfolder). "Show hidden" reveals them so they can be
 * unhidden again.
 */
object HiddenGames {
    private const val KEY = "library.hiddenGames"
    private const val SHOW_KEY = "library.showHidden"
    val hidden = mutableStateOf<Set<String>>(emptySet())
    val showHidden = mutableStateOf(false)
    private fun keyOf(game: GameInfo) = game.uri.toString()
    fun load() {
        hidden.value = MainActivityRuntime.prefs.getStringSet(KEY, emptySet())?.toSet() ?: emptySet()
        showHidden.value = MainActivityRuntime.prefs.getBoolean(SHOW_KEY, false)
    }
    fun isHidden(game: GameInfo) = hidden.value.contains(keyOf(game))
    fun setHidden(game: GameInfo, value: Boolean) {
        val next = hidden.value.toMutableSet().apply { if (value) add(keyOf(game)) else remove(keyOf(game)) }
        hidden.value = next
        MainActivityRuntime.prefs.edit().putStringSet(KEY, next).apply()
    }
    fun setShowHidden(value: Boolean) {
        showHidden.value = value
        MainActivityRuntime.prefs.edit().putBoolean(SHOW_KEY, value).apply()
    }
}

/**
 * Library toggle: show the game title under each cover on the shelves. Off by
 * default — the cover already carries the title and a label under every card
 * crowds the shelf UI — but exposed as a quick toggle on the library's left
 * rail for users who keep multiple versions of a game or browse by name.
 */
object LibraryTitles {
    private const val KEY = "library.showTitles"
    val show = mutableStateOf(false)
    fun load() { show.value = MainActivityRuntime.prefs.getBoolean(KEY, false) }
    fun set(value: Boolean) {
        show.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY, value).apply()
    }
}

/** Whether the "Recently Played" shelf shows above the library (#263). Off =
 *  a single unified library with no recent shelf. Default on. */
object LibraryRecentShelf {
    private const val KEY = "library.showRecentlyPlayed"
    val show = mutableStateOf(true)
    fun load() { show.value = MainActivityRuntime.prefs.getBoolean(KEY, true) }
    fun set(value: Boolean) {
        show.value = value
        MainActivityRuntime.prefs.edit().putBoolean(KEY, value).apply()
    }
}

/**
 * Library view options: switch between the cover SHELF view and a compact LIST
 * view (game names only) for fast finding on small screens, plus a manual grid
 * size (columns + rows) that drives cover size in shelf view. 0 = Auto.
 * Persisted in MainActivityRuntime.prefs and observed by the library so changes recompose the grid.
 */
object LibraryView {
    private const val KEY_LIST = "library.listMode"
    private const val KEY_COLS = "library.gridColumns"
    private const val KEY_ROWS = "library.gridRows"
    const val MAX_COLS = 6
    const val MAX_ROWS = 5
    /** true = compact name list; false = cover shelves. */
    val listMode = mutableStateOf(false)
    /** Covers per row in shelf view; 0 = auto-fit to screen width. */
    val columns = mutableStateOf(0)
    /** Target visible rows in shelf view (caps cover height); 0 = auto. */
    val rows = mutableStateOf(0)
    fun load() {
        listMode.value = MainActivityRuntime.prefs.getBoolean(KEY_LIST, false)
        columns.value = MainActivityRuntime.prefs.getInt(KEY_COLS, 0).coerceIn(0, MAX_COLS)
        rows.value = MainActivityRuntime.prefs.getInt(KEY_ROWS, 0).coerceIn(0, MAX_ROWS)
    }
    fun setListMode(v: Boolean) {
        listMode.value = v
        MainActivityRuntime.prefs.edit().putBoolean(KEY_LIST, v).apply()
    }
    fun toggleListMode() = setListMode(!listMode.value)
    /** Cycle columns Auto→2→3→…→MAX→Auto (shelf view cover size). */
    fun cycleColumns() {
        val next = when {
            columns.value <= 0 -> 2
            columns.value >= MAX_COLS -> 0
            else -> columns.value + 1
        }
        columns.value = next
        MainActivityRuntime.prefs.edit().putInt(KEY_COLS, next).apply()
    }
    /** Cycle rows Auto→2→3→…→MAX→Auto (caps cover height). */
    fun cycleRows() {
        val next = when {
            rows.value <= 0 -> 2
            rows.value >= MAX_ROWS -> 0
            else -> rows.value + 1
        }
        rows.value = next
        MainActivityRuntime.prefs.edit().putInt(KEY_ROWS, next).apply()
    }
}

/**
 * One row in the games-list screen. Title and serial come from the filename
 * ([FilenameParser]) and the disc's own boot line
 * ([com.armsx2.core.Ps1DiscId]); compatibility is always 0 (no stars filled)
 * because the PS1 core ships no compatibility database.
 *
 * **PS1 only.** This app emulates the PlayStation and nothing else, so there
 * is exactly one platform and it cannot be anything but [PS1]. The enum is
 * kept (rather than deleted) purely because the UI prints `platform.key` /
 * `platform.name` on the game card and the Info tab. Cover art therefore
 * always resolves against xlenore/psx-covers — there is no ps2-covers path
 * anywhere in this build.
 */
enum class GamePlatform(val key: String) {
    PS1("ps1");

    companion object {
        /** Always [PS1]. Takes a key only so a cache written by an older build — which could
         *  still say "ps2" — folds into the one platform this app has instead of failing. */
        @Suppress("UNUSED_PARAMETER")
        fun fromKey(s: String?): GamePlatform = PS1
    }
}

data class GameInfo(
    val uri: Uri,
    val title: String,
    val serial: String?,
    val compatibility: Int = 0,    // 0..5 — always 0: the PS1 core has no compatibility database
    val extension: String = "",    // upper-case container ext, e.g. "CUE", "BIN", "CHD"
    val platform: GamePlatform = GamePlatform.PS1,
    /** GameDB `name-sort` — the title's sort key. For a Japanese game this is the kana
     *  reading, which is the only way the list sorts the way a Japanese reader expects:
     *  sorting the kanji sorts by codepoint, which is meaningless. Empty when the DB has
     *  no separate key (most non-JP games), or when the title came from the filename. */
    val titleSort: String = "",
    /** GameDB `name-en` — the romanised title, present only where the original isn't
     *  English. Its presence is exactly how we know [title] is non-English. */
    val titleEn: String = "",
) {
    /** The title to show. Mirrors GameList.h's `GetTitle(force_en)`: the original unless
     *  English is asked for AND a separate English title exists. */
    /** A user override wins over both the parsed and the English title — it exists precisely
     *  because those are wrong for this disc (see [CustomNames]). */
    fun displayTitle(forceEn: Boolean): String =
        CustomNames.nameFor(settingsKey)
            ?: if (forceEn && titleEn.isNotEmpty()) titleEn else title

    /** The key to sort by. Mirrors GameList.h's `GetTitleSort(force_en)`, including the
     *  subtlety it documents: when a separate English title exists, [titleSort] is in the
     *  WRONG language for an English list, so the English title has to sort itself. */
    fun sortKey(forceEn: Boolean): String = when {
        // A renamed game sorts under the name the user actually sees; otherwise it files
        // itself under the garbage title they renamed it to get away from ("UN6 A35" landing
        // under U instead of N for Naruto).
        !CustomNames.nameFor(settingsKey).isNullOrBlank() -> CustomNames.nameFor(settingsKey)!!
        forceEn && titleEn.isNotEmpty() -> titleEn
        titleSort.isNotEmpty() -> titleSort
        else -> title
    }
    /**
     * The cover to load, as something Coil accepts. Priority mirrors
     * [com.armsx2.core.Ps1Covers]: a cover image the user dropped next to the ROM
     * (`<name>.jpg/png/webp`) wins, then the psx-covers mirror keyed by disc serial, then null →
     * the caller draws its gradient placeholder. PS1 only: [GamePlatform] has no other member, so
     * `psx-covers` is the only repo this can ever produce.
     *
     * The sibling probe is memoised inside Ps1Covers and pre-warmed by the library scan, so
     * reading this during composition costs a map lookup.
     */
    val coverUrl: String? get() {
        localCoverPath()?.let { return Uri.fromFile(File(it)).toString() }
        val s = coverSerial ?: return null
        // A cover pre-fetched into <DataRoot>/covers beats the network: it renders instantly,
        // works offline, and survives a cache clear. Only 2D art is stored this way, so the 3D
        // style still goes to the network for its own URL.
        if (!CoverArtStyle.use3d.value) {
            com.armsx2.core.Ps1Covers.downloadedCover(s)?.let { return Uri.fromFile(it).toString() }
        }
        // 3D cases live under covers/3d/*.png; flat 2D scans under
        // covers/default/*.jpg. Coil decodes by content, so the extension
        // mismatch on the cached file is fine.
        return if (CoverArtStyle.use3d.value)
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/3d/$s.png"
        else
            "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/$s.jpg"
    }

    /**
     * The serial to fetch BOX ART with — [serial] where the disc identified itself, otherwise a
     * dump-name lookup ([com.armsx2.core.Ps1TitleSerials]).
     *
     * Separate from [serial] on purpose. [serial] is the game's identity: RetroAchievements hashes
     * against it, per-game settings and play time key off it, and a value guessed from a filename
     * has no business deciding any of those. Art is the one thing a guess can safely drive — the
     * worst case is the wrong picture — so a disc the extractor cannot read (a .chd, a damaged
     * image) degrades to "cover still works" rather than a blank tile.
     */
    val coverSerial: String? get() = serial?.takeIf { it.isNotBlank() }
        ?: com.armsx2.core.Ps1TitleSerials.coverSerialFor(title, uri.lastPathSegment)

    /** Absolute path of a cover sitting next to this ROM, or null. */
    private fun localCoverPath(): String? {
        if (uri.scheme != null && uri.scheme != "file") return null
        val path = uri.path?.takeIf { it.isNotBlank() } ?: return null
        return runCatching { com.armsx2.core.Ps1Covers.siblingCoverPath(path) }.getOrNull()
    }

    /** Human-readable region (USA / Europe / Japan / Korea / …), from the disc serial's
     *  publisher prefix. Shown under the cover so users can tell apart multiple regional
     *  versions of the same game. [gameDbRegion] is the (currently inert) hook for a curated
     *  override; without a PS1 database the prefix heuristic is the whole answer. */
    val region: String? get() = serial?.let { gameDbRegion(it) ?: regionForSerial(it) }

    /** Region as a flag emoji (🇺🇸 / 🇪🇺 / 🇯🇵 / …) for the cover label, or null.
     *  Rendered ahead of the title so the region is always visible even when a
     *  long name wraps/ellipsizes. */
    val regionFlag: String? get() = region?.let { regionFlagFor(it) }

    /** A short version/edition tag shown under the title so two copies of the
     *  same game can be told apart: a disc-version token parsed from the dump
     *  filename (e.g. "v3.00") when present, otherwise the serial. */
    val versionTag: String? get() {
        val name = uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
        return name?.let { FilenameParser.versionTokenOf(it) } ?: serial
    }

    /** Stable per-game identity used to key per-game SETTINGS (config.game.<key>).
     *  Disc games use their serial (byte-identical to before). Serial-less
     *  ELF/homebrew fall back to a normalized filename stem so their per-game
     *  settings persist across a reboot — a serial-keyed store silently dropped
     *  them to global at boot, which is issue #253. Derived purely from the ROM
     *  path so the boot path and the in-game overlay resolve the SAME key (the
     *  bug was the overlay saving under one key while boot read another). Stem
     *  keys can collide if two ELFs share a filename; acceptable for homebrew. */
    val settingsKey: String? get() = serial?.takeIf { it.isNotBlank() }
        ?: uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
            ?.substringBeforeLast('.')?.trim()?.takeIf { it.isNotEmpty() }
}

/**
 * User-supplied cover overrides for games the online repo can't match — homebrew,
 * ELF ports, obscure dumps with no serial. Stored as image files under
 * `<dataRoot>/covers/custom/`. A cover is matched case-insensitively (png / jpg /
 * jpeg / webp) by the game's serial, its ROM filename, OR its displayed title — so
 * dropping in a file named after the game just works, and the in-app picker writes
 * to the same folder. A custom cover always wins over the online repo cover.
 */
object CustomCovers {
    /** Bumped on set/remove so cover tiles re-resolve. */
    val version = mutableStateOf(0)

    // MainActivityRuntime.assetCopyRoot() can flip between the chosen system dir and the
    // app-private fallback depending on a transient write-probe — so covers got
    // stored under one root and looked up under another ("sometimes there,
    // sometimes not"). Cache the covers root on first resolve so set + load always
    // agree, and share this exact cache with the library cover loader.
    @Volatile
    private var cachedCoversRoot: File? = null
    fun coversRoot(context: Context): File =
        cachedCoversRoot ?: File(MainActivityRuntime.assetCopyRoot(context), "covers").also { cachedCoversRoot = it }

    private fun dir(context: Context): File = File(coversRoot(context), "custom")

    private fun filenameStem(game: GameInfo): String? =
        game.uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')
            ?.substringBeforeLast('.')?.trim()?.takeIf { it.isNotEmpty() }

    /** Names the user might give the cover file, highest priority first. */
    private fun keys(game: GameInfo): List<String> = buildList {
        game.serial?.takeIf { it.isNotBlank() }?.let { add(it) }
        filenameStem(game)?.let { add(it) }
        game.title.takeIf { it.isNotBlank() }?.let { add(it) }
    }

    /** Load all custom covers as lowercased-stem -> File, in ONE directory
     *  listing. The library preloads this once (and refreshes on [version]) so
     *  cover tiles can resolve synchronously instead of each doing its own I/O
     *  during a scroll — which raced and mis-assigned covers across games. */
    fun loadAll(context: Context): Map<String, File> {
        val files = dir(context).listFiles()?.filter { it.isFile && it.length() > 0L } ?: return emptyMap()
        if (files.isEmpty()) return emptyMap()
        val byStem = HashMap<String, File>(files.size)
        for (f in files) byStem.putIfAbsent(f.nameWithoutExtension.lowercase(), f)
        return byStem
    }

    /** Resolve [game]'s cover against a preloaded [loadAll] map. No I/O.
     *  Look-up keys are run through the same [sanitize] the writer uses, so a
     *  title containing : / \ etc. (which [targetFor] wrote as '_') still
     *  resolves; sanitize is a no-op for clean keys. */
    fun matchIn(map: Map<String, File>, game: GameInfo): File? {
        if (map.isEmpty()) return null
        for (k in keys(game)) map[sanitize(k).lowercase()]?.let { return it }
        return null
    }

    /** An existing custom cover for [game], or null. Single-game convenience
     *  (does its own listing) — used off the scroll path. */
    fun fileFor(context: Context, game: GameInfo): File? = matchIn(loadAll(context), game)

    /** Path the in-app picker writes to (serial if present, else ROM filename). */
    private fun targetFor(context: Context, game: GameInfo): File {
        val key = game.serial?.takeIf { it.isNotBlank() }
            ?: filenameStem(game) ?: game.title.ifBlank { "cover" }
        return File(dir(context), sanitize(key) + ".png")
    }

    /** Copy [source] in as [game]'s cover, replacing any prior one. */
    fun set(context: Context, game: GameInfo, source: Uri): Boolean = runCatching {
        remove(context, game)
        val target = targetFor(context, game)
        target.parentFile?.mkdirs()
        context.contentResolver.openInputStream(source)?.use { ins ->
            target.outputStream().use { outs -> ins.copyTo(outs) }
        }
        (target.isFile && target.length() > 0L).also { if (it) version.value++ }
    }.getOrDefault(false)

    fun remove(context: Context, game: GameInfo): Boolean {
        val f = fileFor(context, game) ?: return false
        return f.delete().also { if (it) version.value++ }
    }

    private fun sanitize(s: String): String =
        s.replace(Regex("""[/\\:*?"<>|\n\r\t]"""), "_").trim().ifEmpty { "cover" }
}

/**
 * Always null in the PS1 build.
 *
 * This used to ask PCSX2's GameDB over JNI, which resolved the cases a serial PREFIX cannot
 * (SCES covering both Europe and India, etc.). The PS1 core carries no such database and the
 * PCSX2 JNI is a stub returning null, so the prefix heuristic in [regionForSerial] is the whole
 * answer now. Kept as a named seam so [GameInfo.region] still reads as "curated, else heuristic"
 * if a PS1 database ever lands.
 */
@Suppress("UNUSED_PARAMETER")
fun gameDbRegion(serial: String): String? = null

/** Map a PS1 disc-serial prefix to a region label. */
fun regionForSerial(serial: String): String? = when (serial.take(4).uppercase()) {
    "SLUS", "SCUS", "LSP0" -> "USA"
    "SLES", "SCES", "SLED", "SCED" -> "Europe"
    // SIPS / ESPM / SCPM / PAPX / CPCS are PS1-era Japanese publisher prefixes.
    "SLPS", "SLPM", "SCPS", "SCPM", "SIPS", "ESPM", "SCAJ", "ALCH", "PAPX",
    "ROSE", "TCPS", "KOEI", "PCPX", "CPCS", "SLPN" -> "Japan"
    "SLKA", "SCKA" -> "Korea"
    "SLAJ" -> "Asia"
    else -> null
}

/** Map a region label to a flag emoji. Asia falls back to a globe. */
fun regionFlagFor(region: String): String? = when (region) {
    "USA" -> "🇺🇸"
    "Europe" -> "🇪🇺"
    "Japan" -> "🇯🇵"
    "Korea" -> "🇰🇷"
    "India" -> "🇮🇳"
    "China" -> "🇨🇳"
    "Hong Kong" -> "🇭🇰"
    "Asia" -> "🌏"
    else -> null
}

/**
 * Best-effort serial extractor — the FALLBACK for containers
 * [com.armsx2.core.Ps1DiscId] cannot read into (.chd/.zip/.exe). Recognized
 * dump conventions:
 *   "Game (USA) [SLUS-00594].bin"      → SLUS-00594
 *   "Game (USA) [SLUS_005.94].bin"     → SLUS-00594
 *   "SCUS_949.00 - Game.cue"           → SCUS-94900
 *   "slus_005.94.iso"                  → SLUS-00594
 *
 * The pattern matches 4 letters + optional separator + 3 digits + optional
 * dot + 2 digits, normalized to "AAAA-NNNNN" upper-case.
 */
object FilenameParser {
    private val serialRegex = Regex("""([A-Za-z]{4})[\s_-]?(\d{3})\.?(\d{2})""")
    private val tagsRegex = Regex("""[\[(].*?[\])]""")
    // Disc-version token (e.g. "v3.00", "v 1.0"). The 'v' prefix is required so
    // release years ("(2004)") and unrelated x.y numbers aren't mistaken for it.
    private val versionRegex = Regex("""(?i)\bv\.?\s?(\d{1,2}(?:\.\d{1,2}){1,2})\b""")

    /** A disc-version token like "v3.00" parsed from a filename, or null. Lets
     *  two copies of the same game (same serial, different disc revision) be told
     *  apart when the dump filename carries the version. */
    fun versionTokenOf(filename: String): String? =
        versionRegex.find(filename)?.let { "v" + it.groupValues[1] }
    private val whitespaceRegex = Regex("""\s+""")

    fun parse(filename: String): Pair<String, String?> {
        val withoutExt = filename.substringBeforeLast('.')
        val match = serialRegex.find(withoutExt)
        val serial = match?.let {
            "${it.groupValues[1].uppercase()}-${it.groupValues[2]}${it.groupValues[3]}"
        }
        // (An alias table pinning two PS2 Devil May Cry 2 discs used to live here. Removed with
        // the PS1 rewrite — this app never identifies a PS2 title.)
        // Strip the matched serial token + any [region] / (lang) tags so the
        // displayed title is the game name rather than the full filename.
        var title = withoutExt
        if (match != null) title = title.replace(match.value, "")
        title = title.replace(tagsRegex, "")
            .replace(whitespaceRegex, " ")
            .trim(' ', '-', '_', '.')
        if (title.isEmpty()) title = withoutExt
        return title to serial
    }
}
