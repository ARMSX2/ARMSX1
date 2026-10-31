package com.armsx2.ui.controls

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.input.ControllerMappings
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.LocalPs1SettingsScope
import com.armsx2.ui.settings.LocalSettingsScrollState
import com.armsx2.ui.settings.PadTab
import com.armsx2.ui.settings.ControllerSections

/** Controls opened from the library drawer (also reachable from Controller mapping in-game). */
@Composable
fun ControllerManagerScreen(onBack: () -> Unit) {
    val scroll = rememberScrollState()
    val player = rememberSaveable { mutableIntStateOf(0) }
    val captureReset = remember { mutableIntStateOf(0) }
    val settings = remember { mutableStateOf(Settings()) }

    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(scroll)) {
            ArmsTopBar(
                title = str("tab.controls"),
                leading = { RoundAction("←", str("action.back"), onBack) },
                actions = {
                    if (ControllerSections.selected.value.editsPlayer) RoundAction("↺", str("action.reset"), {
                        ControllerMappings.reset(ControllerSections.selected.value.playerIndex ?: player.intValue)
                        ControllerMappings.stickBindTick.value++
                        captureReset.intValue++
                    })
                },
            )
            // This standalone manager has always edited global mappings. Do not inherit
            // a Game scope left behind by the main Settings page or an in-game overlay.
            CompositionLocalProvider(
                LocalSettingsScrollState provides scroll,
                LocalPs1SettingsScope provides null,
            ) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                    PadTab(
                        state = settings,
                        sectionTabs = true,
                        forceGlobal = true,
                        playerState = player,
                        captureReset = captureReset.intValue,
                    )
                }
            }
        }
    }
}
