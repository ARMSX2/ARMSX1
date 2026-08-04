package com.armsx2.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.GameInfo
import com.armsx2.config.Ps1GameSettings
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.i18n.I18n
import com.armsx2.i18n.str
import com.armsx2.navigation.SettingsCategory

/**
 * Per-game settings: the SCOPE the PS1 tabs are being edited in, and the UI that makes the winning
 * layer visible.
 *
 * The rule this file exists to enforce: **a user must always be able to tell which layer is
 * winning.** An invisible per-game override that beats a global edit is the single largest source
 * of "this setting does nothing" reports on the sibling PS2 project, and it is invisible only
 * because nothing on screen ever said the override was there. So:
 *
 *  - in per-game scope every mapped row is badged either **Per-game** (this game pins it; tapping
 *    the badge drops it back to global) or **Global** (it is following the baseline);
 *  - in global scope a row the CONTEXT game overrides is badged **Overridden here**, because
 *    editing it globally will not change what that game does until the override is removed —
 *    tapping the badge removes it;
 *  - host-only rows (device output, which is not in settings.toml at all) are badged **App-wide**
 *    so they are not mistaken for per-game values;
 *  - [Ps1PerGamePanel] lists EVERY override the game has, including keys no row claims, so an
 *    override can never hide from the one screen that is supposed to explain it.
 */

// ---- scope --------------------------------------------------------------------------------

/**
 * Which layer the PS1 settings tabs are editing.
 *
 * [gameKey] non-null = the tabs write per-game overrides. [contextKey] is the game the screen is
 * ABOUT even while editing global (the running game, or the one whose detail screen was opened) —
 * without it, global scope could not warn that this very game overrides the row being edited.
 */
internal class Ps1SettingsScope(
    val gameKey: String?,
    val gameTitle: String?,
    val contextKey: String?,
    val contextTitle: String?,
) {
    val perGame: Boolean get() = gameKey != null
}

/** Null outside the PS1 emulator tabs, so no other screen's rows can pick up a badge. */
internal val LocalPs1SettingsScope = compositionLocalOf<Ps1SettingsScope?> { null }

/**
 * Provide the scope for one PS1 tab and render the honest header above it.
 *
 * Kept here rather than inside each tab so all settings rows share the same override, reset and
 * listing behavior as soon as they write through the editor.
 */
@Composable
internal fun Ps1Scoped(
    category: SettingsCategory,
    scopeGame: GameInfo?,
    contextGame: GameInfo?,
    content: @Composable () -> Unit,
) {
    val scope = remember(scopeGame?.settingsKey, contextGame?.settingsKey) {
        Ps1SettingsScope(
            gameKey = scopeGame?.settingsKey,
            gameTitle = scopeGame?.title,
            contextKey = contextGame?.settingsKey,
            contextTitle = contextGame?.title,
        )
    }
    CompositionLocalProvider(LocalPs1SettingsScope provides scope) {
        Column(Modifier.fillMaxWidth()) {
            Ps1ScopeNote()
            Ps1PerGamePanel(category)
            content()
        }
    }
}

// ---- row -> settings.toml keys -------------------------------------------------------------

internal enum class Ps1Tab { Video, Emulation, Audio, Bios, Advanced, Cheats }

private class RowKeys(val tab: Ps1Tab, val labelKey: String, val keys: List<String>)

/**
 * Which `settings.toml` keys each row on the PS1 tabs owns, keyed by the row's i18n LABEL KEY.
 *
 * It used to be keyed by the English literal the tabs passed. Once those rows became `str(...)`
 * lookups the label a row renders is the TRANSLATED one, so an English-keyed table matched nothing
 * outside English and every badge silently vanished. The i18n key is the stable identity now, and
 * [labelIndex] re-resolves it whenever the language changes.
 *
 * This table is for DISPLAY only — the badge, the per-row reset, and the per-tab reset. Recording
 * an override never consults it: [Ps1SettingsStore.update] diffs the serialized form, so a row
 * added by anyone is stored correctly whether or not it appears below. A row missing here loses its
 * badge, which [Ps1PerGamePanel] then reports LOUDLY as an unlabelled override rather than letting
 * it hide.
 */
