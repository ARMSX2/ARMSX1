package com.armsx2.ui.settingshub

import com.armsx2.navigation.SettingsCategory

/** Keep the tab strip, shoulder navigation and search on the same visible categories. */
internal fun settingsCategoryVisible(category: SettingsCategory, gameSpecific: Boolean): Boolean =
    when (category) {
        // Hotkeys and Skins show in both, as in ARMSX2/ARMSX3: they are app-wide, and the sidebar
        // pages that used to be their only global home are gone.
        SettingsCategory.Controls, SettingsCategory.Patches, SettingsCategory.Info -> gameSpecific
        SettingsCategory.General, SettingsCategory.About -> !gameSpecific
        else -> true
    }

/** Old global manager routes must not reopen duplicate settings pages. */
internal fun visibleSettingsCategory(category: SettingsCategory, gameSpecific: Boolean): SettingsCategory =
    if (settingsCategoryVisible(category, gameSpecific)) category
    else if (gameSpecific) SettingsCategory.Performance else SettingsCategory.General
