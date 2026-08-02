package com.armsx2.ui.cheats

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.GameInfo
import com.armsx2.cheats.Ps1CheatEntry
import com.armsx2.cheats.Ps1Cheats
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.EmptyState
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.SectionTitle
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.LocalPs1SettingsScope
import com.armsx2.ui.settings.Ps1ActionRow
import com.armsx2.ui.settings.Ps1InfoRow
import com.armsx2.ui.settings.SettingsDivider
import com.armsx2.ui.settings.ToggleRow
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Cheats for one PS1 game.
 *
 * ## What replaced what
 *
 * This replaces the lifted PS2 Patch Manager, which was PNACH-shaped end to end — files named
 * `<SERIAL>_<CRC>.pnach`, an online browser keyed by PS2 serials, and five NativeApp calls
 * (`setEnabledPatches`, `reloadPatches`, `getGameCRC`, `getGameSerial`, `getGameSerialFromFd`)
 * that were every one of them EMPTY STUBS. It was reachable from the drawer and from the pause
 * menu, so a user could tick a patch, watch the UI say it was on, and have nothing whatsoever
 * happen. Both of those entry points now come here, and every control on this screen reaches a
 * real native or a real file.
 *
 * ## The one rule this screen enforces
 *
 * A cheat is per-GAME. There is no global cheat list and no way to make one — every write goes
 * through [Ps1Cheats.setEnabled], which writes at the game's tier of the per-game settings layer.
 * The PS2 project stored enablement by NAME somewhere that could be global, and the result was
 * that arming "Infinite Health" in one game armed the identically-named group in every other
 * game the user owned.
 */

// ---- the shared body -------------------------------------------------------------------------

/**
 * The whole feature, minus chrome. Used by both the settings-hub tab and the full screen so the
 * two can never drift.
 *
 * [gameKey] null means "no game to talk about" — the screen says so instead of showing a list
 * that belongs to nobody.
 */
