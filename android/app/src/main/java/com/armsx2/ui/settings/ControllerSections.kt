package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SecondaryScrollableTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str

internal enum class ControllerSection(val labelKey: String, val playerIndex: Int? = null) {
    Settings("pad.section.settings"),
    Touch("pad.section.touchscreen"),
    Controller1("pad.section.controller1", 0),
    Controller2("pad.section.controller2", 1),
    Multiplayer("pad.section.multiplayer"),
    Hotkeys("tab.hotkeys");

    val editsPlayer: Boolean get() = playerIndex != null
    fun stepped(delta: Int): ControllerSection = entries[Math.floorMod(ordinal + delta, entries.size)]

    companion object {
        fun forSettingKey(key: String): ControllerSection? {
            entries.firstOrNull { it.labelKey == key }?.let { return it }
            return when {
                key.startsWith("pad.multiplayer.") || key == "pad.section.controller" -> Multiplayer
                key.startsWith("hotkeys.") -> Hotkeys
                key.startsWith("pad.autoMapping.") || key == "pad.section.bindings" -> Controller1
                key == "pad.controllerType.player2" -> Controller2
                key.startsWith("pad.gyro.") || key.startsWith("pad.macro") ||
                    key == "pad.section.macros" -> Settings
                key.startsWith("pad.gesture.") -> Touch
                key.startsWith("pad.padProfiles.") -> Controller1
                key.startsWith("pad.leftStick") || key.startsWith("pad.rightStick") ||
                    key.startsWith("pad.stickFeel.") || key.startsWith("pad.dpadAsLeftStick.") -> Controller1
                key.startsWith("pad.onScreenControls.") || key.startsWith("pad.touchHaptics.") ||
                    key.startsWith("pad.compactShoulders.") || key.startsWith("pad.multiTouch.") ||
                    key.startsWith("touch.editor.") || key == "pad.section.onScreenControls" -> Touch
                key.startsWith("pad.rumble.") || key.startsWith("pad.hapticStrength.") ||
                    key.startsWith("pad.testRumble.") || key.startsWith("pad.pressureAmount.") ||
                    key.startsWith("pad.editing.") || key.startsWith("pad.controllerType.") ||
                    key in setOf("pad.section.playerRumble", "pad.section.analogSticks",
                        "pad.section.buttonMapping", "pad.section.padProfiles") -> Controller1
                else -> null
            }
        }
    }
}

/** Section selection for the Controls page opened from the library sidebar. */
internal object ControllerSections {
    val selected = mutableStateOf(ControllerSection.Settings)
    /** Activity input must see shoulders before generic frontend navigation consumes them. */
    @Volatile var cycle: ((Int) -> Unit)? = null
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ControllerSectionTabs(selected: ControllerSection, onSelect: (ControllerSection) -> Unit) {
    val title = str("pad.sections.label")
    val latestSelect = rememberUpdatedState(onSelect)
    DisposableEffect(Unit) {
        // Read the owner's state per press, not the last composed frame: rapid
        // presses from one or several pads must each advance exactly once.
        val cycle: (Int) -> Unit = { delta -> latestSelect.value(ControllerSections.selected.value.stepped(delta)) }
        ControllerSections.cycle = cycle
        onDispose {
            if (ControllerSections.cycle === cycle) ControllerSections.cycle = null
        }
    }
    fun step(delta: Int) = onSelect(selected.stepped(delta))
    Text(title, style = MaterialTheme.typography.titleSmall)
    // Material's scrollable tab row keeps the selected tab visible on small screens,
    // including when controller Left/Right selects an off-screen section.
    SecondaryScrollableTabRow(
        selectedTabIndex = selected.ordinal,
        edgePadding = 0.dp,
        containerColor = Color.Transparent,
        indicator = {},
        divider = {},
        modifier = Modifier.fillMaxWidth().controllerFocusable("segmented:$title",
            onConfirm = { step(1) }, onLeft = { step(-1) }, onRight = { step(1) }, showOutline = false),
    ) {
        ControllerSection.entries.forEach { section ->
            val active = section == selected
            val shape = RoundedCornerShape(11.dp)
            Tab(
                selected = active,
                onClick = { onSelect(section) },
                modifier = Modifier.padding(horizontal = 4.dp, vertical = 6.dp)
                    .clip(shape)
                    .background(if (active) MaterialTheme.colorScheme.primaryContainer else Color.Transparent),
                selectedContentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                text = { Text(str(section.labelKey), maxLines = 1, style = MaterialTheme.typography.labelLarge) },
            )
        }
    }
}

/** Standalone sidebar tabs show content directly; Settings and the quick menu keep compact groups. */
@Composable
internal fun ControllerSectionBody(title: String, standalone: Boolean, content: @Composable () -> Unit) {
    if (standalone) Column { content() }
    else CollapsibleSection(title, initiallyExpanded = false, content = content)
}

@Composable
internal fun ControllerPageSection(show: Boolean, title: String, tabbed: Boolean, content: @Composable () -> Unit) {
    if (show) ControllerSectionBody(title, tabbed, content)
}
