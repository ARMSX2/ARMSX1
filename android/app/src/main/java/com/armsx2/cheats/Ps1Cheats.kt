package com.armsx2.cheats

import android.content.Context
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * One entry in a game's cheat catalogue: a `[Name]` group and the code lines under it.
 *
 * [lineCount] is the number of CODE lines, not text lines — a group with zero is a header with
 * nothing under it, which is worth showing rather than hiding because it is almost always a
 * paste that lost its codes. [hasUnsupported] means at least one line uses a GameShark code type
 * the core does not implement; the entry still loads, and its other lines still work.
 */
data class Ps1CheatEntry(
    val name: String,
    val lineCount: Int,
    val hasUnsupported: Boolean = false,
    val description: String = "",
)

/**
 * PS1 cheats: where a game's `.cht` file lives, how it is read, and how a selection reaches the
 * emulator.
 *
 * ## Format
 *
 * GameShark / Action Replay codes (`800AB3C4 0063`) grouped by `[Name]`, in a `.cht` file. This
 * is NOT the PS2 project's PNACH: PNACH is `patch=1,EE,<addr>,<width>,<value>` keyed by
 * `<SERIAL>_<CRC>`, and the PS1 has no EE/IOP split, no 32 MB address space and no ELF CRC that
 * anyone publishes patches against. Every PS1 cheat that has ever been published is a pair of
 * hex words, so that is what this reads — a user can paste one off a twenty-year-old FAQ and it
 * works. The authoritative parser is the CORE's (`psx/cheats.c`); the reader here exists so the
 * list can be shown for a game that is not running, and [coreCatalogue] lets the two be compared
 * rather than assumed equal.
 *
 * ## Identity
 *
 * Keyed by `GameInfo.settingsKey` — the disc serial from [com.armsx2.core.Ps1DiscId], with the
 * filename-stem fallback for serial-less homebrew. That is the SAME key cover art,
 * RetroAchievements, per-game controller skins and per-game settings already use. There is no
 * fourth idea of "which game is this" here.
 *
 * ## Storage
 *
 * The FILE is content and lives on disk at `<dataRoot>/cheats/<key>.cht`, so a user can also just
 * drop one in. WHICH entries are armed is a SETTING and lives in the per-game layer that already
 * exists ([Ps1SettingsStore], `[cheats] enabled_codes`) — no parallel store, one merge point.
 *
 * Every write goes to the GAME's tier, never global. That is deliberate: enablement stored by
 * name in a layer that can win globally is how the sibling PS2 project armed "Infinite Health"
 * in every game a user owned, and it needed a one-time repair function to undo.
 */
object Ps1Cheats {

    /** `<dataRoot>/cheats` — the same folder the old PS2 patch manager used, so an existing
     *  install keeps whatever the user already put there. */
    private fun dir(context: Context): File =
        File(MainActivityRuntime.assetCopyRoot(context.applicationContext), "cheats")

    /**
     * The catalogue file for [gameKey].
     *
     * The key is a disc serial (`SLUS-00594`) or a filename stem, both already filename-safe;
     * anything else is sanitised rather than trusted, because this string ends up as a path.
     */
    fun fileFor(context: Context, gameKey: String): File =
        File(dir(context), sanitise(gameKey) + ".cht")

    private fun sanitise(key: String): String =
        key.map { if (it.isLetterOrDigit() || it == '-' || it == '_' || it == '.') it else '_' }
            .joinToString("")
            .trim('.', ' ')
            .ifEmpty { "unknown" }

    /** The catalogue's raw text, or "" when the game has no file yet. Never null. */
    private fun readText(context: Context, gameKey: String): String =
        runCatching { fileFor(context, gameKey).takeIf { it.isFile }?.readText() }
            .getOrNull() ?: ""

    /** Replace the catalogue's text, creating the folder if needed. Returns success. */
    private fun writeText(context: Context, gameKey: String, text: String): Boolean = runCatching {
        val file = fileFor(context, gameKey)
        file.parentFile?.mkdirs()
        file.writeText(if (text.endsWith("\n") || text.isEmpty()) text else text + "\n")
        syncFilePath(context, gameKey)
        true
    }.getOrDefault(false)