@Composable
fun Ps1CheatsPanel(gameKey: String?, gameTitle: String?) {
    val context = LocalContext.current.applicationContext

    if (gameKey.isNullOrBlank()) {
        EmptyState(
            title = str("cheats.noGame.title"),
            message = str("cheats.noGame.body"),
        )
        return
    }

    // Bumped whenever the file or the selection changes, to re-read both. The file is on disk and
    // the selection is in prefs; neither is something Compose can observe on its own.
    var revision by remember(gameKey) { mutableIntStateOf(0) }

    val settings = remember(gameKey, revision) { Ps1Cheats.settingsFor(context, gameKey) }
    val entries = remember(gameKey, revision) { Ps1Cheats.entriesFor(context, gameKey) }
    val file = remember(gameKey, revision) { Ps1Cheats.fileFor(context, gameKey) }
    val running = remember(gameKey, revision) { Ps1Cheats.isRunning(gameKey) }
    val armed = remember(gameKey, revision) { if (running) Ps1Cheats.armedCount() else -1 }
    // What the EMULATOR parsed out of the same file. Only asked for while this game is the one
    // running, because the core holds one catalogue at a time and it belongs to that game.
    // Listing an entry the engine never saw is a lie the user cannot detect on their own, so it
    // is checked rather than assumed — see the mismatch notice below.
    val coreEntries = remember(gameKey, revision) {
        if (running) Ps1Cheats.coreCatalogue() else emptyList()
    }
    val hardcore = remember(revision) { runCatching { NativeApp.isHardcoreMode() }.getOrDefault(false) }

    val enabled = settings.cheatsEnabledCodes
    val master = settings.cheatsEnabled

    fun commit(nextMaster: Boolean, nextEnabled: List<String>) {
        Ps1Cheats.setEnabled(context, gameKey, nextMaster, nextEnabled)
        revision++
    }

    // Text entry hands off to LibraryKeyboard, not a Compose AlertDialog: a modal Dialog owns its
    // own focused window and swallows gamepad keys, so a pad user could open the paste box and
    // then be unable to type in it. The keyboard's CLOSING is the commit signal — it has no
    // "done" callback, same as the rename row on the Info tab.
    var pasting by remember { mutableStateOf(false) }
    // Resolved out here: str() is @Composable, so it cannot be called from inside the effect
    // below or from the row's onClick lambda.
    val pastedGroupName = str("cheats.pastedGroup")
    val pastePlaceholder = str("cheats.action.paste.placeholder")
    LaunchedEffect(LibraryKeyboard.visible.value) {
        if (pasting && !LibraryKeyboard.visible.value) {
            pasting = false
            val text = LibraryKeyboard.text.value.trim()
            if (text.isNotEmpty()) {
                // A bare paste with no header still becomes a selectable entry (the parser calls
                // it "Unnamed codes"), but naming it is what makes a list of five cheats usable,
                // so wrap anything that does not already carry one.
                val body = if (text.contains('[')) text else "[$pastedGroupName]\n$text"
                Ps1Cheats.appendText(context, gameKey, body)
                revision++
            }
        }
    }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val text = runCatching {
                context.contentResolver.openInputStream(uri)?.use { it.readBytes().decodeToString() }
            }.getOrNull()
            if (!text.isNullOrEmpty()) {
                Ps1Cheats.appendText(context, gameKey, text)
                revision++
            }
        }
    }

    // Names the selection holds that the file no longer has. This is the failure a user cannot
    // diagnose on their own — the switch is on and nothing happens — and it is exactly what
    // renaming a group in the .cht produces, so it is stated rather than left to be discovered.
    val known = entries.mapTo(HashSet()) { it.name.trim().lowercase() }
    val orphans = enabled.filter { it.trim().lowercase() !in known }

    Column(Modifier.fillMaxWidth()) {
        GlassPanel(Modifier.fillMaxWidth()) {
            Column {
                SectionTitle(gameTitle ?: gameKey, str("cheats.section.forGame"))
                Spacer(Modifier.height(8.dp))
                Ps1InfoRow(str("cheats.info.file"), file.absolutePath)
                Ps1InfoRow(
                    str("cheats.info.entries"),
                    if (entries.isEmpty()) str("cheats.info.noFile") else entries.size.toString(),
                )
                // The CORE's count, not a count of ticked boxes, so "you ticked five" and
                // "five are running" cannot quietly disagree.
                if (running) {
                    Ps1InfoRow(
                        str("cheats.info.armed"),
                        if (armed >= 0) armed.toString() else "—",
                        str("cheats.info.armed.description"),
                    )
                }
            }
        }

        Spacer(Modifier.height(10.dp))

        if (hardcore) {
            Notice(
                title = str("cheats.hardcore.title"),
                body = str("cheats.hardcore.body"),
                tint = MaterialTheme.colorScheme.error,
            )
        }

        ToggleRow(
            label = str("cheats.master.label"),
            value = master && !hardcore,
            description = str("cheats.master.description"),
            idSuffix = "cheats.master",
        ) { on ->
            if (!hardcore) commit(on, enabled)
        }
        SettingsDivider()

        if (entries.isEmpty()) {
            Text(
                str("cheats.empty.body"),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 13.sp,
                lineHeight = 18.sp,
                modifier = Modifier.padding(vertical = 10.dp),
            )
        } else {
            entries.forEach { entry ->
                CheatRow(
                    entry = entry,
                    checked = enabled.any { it.equals(entry.name, ignoreCase = true) },
                    locked = hardcore || !master,
                ) { on ->
                    val next = enabled.filterNot { it.equals(entry.name, ignoreCase = true) }
                    // Turning a cheat on turns the master switch on with it. A row that ticks
                    // and does nothing because a switch further up is off is the same bug as a
                    // row wired to a stub, from the user's side of the screen.
                    commit(if (on) true else master, if (on) next + entry.name else next)
                }
                SettingsDivider()
            }
        }

        // The app's reader and the core's parser disagreeing means this list is not describing
        // what will actually run. It should be impossible; saying so out loud is what makes it a
        // bug report instead of a mystery.
        if (running && coreEntries.isNotEmpty() && coreEntries.size != entries.size) {
            Notice(
                title = str("cheats.mismatch.title"),
                body = str("cheats.mismatch.body").format(entries.size, coreEntries.size),
                tint = MaterialTheme.colorScheme.error,
            )
        }

        if (orphans.isNotEmpty()) {
            Notice(
                title = str("cheats.orphans.title"),
                body = str("cheats.orphans.body").format(orphans.joinToString(", ")),
                tint = MaterialTheme.colorScheme.tertiary,
                actionLabel = str("cheats.orphans.clear"),
                onAction = { commit(master, enabled.filterNot { it in orphans }) },
            )
        }

        if (!running && master && enabled.isNotEmpty()) {
            Text(
                str("cheats.note.nextLaunch"),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 13.sp,
                lineHeight = 18.sp,
                modifier = Modifier.padding(vertical = 8.dp),
            )
        }

        Ps1ActionRow(
            label = str("cheats.action.paste"),
            controllerId = "ps1.cheats.paste",
            description = str("cheats.action.paste.description"),
        ) {
            pasting = true
            LibraryKeyboard.open(initial = "", onChange = {}, placeholder = pastePlaceholder)
        }
        SettingsDivider()

        Ps1ActionRow(
            label = str("cheats.action.import"),
            controllerId = "ps1.cheats.import",
            description = str("cheats.action.import.description"),
        ) {
            // Anything readable: community lists ship as .cht and as .txt, and plenty of
            // providers serve either with a generic MIME type.
            importer.launch(arrayOf("*/*"))
        }
        SettingsDivider()

        Ps1ActionRow(
            label = str("cheats.action.reload"),
            controllerId = "ps1.cheats.reload",
            description = str("cheats.action.reload.description"),
        ) {
            revision++
        }
    }
}