private val ROWS: List<RowKeys> = listOf(
    // Video
    RowKeys(Ps1Tab.Video, "renderer.gpuBackend.label", listOf("video.gpu_backend")),
    RowKeys(Ps1Tab.Video, "renderer.hwRasterizer.label", listOf("video.renderer")),
    RowKeys(Ps1Tab.Video, "renderer.pgxp.label", listOf("video.pgxp")),
    RowKeys(Ps1Tab.Video, "renderer.internalRes.label", listOf("video.internal_scale")),
    RowKeys(Ps1Tab.Video, "renderer.verticalSync.label", listOf("video.vsync")),
    RowKeys(Ps1Tab.Video, "renderer.bilinear.label", listOf("video.texture_scale_mode")),
    RowKeys(Ps1Tab.Video, "renderer.integerScaling.label", listOf("video.integer_scaling")),
    RowKeys(
        Ps1Tab.Video,
        "renderer.aspect.label",
        listOf("video.stretch_mode", "video.display_aspect"),
    ),
    RowKeys(Ps1Tab.Video, "renderer.customRatio.label", listOf("video.display_aspect_custom")),
    RowKeys(Ps1Tab.Video, "renderer.wideUpscale.label", listOf("video.wide_upscale")),
    RowKeys(Ps1Tab.Video, "renderer.debugPanel.label", listOf("video.debug_panel")),
    RowKeys(Ps1Tab.Video, "renderer.textureReplacements.label", listOf("video.texture_replacements")),
    RowKeys(Ps1Tab.Video, "renderer.textureDump.label", listOf("video.texture_dump")),
    RowKeys(Ps1Tab.Video, "renderer.textureDir.label", listOf("video.texture_dir")),
    // Emulation
    RowKeys(Ps1Tab.Emulation, "perf.cpuMode.label", listOf("cpu.execution_mode")),
    RowKeys(Ps1Tab.Emulation, "perf.region.label", listOf("console.region")),
    RowKeys(Ps1Tab.Emulation, "perf.skipBios.label", listOf("cpu.fast_boot")),
    RowKeys(Ps1Tab.Emulation, "perf.frameLimit.label", listOf("runtime.frame_limit")),
    RowKeys(Ps1Tab.Emulation, "perf.emulationSpeed.label", listOf("runtime.speed_percent")),
    RowKeys(Ps1Tab.Emulation, "perf.fpsCap.label", listOf("runtime.fps_limit")),
    RowKeys(Ps1Tab.Emulation, "perf.frameSkipMode.label", listOf("runtime.frame_skip")),
    RowKeys(
        Ps1Tab.Emulation,
        "perf.fastForwardSpeed.label",
        listOf("runtime.fast_forward_speed"),
    ),
    // Audio
    RowKeys(Ps1Tab.Audio, "audio.master.label", listOf("audio.volume")),
    RowKeys(Ps1Tab.Audio, "audio.muted.label", listOf("audio.muted")),
    RowKeys(Ps1Tab.Audio, "audio.swap.label", listOf("audio.swap_channels")),
    RowKeys(Ps1Tab.Audio, "audio.bufferMs.label", listOf("audio.buffer_ms")),
    RowKeys(Ps1Tab.Audio, "audio.skipReverb.label", listOf("audio.skip_reverb")),
    RowKeys(Ps1Tab.Audio, "audio.backend.label", listOf("audio.driver")),
    RowKeys(Ps1Tab.Audio, "audio.background.label", listOf("audio.background_playback")),
    RowKeys(Ps1Tab.Audio, "audio.muteFastForward.label", listOf("audio.mute_fast_forward")),
    RowKeys(Ps1Tab.Audio, "audio.ffVolume.label", listOf("audio.fast_forward_volume")),
    // BIOS
    RowKeys(Ps1Tab.Bios, "bios.model.label", listOf("bios.preferred_model")),
    RowKeys(Ps1Tab.Bios, "bios.forced.label", listOf("bios.override_file")),
    RowKeys(Ps1Tab.Bios, "bios.searchFolder.label", listOf("bios.search_path")),
    // Advanced
    // Logging and Quiet are ONE switch in the core written as two keys, so either row owns both —
    // resetting one and leaving the other would produce the contradictory pair the core resolves
    // by ignoring the second.
    RowKeys(
        Ps1Tab.Advanced,
        "advanced.logging.label",
        listOf("runtime.logging_enabled", "runtime.quiet"),
    ),
    RowKeys(
        Ps1Tab.Advanced,
        "advanced.quiet.label",
        listOf("runtime.quiet", "runtime.logging_enabled"),
    ),
    RowKeys(Ps1Tab.Advanced, "advanced.logLevel.label", listOf("runtime.log_level")),
    RowKeys(Ps1Tab.Advanced, "advanced.expansionRom.label", listOf("paths.expansion_rom")),
    RowKeys(Ps1Tab.Advanced, "advanced.defaultExe.label", listOf("paths.default_psx_exe")),
    // Cheats. The master switch owns all three [cheats] keys, because they are ONE decision
    // written as three: turning cheats off for a game and leaving its file and code list pinned
    // would put the game in a state no row on screen describes. Resetting the badge therefore
    // drops the whole selection back to the global (empty) baseline, which is the only
    // meaningful "follow global" for a cheat list.
    RowKeys(
        Ps1Tab.Cheats,
        "cheats.master.label",
        listOf("cheats.enabled", "cheats.file", "cheats.enabled_codes"),
    ),
)

