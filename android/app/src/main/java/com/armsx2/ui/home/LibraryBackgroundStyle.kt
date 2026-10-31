package com.armsx2.ui.home

/** Separate from ThemeMode: changing the backdrop must not replace the user's UI palette. */
enum class LibraryBackgroundStyle {
    Original, PsOne, CustomImage;

    companion object {
        fun restored(stored: String?, hasCustomImage: Boolean): LibraryBackgroundStyle {
            val fallback = if (hasCustomImage) CustomImage else Original
            val selected = entries.firstOrNull { it.name == stored } ?: fallback
            return if (selected == CustomImage && !hasCustomImage) Original else selected
        }
    }
}