@Composable
private fun CheatRow(
    entry: Ps1CheatEntry,
    checked: Boolean,
    locked: Boolean,
    onChange: (Boolean) -> Unit,
) {
    val details = buildString {
        append(str("cheats.row.lines").format(entry.lineCount))
        if (entry.description.isNotEmpty()) {
            append(" · ").append(entry.description)
        }
        if (entry.hasUnsupported) {
            append(" · ").append(str("cheats.row.unsupported"))
        }
    }
    ToggleRow(
        label = entry.name,
        value = checked && !locked,
        description = details,
        idSuffix = "ps1.cheat." + entry.name,
    ) { on -> if (!locked || !on) onChange(on) }
}

@Composable
private fun Notice(
    title: String,
    body: String,
    tint: androidx.compose.ui.graphics.Color,
    actionLabel: String? = null,
    onAction: (() -> Unit)? = null,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(tint.copy(alpha = 0.10f))
            .border(1.dp, tint.copy(alpha = 0.42f), RoundedCornerShape(16.dp))
            .padding(horizontal = 14.dp, vertical = 12.dp),
    ) {
        Text(title, color = tint, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(4.dp))
        Text(
            body,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 13.sp,
            lineHeight = 18.sp,
        )
        if (actionLabel != null && onAction != null) {
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    actionLabel,
                    color = tint,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .clip(RoundedCornerShape(10.dp))
                        .background(tint.copy(alpha = 0.16f))
                        .clickable {
                            com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.RESET)
                            onAction()
                        }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                )
            }
        }
    }
}

// ---- entry points ------------------------------------------------------------------------------

/**
 * The Settings-hub tab.
 *
 * Cheats are per-game by nature, so this resolves the game from the hub's scope: the per-game
 * scope's key when the switch is on "This game", otherwise the game the screen is ABOUT (opened
 * from a library long-press), otherwise the one currently running. It never shows a global list,
 * because there is deliberately no such thing.
 */
@Composable
fun Ps1CheatsTab() {
    val scope = LocalPs1SettingsScope.current
    val runningGame = MainActivityRuntime.currentGame.value
    val key = scope?.gameKey ?: scope?.contextKey ?: runningGame?.settingsKey
    val title = scope?.gameTitle ?: scope?.contextTitle ?: runningGame?.title
    Ps1CheatsPanel(key, title)
}

/**
 * The full screen, for the drawer and the in-game pause menu.
 *
 * [game] is the game the caller is talking about; with none, it falls back to whatever is
 * running, which is what the pause menu wants.
 */
@Composable
fun Ps1CheatsScreen(onBack: () -> Unit, game: GameInfo? = null) {
    val target = game ?: MainActivityRuntime.currentGame.value
    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            ArmsTopBar(
                title = str("cheats.title"),
                subtitle = target?.title ?: str("cheats.noGame.title"),
                leading = { RoundAction("←", str("action.back"), onBack) },
            )
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
                Ps1CheatsPanel(target?.settingsKey, target?.title)
                Spacer(Modifier.height(24.dp))
            }
        }
    }
}
