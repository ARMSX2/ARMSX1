package com.armsx2.ui.settings

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore

/**
 * Shared plumbing for the PS1 settings tabs.
 *
 * Every emulator-facing tab in this package now edits ONE object — [Ps1Settings] — and persists
 * through [Ps1SettingsStore], which rewrites the native `settings.toml` that `frontend/config.c`
 * parses at launch. There is no live-apply path: the PS1 core runs in its own SDL activity and
 * re-reads the file when it starts, so every change is written to disk immediately and takes
 * effect on the next launch.
 *
 * (The old PS2 tabs went through `ConfigStore`/`InGameOverlay.saveSettings` + ~134 JNI setters.
 * None of that exists for ARMSX1; `NativeApp` is a no-op shim.)
 */
internal class Ps1SettingsEditor(
    private val context: Context,
    private val state: MutableState<Ps1Settings>,
    /**
     * Which layer this editor writes: null = the global baseline, a game's
     * [com.armsx2.GameInfo.settingsKey] = that game's sparse overrides. Comes from
     * [LocalPs1SettingsScope], which [Ps1Scoped] provides around each PS1 tab.
     */
    private val gameKey: String? = null,
) {
    /** Current values. Reading this inside a composable subscribes it to changes. */
    val value: Ps1Settings get() = state.value

    /**
     * Apply an edit and persist it at this editor's tier.
     *
     * The transform runs against what is ON DISK (resolved for this tier), not against this tab's
     * snapshot: the global layer is also written by [com.armsx2.core.Ps1Folders] when the library
     * list changes, and a regenerate-from-snapshot would silently roll that back.
     *
     * In per-game scope [Ps1SettingsStore.update] records ONLY the keys this one edit changed, so
     * everything the user did not touch keeps following global — including changes made to global
     * later. Recording the whole object instead is how a game gets frozen at the values it had the
     * day it was first opened.
     */
    fun update(transform: (Ps1Settings) -> Ps1Settings) {
        val next = runCatching { Ps1SettingsStore.update(context, gameKey, transform) }
            .getOrElse { transform(state.value) }
        state.value = next
        // Every tab rewrites the WHOLE file, so any tab's save can move the [runtime] pacing
        // keys. Drop Ps1Pacing's cache and re-push, or the in-game pacing rows would keep
        // showing (and pushing) the values from before this edit.
        com.armsx2.core.Ps1Pacing.invalidate()
        com.armsx2.core.Ps1Pacing.push(context)
        // Same reasoning for the presentation-only display settings: aspect / custom ratio /
        // stretch apply on the next presented frame, so pushing them here is what makes the
        // Video tab take effect on a running game instead of only at the next launch.
        //
        // push(context), NOT push(next): `next` is the tier being EDITED, which is not necessarily
        // the tier the core is running. Editing global while a game that pins its display mode is
        // live would otherwise shove the losing value straight at the renderer — the picture would
        // change and then snap back at the next launch, which is worse than the setting doing
        // nothing. push(context) re-resolves the active scope.
        com.armsx2.core.Ps1Display.push(context)
        // Same reasoning for the emulation-behaviour settings: multitap, rewind and runahead
        // are all live in the core, so pushing here is what makes those rows take effect on a
        // running game instead of only at the next launch. push(context) re-resolves the
        // active scope for the same reason Ps1Display does.
        com.armsx2.core.Ps1Emulation.push(context)
        // Texture dumping / replacement is live too: psx_texrep_configure() rebuilds the pack
        // index on the emulation thread, so a pack dropped in while a game runs is picked up
        // without a restart. push(context) re-resolves the active scope, same reason as above.
        com.armsx2.core.Ps1Textures.push(context)
    }

    /** Re-read from disk — used after something outside the tab touched the file. */
    fun reload() {
        state.value = Ps1SettingsStore.load(context, gameKey)
    }
}

/**
 * Loads settings.toml once per tab entry and hands back an editor that writes every change back.
 *
 * Only one settings tab is composed at a time (the hub swaps [CategoryContent]), so a per-tab
 * snapshot cannot drift: switching tabs disposes this `remember` and the next tab re-reads the
 * file the previous one just wrote.
 */
@Composable
internal fun rememberPs1SettingsEditor(): Ps1SettingsEditor {
    val context = LocalContext.current.applicationContext
    // The scope is provided by [Ps1Scoped] around each PS1 tab; null outside it (and in the
    // in-game panes), which resolves to the global layer exactly as before.
    val gameKey = LocalPs1SettingsScope.current?.gameKey
    // Keyed on the scope AND on the per-game store's version so switching Global <-> This game, or
    // clearing an override from a badge, re-resolves the merge instead of showing the old tier's
    // values with the new tier's badges.
    val overrideVersion = com.armsx2.config.Ps1GameSettings.version.intValue
    val state = remember(gameKey, overrideVersion) {
        mutableStateOf(Ps1SettingsStore.load(context, gameKey))
    }
    return remember(state, gameKey) { Ps1SettingsEditor(context, state, gameKey) }
}

