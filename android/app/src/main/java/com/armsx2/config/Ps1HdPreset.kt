package com.armsx2.config

/** Shortcuts over existing video settings, not a new persisted layer or texture pack. */
enum class Ps1HdPreset(val scale: Int) {
    NATIVE(1), HD_2X(2), HD_3X(3), HD_4X(4);

    fun applyTo(settings: Ps1Settings): Ps1Settings = settings.copy(
        internalScale = scale,
        // Native means native resolution, not disabling a user's hardware renderer/PGXP/packs.
        hwRasterizer = settings.hwRasterizer || scale > 1,
        downsample = 0,
    )

    companion object {
        /** No matching chip is selected when manual resolution/downsampling is custom. */
        fun selected(settings: Ps1Settings): Ps1HdPreset? {
            val scale = if (settings.hwRasterizer) settings.internalScale else 1
            return entries.firstOrNull { it.scale == scale && settings.downsample == 0 }
        }
    }
}
