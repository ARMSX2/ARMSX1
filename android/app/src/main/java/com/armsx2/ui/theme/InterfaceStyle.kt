package com.armsx2.ui.theme

/** Separate from colour and wallpaper preferences: switching back must restore both. */
enum class InterfaceStyle {
    Original, PsOne;

    companion object {
        fun restored(value: String?): InterfaceStyle = entries.firstOrNull { it.name == value } ?: Original
    }
}