/**
 * Rows on the PS1 tabs that are NOT settings.toml at all — they act on the phone's output surface
 * and clock policy and are stored in the lifted host Settings/prefs. Badged "App-wide" so a user in
 * per-game scope is not left assuming they follow the game.
 */
private val HOST_ROW_KEYS = listOf(
    "renderer.outputScale.label",
    "renderer.screenRes.label",
    "renderer.lowLatency.label",
    "renderer.sustainedPerf.label",
    // The device-output block's clock levers. Prefs / the lifted host Settings, pushed straight to
    // native — no settings.toml key, so no per-game tier to badge.
    "renderer.adpf.label",
    "renderer.affinity.label",
)

/** Rendered label -> settings.toml keys, rebuilt on a language switch (the widgets only hand
 *  [PerGameMarker] the text they drew, so the index has to be in the current language). */
private var labelIndexLang: String? = null
private var labelIndex: Map<String, List<String>> = emptyMap()
private var hostLabels: Set<String> = emptySet()

private fun ensureLabelIndex() {
    val lang = I18n.current
    if (labelIndexLang == lang && labelIndex.isNotEmpty()) return
    labelIndex = buildMap { for (r in ROWS) putIfAbsent(I18n.get(r.labelKey), r.keys) }
    hostLabels = HOST_ROW_KEYS.mapTo(HashSet()) { I18n.get(it) }
    labelIndexLang = lang
}

private val LABELKEY_BY_KEY: Map<String, String> = buildMap {
    // First declaration wins so "Quiet mode"'s secondary claim on runtime.quiet doesn't steal the
    // name from "Enable logging" in the panel listing.
    for (r in ROWS) for (k in r.keys) putIfAbsent(k, r.labelKey)
}

/** Keys a whole tab owns — what the top-bar Reset clears in per-game scope. */
internal fun ps1KeysForCategory(category: SettingsCategory): List<String> {
    val tab = when (category) {
        SettingsCategory.Graphics -> Ps1Tab.Video
        SettingsCategory.Performance -> Ps1Tab.Emulation
        SettingsCategory.Audio -> Ps1Tab.Audio
        SettingsCategory.Bios -> Ps1Tab.Bios
        SettingsCategory.Advanced -> Ps1Tab.Advanced
        // The enum value is still called Patches; the tab is Cheats. See settingsSections().
        SettingsCategory.Patches -> Ps1Tab.Cheats
        else -> return emptyList()
    }
    return ROWS.filter { it.tab == tab }.flatMap { it.keys }.distinct()
}

/** True for the tabs whose values live in settings.toml and can therefore differ per game. */
internal fun ps1CategoryIsPerGameCapable(category: SettingsCategory): Boolean = when (category) {
    SettingsCategory.Graphics,
    SettingsCategory.Performance,
    SettingsCategory.Audio,
    SettingsCategory.Bios,
    SettingsCategory.Advanced,
    // Cheats. Per-game is not merely SUPPORTED here, it is the only mode that means anything:
    // a cheat belongs to one disc. The tab resolves the game itself so the scope switch never
    // leaves it showing a list that belongs to nobody.
    SettingsCategory.Patches,
    -> true
    else -> false
}

// ---- the badge ------------------------------------------------------------------------------

/**
 * The override marker for a settings row, drawn inline next to its label.
 *
 * A no-op (draws nothing, costs a map lookup) outside the PS1 tabs, because [LocalPs1SettingsScope]
 * is null there.
 */
