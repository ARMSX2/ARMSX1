package com.armsx2.ui.settings

import android.os.Build
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.theme.LibraryBackgroundColorPreferences
import com.armsx2.ui.theme.ThemeMode
import com.armsx2.ui.theme.ThemePreferences

/** Existing appearance preferences, hosted only by sidebar Customisation. */
@Composable
fun ThemeAppearanceSection(showTitle: Boolean = true) {
    // Theme picker. NOT a SegmentedRow: that's a fixed-width Box, so eleven options would
    // squeeze into unreadable slivers — this wraps instead. Driven straight off the enum so
    // adding a colour needs no index bookkeeping; the old version mapped index<->mode by hand
    // in two separate places, which is precisely how such pairs drift out of sync.
    if (!com.armsx2.ui.theme.InterfacePreferences.psOne) Column(Modifier.fillMaxWidth().padding(vertical = 5.dp)) {
        if (showTitle) Text(str("app.theme"), style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(8.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            // Material You needs Android 12. Hide it below that rather than letting it fall
            // back silently — picking a theme and getting a different one reads as a bug.
            ThemeMode.entries.filter {
                !it.requiresDynamicColor || Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
            }.forEach { theme ->
                val apply = { ThemePreferences.set(theme) }
                FilterChip(
                    selected = ThemePreferences.mode.value == theme,
                    onClick = apply,
                    label = { Text(str("app.theme.${theme.name.lowercase()}")) },
                    shape = RoundedCornerShape(11.dp),
                    modifier = Modifier.controllerFocusable(
                        "app.theme.${theme.name}",
                        RoundedCornerShape(11.dp),
                        onConfirm = apply,
                    ),
                )
            }
        }

        // OLED black is a MODIFIER on whichever theme is chosen above, not a theme of its own,
        // so "OLED + purple" / "OLED + yellow" are possible (the standalone OLED chip stays as
        // the neutral blue-accent preset). A chip rather than a ToggleRow so it lives with the
        // theme chips and keeps the controllerFocusable registration pad navigation needs.
        Spacer(Modifier.height(10.dp))
        run {
            val toggleOled = { ThemePreferences.setOledBase(!ThemePreferences.oledBase.value) }
            FilterChip(
                selected = ThemePreferences.oledBase.value,
                onClick = toggleOled,
                label = { Text(str("app.theme.oledBase")) },
                shape = RoundedCornerShape(11.dp),
                modifier = Modifier.controllerFocusable(
                    "app.theme.oledBase",
                    RoundedCornerShape(11.dp),
                    onConfirm = toggleOled,
                ),
            )
        }

        // RGB picker, only while Custom is the active theme. The scheme is derived from
        // this colour's hue with saturation/brightness clamped (see customScheme), so the
        // accent stays recognisably what was picked without any channel combination being
        // able to produce unreadable chrome.
        if (ThemePreferences.mode.value == ThemeMode.Custom) {
            val argb = ThemePreferences.customColor.value
            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Surface(
                    modifier = Modifier.size(34.dp),
                    shape = RoundedCornerShape(9.dp),
                    color = Color(argb),
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
                ) {}
                Spacer(Modifier.width(10.dp))
                Text(
                    String.format("#%06X", 0xFFFFFF and argb),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            listOf(
                Triple("app.theme.custom.r", 16, android.graphics.Color.red(argb)),
                Triple("app.theme.custom.g", 8, android.graphics.Color.green(argb)),
                Triple("app.theme.custom.b", 0, android.graphics.Color.blue(argb)),
            ).forEach { (labelKey, shift, value) ->
                IntSliderRow(
                    label = str(labelKey),
                    value = value,
                    min = 0,
                    max = 255,
                    onChange = { channel ->
                        // Replace just this channel, keeping alpha opaque.
                        val cleared = argb and (0xFF shl shift).inv()
                        ThemePreferences.setCustomColor(cleared or (channel shl shift) or (0xFF shl 24))
                    },
                )
            }
        }
    }
}

@Composable
fun LibraryAppearanceSection() {
    LibraryBackgroundStyleSection(showTitle = false)

    if (com.armsx2.ui.home.LibraryBackground.style.value == com.armsx2.ui.home.LibraryBackgroundStyle.Original) {
    // Library background (wave) color. Separate from the theme accent above — that only tints
    // the UI chrome; this recolors the animated backdrop itself, with the white waves riding
    // over it. Unset = the built-in blue. Live: XmbGlView's GL thread reads the new color on
    // its next frame (~33ms), so the backdrop updates as the sliders move.
    Column(Modifier.fillMaxWidth().padding(vertical = 5.dp)) {
        Text(str("app.bgColor"), style = MaterialTheme.typography.titleMedium)
        Text(
            str("app.bgColor.desc"),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        // Opt into the lightweight 2D animated backdrop everywhere (the same one older Mali /
        // GL-fail devices already get) instead of the GLES3 XMB wave. The colour options below
        // apply to it too. No effect if a custom background image is set.
        ToggleRow(
            label = str("app.bg.simple"),
            value = com.armsx2.ui.home.LibraryBackground.animated2D.value,
            description = str("app.bg.simple.desc"),
            onChange = { com.armsx2.ui.home.LibraryBackground.setAnimated2D(it) },
        )
        // Continuous RGB hue-cycle — same idea as the theme's RGB mode. While on, the fixed
        // color (presets + sliders) doesn't apply, so it's hidden.
        ToggleRow(
            label = str("app.bgColor.rgb"),
            value = LibraryBackgroundColorPreferences.rgbCycle.value,
            description = str("app.bgColor.rgb.desc"),
            onChange = { LibraryBackgroundColorPreferences.setRgbCycle(it) },
        )
        if (!LibraryBackgroundColorPreferences.rgbCycle.value) {
        val customized = LibraryBackgroundColorPreferences.color.value != 0
        val argb = if (customized) LibraryBackgroundColorPreferences.color.value
                   else LibraryBackgroundColorPreferences.DefaultDisplayColor
        val r = android.graphics.Color.red(argb)
        val g = android.graphics.Color.green(argb)
        val b = android.graphics.Color.blue(argb)
        // Custom-slider mode: open when a non-preset colour is active, or the user taps "Custom".
        val customOpen = remember {
            mutableStateOf(customized && LibraryBackgroundColorPreferences.PRESETS.none { it == argb })
        }
        // Quick-pick presets (XMB palette) + a "Custom" chip that reveals the RGB sliders — the
        // same shape as the theme colour picker. The active swatch (or Custom) is ringed.
        Spacer(Modifier.height(9.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            LibraryBackgroundColorPreferences.PRESETS.forEach { preset ->
                val selected = !customOpen.value && preset == argb
                val pick = { customOpen.value = false; LibraryBackgroundColorPreferences.set(preset) }
                Surface(
                    onClick = pick,
                    modifier = Modifier.size(34.dp)
                        .controllerFocusable("app.bgColor.preset.$preset", RoundedCornerShape(9.dp), onConfirm = pick),
                    shape = RoundedCornerShape(9.dp),
                    color = Color(preset),
                    border = BorderStroke(
                        if (selected) 3.dp else 1.dp,
                        if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline,
                    ),
                ) {}
            }
            val pickCustom = {
                customOpen.value = true
                if (!customized) LibraryBackgroundColorPreferences.set(LibraryBackgroundColorPreferences.DefaultDisplayColor)
            }
            FilterChip(
                selected = customOpen.value,
                onClick = pickCustom,
                label = { Text(str("app.theme.custom")) },
                shape = RoundedCornerShape(9.dp),
                modifier = Modifier.controllerFocusable("app.bgColor.custom", RoundedCornerShape(9.dp), onConfirm = pickCustom),
            )
        }
        // Preview + RGB sliders only in Custom mode (mirrors the theme colour picker).
        if (customOpen.value) {
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Surface(
                    modifier = Modifier.size(width = 66.dp, height = 34.dp),
                    shape = RoundedCornerShape(9.dp),
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
                ) {
                    Box(
                        Modifier.fillMaxSize().background(
                            Brush.verticalGradient(
                                listOf(
                                    Color(r * 0.20f / 255f, g * 0.20f / 255f, b * 0.20f / 255f, 1f),
                                    Color(argb),
                                )
                            )
                        )
                    )
                }
                Spacer(Modifier.width(10.dp))
                Text(
                    String.format("#%06X", 0xFFFFFF and argb),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            listOf(
                Triple("app.theme.custom.r", 16, r),
                Triple("app.theme.custom.g", 8, g),
                Triple("app.theme.custom.b", 0, b),
            ).forEach { (labelKey, shift, value) ->
                IntSliderRow(
                    label = str(labelKey),
                    value = value,
                    min = 0,
                    max = 255,
                    onChange = { channel ->
                        val cleared = argb and (0xFF shl shift).inv()
                        LibraryBackgroundColorPreferences.set(cleared or (channel shl shift) or (0xFF shl 24))
                    },
                )
            }
        }
        if (customized) {
            Spacer(Modifier.height(4.dp))
            val reset = {
                customOpen.value = false
                LibraryBackgroundColorPreferences.reset()
            }
            OutlinedButton(
                onClick = reset,
                modifier = Modifier.controllerFocusable("app.bgColor.reset", onConfirm = reset),
            ) { Text(str("action.reset")) }
        }
        }
    }

    }
}
