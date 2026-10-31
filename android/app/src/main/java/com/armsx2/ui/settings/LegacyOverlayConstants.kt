package com.armsx2.ui.settings

/**
 * Leftovers from the PS2 settings tabs that the in-game quick menu
 * (`ui/emulation/EmulationMenuScreen.kt`) still references.
 *
 * The tab that declared them — OverlayTab's OSD block — is gone in the PS1 port; the colour list
 * lives on because the pause menu's OSD colour row still cycles it.
 *
 * UPSCALE_OPTIONS (0.25x-8x, PCSX2's upscale multiplier) was removed with the pause menu's Upscale
 * row. That row now writes the real PlayStation lever — `[video] renderer` + `internal_scale` in
 * settings.toml, 1x-8x on the hardware rasteriser — so the PS2 list had no reader left.
 */

/** OSD text colours as 0xRRGGBB, index-aligned with [OSD_COLOR_LABEL_KEYS]. 0 = default white. */
internal val OSD_COLORS = listOf(
    0x000000, // default (white — 0 means "unset" to the renderer)
    0x66FF66, // green
    0x66E0FF, // cyan
    0xFFE066, // yellow
    0xFFA64D, // orange
    0xFF6666, // red
    0xFF7AC8, // pink
    0xC08CFF, // purple
)

/** i18n keys for [OSD_COLORS], same order. */
internal val OSD_COLOR_LABEL_KEYS = listOf(
    "overlay.osdColor.default", "overlay.osdColor.green", "overlay.osdColor.cyan",
    "overlay.osdColor.yellow", "overlay.osdColor.orange", "overlay.osdColor.red",
    "overlay.osdColor.pink", "overlay.osdColor.purple",
)