@Composable
internal fun PerGameMarker(label: String) {
    val scope = LocalPs1SettingsScope.current ?: return
    val context = LocalContext.current.applicationContext
    // Subscribe to the language, then re-resolve the label index against it — the label arriving
    // here is already translated.
    I18n.current
    ensureLabelIndex()
    val keys = labelIndex[label]

    if (keys == null) {
        if (scope.perGame && label in hostLabels) {
            Spacer(Modifier.width(6.dp))
            MarkerChip(str("scope.badge.appWide"), MaterialTheme.colorScheme.onSurfaceVariant, null)
        }
        return
    }

    if (scope.perGame) {
        val overrides = Ps1GameSettings.overridesFor(scope.gameKey)
        val pinned = keys.any { overrides.containsKey(it) }
        Spacer(Modifier.width(6.dp))
        if (pinned) {
            MarkerChip(str("scope.badge.perGame"), MaterialTheme.colorScheme.primary) {
                Ps1SettingsStore.clearOverrides(context, scope.gameKey, keys)
            }
        } else {
            MarkerChip(str("scope.global"), MaterialTheme.colorScheme.onSurfaceVariant, null)
        }
        return
    }

    // Global scope. Say so when THIS game will ignore the edit about to be made.
    val ctx = scope.contextKey ?: return
    val overrides = Ps1GameSettings.overridesFor(ctx)
    if (keys.none { overrides.containsKey(it) }) return
    Spacer(Modifier.width(6.dp))
    MarkerChip(str("scope.badge.overridden"), MaterialTheme.colorScheme.tertiary) {
        Ps1SettingsStore.clearOverrides(context, ctx, keys)
    }
}

@Composable
private fun MarkerChip(text: String, tint: androidx.compose.ui.graphics.Color, onClick: (() -> Unit)?) {
    val shape = RoundedCornerShape(9.dp)
    Text(
        text,
        color = tint,
        fontSize = 11.sp,
        fontWeight = FontWeight.Bold,
        maxLines = 1,
        modifier = Modifier
            .clip(shape)
            .background(tint.copy(alpha = 0.14f))
            .border(1.dp, tint.copy(alpha = 0.45f), shape)
            .then(
                if (onClick == null) Modifier
                else Modifier.clickable {
                    com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.RESET)
                    onClick()
                },
            )
            .padding(horizontal = 7.dp, vertical = 3.dp),
    )
}

// ---- the header: scope note + override list --------------------------------------------------

/**
 * Replaces the old per-tab claim that every change is "saved to settings.toml and picked up the
 * next time a game is launched" — which was true globally and a lie in per-game scope, where the
 * screen showed a game's name while editing everyone's configuration.
 */
@Composable
internal fun Ps1ScopeNote() {
    val scope = LocalPs1SettingsScope.current ?: return
    val text = if (scope.perGame) {
        str("scope.note.perGame").format(scope.gameTitle ?: str("scope.thisGame"))
    } else {
        str("scope.note.global")
    }
    Text(
        text,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontSize = 13.sp,
        lineHeight = 18.sp,
        modifier = Modifier.padding(bottom = 8.dp),
    )
}

/**
 * Every override the game has, with a per-setting reset and a clear-all.
 *
 * Deliberately lists the whole game, not just this tab: an override on another tab is exactly the
 * one a user will not find, and it is the reason the setting they ARE looking at behaves oddly.
 * Keys no row claims are listed by their raw TOML name and flagged, so a setting added without a
 * badge is loud instead of silent.
 */
