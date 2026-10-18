package com.armsx2.ui.settingshub

import com.armsx2.navigation.SettingsCategory

/** Keep the tab strip, shoulder navigation and search on the same visible categories. */
internal fun settingsCategoryVisible(category: SettingsCategory, gameSpecific: Boolean): Boolean =
    when (category) {
        SettingsCategory.Controls, SettingsCategory.Hotkeys, SettingsCategory.Patches, SettingsCategory.Skins,
        SettingsCategory.Info -> gameSpecific
        SettingsCategory.General, SettingsCategory.About -> !gameSpecific
        else -> true
    }

/** Old global manager routes must not reopen duplicate settings pages. */
internal fun visibleSettingsCategory(category: SettingsCategory, gameSpecific: Boolean): SettingsCategory =
    if (settingsCategoryVisible(category, gameSpecific)) category
    else if (gameSpecific) SettingsCategory.Performance else SettingsCategory.General