/**
 * Read-only value row, styled like [ToggleRow] so the informational entries (BIOS search path,
 * resolved override file, launch paths) sit flush with the interactive rows around them.
 */
@Composable
internal fun Ps1InfoRow(label: String, value: String, description: String? = null) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f))
            .border(
                1.dp,
                MaterialTheme.colorScheme.outline.copy(alpha = 0.36f),
                RoundedCornerShape(22.dp),
            )
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            // Which layer this value comes from. These rows are read-only but they are still
            // layered — the forced BIOS file especially, which a single game routinely pins.
            PerGameMarker(label)
            Spacer(Modifier.width(12.dp))
            Text(
                value.ifBlank { "—" },
                color = MaterialTheme.colorScheme.primary,
                fontSize = 15.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1.4f),
            )
        }
        if (description != null) {
            Spacer(Modifier.height(3.dp))
            Text(
                description,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 14.sp,
                lineHeight = 19.sp,
            )
        }
    }
}

/**
 * Free-text row, styled to match [Ps1InfoRow]. Used where a value is a PATH and "blank means
 * automatic" has to be expressible — a folder picker cannot say that, it can only say "some
 * folder", so a picker would make the automatic per-game default unreachable once set.
 *
 * The field is INLINE, never a dialog. A Compose `AlertDialog` is its own focused window and
 * swallows gamepad keys, which is how the pad-profile field in PadTab.kt ended up inline too.
 *
 * [onCommit] fires on every keystroke; callers persist through the editor, which is debounced
 * by the store, not by this row.
 */
@Composable
internal fun Ps1TextRow(
    label: String,
    value: String,
    controllerId: String,
    description: String? = null,
    placeholder: String? = null,
    onCommit: (String) -> Unit,
) {
    val live = remember(value) { mutableStateOf(value) }

    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f))
            .border(
                1.dp,
                MaterialTheme.colorScheme.outline.copy(alpha = 0.36f),
                RoundedCornerShape(22.dp),
            )
            .controllerFocusable(controllerId = controllerId)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text(
                label,
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 17.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
            PerGameMarker(label)
        }
        Spacer(Modifier.height(6.dp))
        OutlinedTextField(
            value = live.value,
            onValueChange = {
                live.value = it
                onCommit(it)
            },
            singleLine = true,
            placeholder = {
                if (placeholder != null) {
                    Text(placeholder, color = Color(0xFF888888), fontSize = 14.sp)
                }
            },
            colors = OutlinedTextFieldDefaults.colors(
                focusedTextColor = MaterialTheme.colorScheme.onSurface,
                unfocusedTextColor = MaterialTheme.colorScheme.onSurface,
                focusedBorderColor = MaterialTheme.colorScheme.primary,
                unfocusedBorderColor = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f),
            ),
            modifier = Modifier.fillMaxWidth(),
        )
        if (description != null) {
            Spacer(Modifier.height(3.dp))
            Text(
                description,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 14.sp,
                lineHeight = 19.sp,
            )
        }
    }
}

/**
 * Tappable action row (import a BIOS, add a library folder, restore defaults…).
 * Registered with the controller nav so a gamepad reaches it like any other row.
 */
@Composable
internal fun Ps1ActionRow(
    label: String,
    controllerId: String,
    description: String? = null,
    onClick: () -> Unit,
) {
    val fire = {
        com.armsx2.MenuSfx.play(com.armsx2.MenuSfx.Event.SELECT)
        onClick()
    }
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp)
            .clip(RoundedCornerShape(22.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f))
            .border(
                1.dp,
                MaterialTheme.colorScheme.outline.copy(alpha = 0.46f),
                RoundedCornerShape(22.dp),
            )
            .controllerFocusable(controllerId = controllerId, onConfirm = fire)
            .clickable(onClick = fire)
            .defaultMinSize(minHeight = if (description == null) 58.dp else 74.dp)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Text(
            label,
            color = MaterialTheme.colorScheme.primary,
            fontSize = 17.sp,
            fontWeight = FontWeight.Bold,
        )
        if (description != null) {
            Spacer(Modifier.height(3.dp))
            Text(
                description,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontSize = 14.sp,
                lineHeight = 19.sp,
            )
        }
    }
}