    /**
     * Point `[cheats] file` at the catalogue if it now exists, and clear it if it does not.
     *
     * Called on every write because the two are recorded separately: the setting only stores
     * keys that CHANGED, so a user who switches cheats on BEFORE the file exists records
     * `enabled = true` against an empty path, and pasting codes afterwards would leave that
     * path empty forever — cheats on, list populated, nothing applied. Nothing about the
     * screen would explain why.
     */
    private fun syncFilePath(context: Context, gameKey: String) {
        val app = context.applicationContext
        val file = fileFor(app, gameKey)
        val resolved = if (file.isFile) file.absolutePath else ""
        runCatching {
            Ps1SettingsStore.update(app, gameKey) { it.copy(cheatsFile = resolved) }
        }
        applyLive(app, gameKey)
    }

    /** Append [text] to the catalogue — what the "paste codes" path does. */
    fun appendText(context: Context, gameKey: String, text: String): Boolean {
        val existing = readText(context, gameKey)
        val joined = when {
            existing.isEmpty() -> text
            existing.endsWith("\n") -> existing + text
            else -> existing + "\n" + text
        }
        return writeText(context, gameKey, joined)
    }

    // ---- reading ---------------------------------------------------------------------------

    /**
     * Code types `psx/cheats.c` implements. Kept here ONLY to mark an entry in the list; the
     * core makes the same judgement independently and is what actually decides.
     *
     * If the core gains a type, adding it here is a display fix, not a functional one — the
     * failure mode of forgetting is a warning badge on a cheat that works, which is visible and
     * harmless, rather than a cheat that silently does nothing.
     */
    private val SUPPORTED_TYPES = setOf(
        0x30, 0x80,                          // write 8 / write 16
        0x50,                                // slide (repeat the next write)
        0xD0, 0xD1, 0xD2, 0xD3,              // 16-bit compares
        0xE0, 0xE1, 0xE2, 0xE3,              // 8-bit compares
    )

    private val CODE_LINE = Regex("""^([0-9A-Fa-f]{8})\s*[:,]?\s*([0-9A-Fa-f]{1,8})\s*(?:[#;]|//.*)?$""")

    /**
     * Parse a `.cht` body into its entries.
     *
     * Mirrors the core's parser: `#`, `;` and `//` comment, `[Name]` starts a group, `@desc`
     * adds a description, and anything else that is two hex words is a code line (space, colon
     * or run-together). Codes before the first header land in an "Unnamed codes" group, exactly
     * as the core does, so a bare list of codes pasted in is still selectable.
     */
    private fun parse(text: String): List<Ps1CheatEntry> {
        val out = ArrayList<Ps1CheatEntry>()
        var name: String? = null
        var lines = 0
        var unsupported = false
        var description = StringBuilder()

        fun flush() {
            val n = name ?: return
            out.add(Ps1CheatEntry(n, lines, unsupported, description.toString().trim()))
            name = null
            lines = 0
            unsupported = false
            description = StringBuilder()
        }

        for (raw in text.lineSequence()) {
            val line = raw.trim().removePrefix("\uFEFF")
            if (line.isEmpty() || line.startsWith("#") || line.startsWith(";") || line.startsWith("//")) continue

            if (line.startsWith("[")) {
                flush()
                name = line.removePrefix("[").substringBeforeLast(']').trim().ifEmpty { "(unnamed)" }
                continue
            }

            if (line.startsWith("@")) {
                if (name != null && line.drop(1).startsWith("desc")) {
                    val t = line.drop(5).trim()
                    if (t.isNotEmpty()) {
                        if (description.isNotEmpty()) description.append(' ')
                        description.append(t)
                    }
                }
                continue
            }

            val match = CODE_LINE.find(line) ?: continue
            if (name == null) name = "Unnamed codes"
            lines++
            val type = match.groupValues[1].substring(0, 2).toInt(16)
            if (type !in SUPPORTED_TYPES) unsupported = true
        }
        flush()
        return out
    }

