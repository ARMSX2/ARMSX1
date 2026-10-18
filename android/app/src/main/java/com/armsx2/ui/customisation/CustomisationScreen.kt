package com.armsx2.ui.customisation

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.config.Settings
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.settings.LocalPs1SettingsScope
import com.armsx2.ui.settings.LocalSettingsScrollState
import com.armsx2.ui.settings.SkinsTab
import com.armsx2.ui.settings.CollapsibleSection
import com.armsx2.ui.settings.InterfaceStyleSection
import com.armsx2.ui.settings.ThemeAppearanceSection
import com.armsx2.ui.settings.LibraryAppearanceSection

/** App-wide appearance tools opened from the sidebar, independent of a stale game scope. */
@Composable
fun CustomisationScreen(onBack: () -> Unit) {
    val scroll = rememberScrollState()
    val settings = remember { mutableStateOf(Settings()) }
    ArmsBackdrop {
        Column(Modifier.fillMaxSize().verticalScroll(scroll)) {
            ArmsTopBar(
                title = str("tab.customisation"),
                leading = { RoundAction("←", str("action.back"), onBack) },
            )
            CompositionLocalProvider(
                LocalSettingsScrollState provides scroll,
                LocalPs1SettingsScope provides null,
            ) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                    CollapsibleSection(str("app.interface.style")) {
                        InterfaceStyleSection(showTitle = false)
                    }
                    if (!com.armsx2.ui.theme.InterfacePreferences.psOne) {
                        CollapsibleSection(str("app.theme")) {
                            ThemeAppearanceSection(showTitle = false)
                        }
                    }
                    CollapsibleSection(str("app.bg.style")) {
                        LibraryAppearanceSection()
                    }
                }
                SkinsTab(settings, forceGlobal = true, collapsibleOptions = true)
            }
        }
    }
}
