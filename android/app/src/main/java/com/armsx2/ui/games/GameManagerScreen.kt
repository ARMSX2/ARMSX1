package com.armsx2.ui.games

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.bios.BiosManagerScreen
import com.armsx2.ui.memorycards.MemoryCardScreen
import com.armsx2.ui.saves.SaveManagerScreen
import com.armsx2.ui.settings.CollapsibleSection
import com.armsx2.ui.settings.ControllerAutoScroll
import com.armsx2.ui.settings.LocalSettingsScrollState

/** Expandable managers share the page's scroll while retaining their existing actions. */
@Composable
fun GameManagerScreen(onBack: () -> Unit) {
    val scroll = rememberScrollState()
    val sections = rememberSaveableStateHolder()
    ControllerAutoScroll(scroll)
    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(scroll)) {
            ArmsTopBar(
                title = str("tab.gameManager"),
                leading = { RoundAction("←", str("action.back"), onBack) },
            )
            CompositionLocalProvider(LocalSettingsScrollState provides scroll) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    CollapsibleSection(str("setup.step.bios.title")) {
                        sections.SaveableStateProvider("bios") {
                            BiosManagerScreen(onBack = {}, embedded = true)
                        }
                    }
                    CollapsibleSection(str("memcard.title")) {
                        // Preserve the selected game and pending document-picker target on collapse.
                        sections.SaveableStateProvider("memorycards") {
                            MemoryCardScreen(onBack = {}, embedded = true)
                        }
                    }
                    CollapsibleSection(str("savestate.title.loadManage")) {
                        sections.SaveableStateProvider("savestates") {
                            SaveManagerScreen(onBack = {}, embedded = true)
                        }
                    }
                }
            }
        }
    }
}