    /** The entries in [gameKey]'s catalogue. Empty when it has no file. */
    fun entriesFor(context: Context, gameKey: String): List<Ps1CheatEntry> =
        parse(readText(context, gameKey))

    // ---- selection (the per-game settings layer) --------------------------------------------

    /** The game's resolved `[cheats]` values, through the one merge point. */
    fun settingsFor(context: Context, gameKey: String): Ps1Settings =
        Ps1SettingsStore.load(context.applicationContext, gameKey)

    /**
     * Record [enabled] as [gameKey]'s armed set and push it to the core if that game is running.
     *
     * ALWAYS writes at the game's tier — [Ps1SettingsStore.update] with a non-null key — so the
     * three `[cheats]` keys can never reach the global layer and arm a name in some other game.
     *
     * The file path is written alongside the list rather than being derived by the core, so the
     * core never has to know how the app names things and a hand-edited settings.toml pointing
     * somewhere else keeps working.
     */
    fun setEnabled(context: Context, gameKey: String, master: Boolean, enabled: List<String>) {
        val app = context.applicationContext
        val file = fileFor(app, gameKey)
        Ps1SettingsStore.update(app, gameKey) { current ->
            current.copy(
                cheatsEnabled = master,
                // Only claim a file that exists. Pointing at a missing path would make the core
                // log a failure every launch for a game the user has no cheats for.
                cheatsFile = if (file.isFile) file.absolutePath else "",
                cheatsEnabledCodes = enabled,
            )
        }
        applyLive(app, gameKey)
    }

    /**
     * Push [gameKey]'s selection into the RUNNING machine, if that is the game running.
     *
     * Returns the number of entries the core armed, -1 when RetroAchievements hardcore refused,
     * and 0 when there is nothing to do (no VM, or a different game is running — in which case
     * the edit is on disk and takes effect at that game's next launch, which is what every other
     * PS1 setting on this screen does too).
     *
     * The guard is not defensive noise: the core's catalogue is process-global, so loading
     * another game's file while a session is live would swap the running game's codes out.
     */
    private fun applyLive(context: Context, gameKey: String): Int {
        if (!isRunning(gameKey)) return 0
        val app = context.applicationContext
        val settings = settingsFor(app, gameKey)
        return runCatching {
            NativeApp.cheatsLoad(if (settings.cheatsEnabled) settings.cheatsFile else "")
            NativeApp.cheatsApply(
                settings.cheatsEnabled,
                settings.cheatsEnabledCodes.toTypedArray(),
            )
        }.getOrDefault(0)
    }

    /** True when [gameKey] is the game the core is running right now. */
    fun isRunning(gameKey: String): Boolean =
        Ps1SettingsStore.activeGameKey == gameKey &&
            runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)

    /** How many entries the running machine has armed, per the CORE. -1 when it cannot be asked. */
    fun armedCount(): Int = runCatching { NativeApp.cheatsArmedCount() }.getOrDefault(-1)

    /**
     * The catalogue as the CORE parsed it, for the running game only.
     *
     * Exists so "the app lists 12 cheats" and "the engine found 12 cheats" can be COMPARED. A
     * screen that lists an entry the engine never parsed is a lie, and this is what makes that
     * detectable instead of a mystery bug report.
     */
    fun coreCatalogue(): List<Ps1CheatEntry> {
        val raw = runCatching { NativeApp.cheatsList() }.getOrDefault("")
        if (raw.isEmpty()) return emptyList()
        return raw.split('\u001E').mapNotNull { record ->
            val fields = record.split('\u001F')
            val name = fields.getOrNull(0)?.takeIf { it.isNotEmpty() } ?: return@mapNotNull null
            Ps1CheatEntry(
                name = name,
                lineCount = fields.getOrNull(1)?.toIntOrNull() ?: 0,
                hasUnsupported = fields.getOrNull(2) == "1",
                description = fields.getOrNull(3).orEmpty(),
            )
        }
    }
}