@Composable
internal fun Ps1PerGamePanel(category: SettingsCategory) {
    val scope = LocalPs1SettingsScope.current ?: return
    val context = LocalContext.current.applicationContext
    var expanded by remember { mutableStateOf(false) }
    var confirmClear by remember { mutableStateOf(false) }

    // Maintainer signal: any settings.toml key with no row in ROWS can never show a badge. Logged
    // once per scope change rather than surfaced, because most of these legitimately have no
    // interactive row (display_scale, the accuracy flags); an override on one of them IS surfaced,
    // in the list below.
    LaunchedEffect(scope.gameKey, scope.contextKey) {
        val unmapped = Ps1SettingsStore.PER_GAME_KEYS - LABELKEY_BY_KEY.keys
        if (unmapped.isNotEmpty()) {
            println("@@PS1_PERGAME_UNMAPPED@@ ${unmapped.sorted().joinToString(",")}")
        }
    }

    val target = if (scope.perGame) scope.gameKey else scope.contextKey
    val overrides = Ps1GameSettings.overridesFor(target)
    if (target == null || overrides.isEmpty()) return

    val thisGame = str("scope.thisGame")
    val title = if (scope.perGame) (scope.gameTitle ?: thisGame) else (scope.contextTitle ?: thisGame)
    val accent =
        if (scope.perGame) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.tertiary

    Column(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 10.dp)
            .clip(RoundedCornerShape(18.dp))
            .background(accent.copy(alpha = 0.10f))
            .border(1.dp, accent.copy(alpha = 0.42f), RoundedCornerShape(18.dp))
            .padding(horizontal = 14.dp, vertical = 12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                str(
                    if (overrides.size == 1) "scope.overrides.countOne"
                    else "scope.overrides.countMany",
                ).format(overrides.size, title),
                color = accent,
                fontSize = 15.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f),
            )
            MarkerChip(if (expanded) str("action.hide") else str("action.show"), accent) {
                expanded = !expanded
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(
            if (scope.perGame) {
                str("scope.overrides.explainPerGame")
            } else {
                str("scope.overrides.explainGlobal").format(title)
            },
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 13.sp,
            lineHeight = 18.sp,
        )

        if (expanded) {
            Spacer(Modifier.height(8.dp))
            overrides.forEach { (key, value) ->
                val rowLabel = LABELKEY_BY_KEY[key]?.let { str(it) }
                val known = key in Ps1SettingsStore.PER_GAME_KEYS
                val globalValue = Ps1SettingsStore.globalValueOf(context, key)
                Row(
                    Modifier.fillMaxWidth().padding(vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            rowLabel ?: key,
                            color = MaterialTheme.colorScheme.onSurface,
                            fontSize = 14.sp,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Text(
                            buildString {
                                append(key).append(" = ").append(value)
                                if (globalValue != null && globalValue != value) {
                                    append("   (")
                                        .append(
                                            str("scope.overrides.globalValue").format(globalValue),
                                        )
                                        .append(')')
                                }
                                if (!known) {
                                    append("   — ").append(str("scope.overrides.unused"))
                                } else if (rowLabel == null) {
                                    append("   — ").append(str("scope.overrides.noBadge"))
                                }
                            },
                            color = if (known && rowLabel != null) {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            } else {
                                MaterialTheme.colorScheme.error
                            },
                            fontSize = 12.sp,
                            lineHeight = 16.sp,
                        )
                    }
                    Spacer(Modifier.width(8.dp))
                    MarkerChip(str("action.reset"), accent) {
                        Ps1SettingsStore.clearOverrides(context, target, listOf(key))
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val tabKeys = ps1KeysForCategory(category).filter { overrides.containsKey(it) }
                if (tabKeys.isNotEmpty()) {
                    MarkerChip(str("scope.overrides.resetTab").format(tabKeys.size), accent) {
                        Ps1SettingsStore.clearOverrides(context, target, tabKeys)
                    }
                }
                MarkerChip(str("scope.overrides.clearAll"), MaterialTheme.colorScheme.error) {
                    confirmClear = true
                }
            }
        }
    }

    if (confirmClear) {
        com.armsx2.ui.common.ConfirmOverlay(
            title = str("scope.overrides.clearAll"),
            message = str("scope.overrides.clearAll.body").format(title),
            confirmLabel = str("action.clear"),
            destructive = true,
            idPrefix = "ps1-clear-overrides",
            onConfirm = {
                Ps1SettingsStore.clearAllOverrides(context, target)
                confirmClear = false
            },
            onDismiss = { confirmClear = false },
        )
    }
}

/**
 * The in-game pause menu's version of the same truth.
 *
 * That menu edits the GLOBAL layer (see its `editPs1`), so when the running game pins one of the
 * keys it exposes, the row will not appear to do anything. This says so, names the pinned rows, and
 * offers the one-tap way out — at the exact moment and place the confusion happens, which is the
 * whole reason "this setting does nothing" gets reported instead of diagnosed.
 *
 * Draws nothing when the running game pins nothing, which is every game until the user says
 * otherwise.
 */
@Composable
fun Ps1PerGameNotice(context: android.content.Context) {
    val app = context.applicationContext
    val gameKey = Ps1SettingsStore.activeGameKey ?: return
    val overrides = Ps1GameSettings.overridesFor(gameKey)
    if (overrides.isEmpty()) return
    val names = overrides.keys.map { LABELKEY_BY_KEY[it]?.let { k -> str(k) } ?: it }.distinct()
    val accent = MaterialTheme.colorScheme.tertiary

    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(accent.copy(alpha = 0.12f))
            .border(1.dp, accent.copy(alpha = 0.42f), RoundedCornerShape(14.dp))
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Text(
            str(
                if (names.size == 1) "scope.notice.pinsOne" else "scope.notice.pinsMany",
            ).format(names.size),
            color = accent,
            fontSize = 14.sp,
            fontWeight = FontWeight.Bold,
        )
        Spacer(Modifier.height(3.dp))
        Text(
            str("scope.notice.body").format(names.joinToString(", ")),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 12.sp,
            lineHeight = 16.sp,
        )
        Spacer(Modifier.height(6.dp))
        MarkerChip(str("scope.notice.useGlobal"), accent) {
            Ps1SettingsStore.clearAllOverrides(app, gameKey)
        }
    }
}
