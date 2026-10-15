package com.armsx2.ui.theme

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.armsx2.runtime.MainActivityRuntime

object InterfacePreferences {
    private const val Key = "ui.interface.style"
    val style = mutableStateOf(InterfaceStyle.Original)
    val psOne: Boolean get() = style.value == InterfaceStyle.PsOne

    fun load() {
        style.value = InterfaceStyle.restored(MainActivityRuntime.prefs.getString(Key, null))
    }

    fun set(value: InterfaceStyle) {
        style.value = value
        MainActivityRuntime.prefs.edit().putString(Key, value.name).apply()
    }
}

// An original, console-inspired shell; no Sony logos, firmware artwork or sounds are bundled.
internal val PsOneScheme = lightColorScheme(
    primary = Color(0xFF304FC0), onPrimary = Color.White,
    primaryContainer = Color(0xFFDCE4FF), onPrimaryContainer = Color(0xFF152965),
    secondary = Color(0xFF26705B), onSecondary = Color.White,
    secondaryContainer = Color(0xFFD7E9DF), onSecondaryContainer = Color(0xFF183F33),
    tertiary = Color(0xFF965077), onTertiary = Color.White,
    tertiaryContainer = Color(0xFFF5DEEC), onTertiaryContainer = Color(0xFF51213F),
    background = Color(0xFFE0E1DC), onBackground = Color(0xFF242735),
    surface = Color(0xFFF1F1EA), onSurface = Color(0xFF242735),
    surfaceVariant = Color(0xFFD7D9D4), onSurfaceVariant = Color(0xFF4A505B),
    surfaceContainerLowest = Color(0xFFFAFAF5), surfaceContainerLow = Color(0xFFECEDE7),
    surfaceContainer = Color(0xFFE6E7E0), surfaceContainerHigh = Color(0xFFDDDFD7),
    surfaceContainerHighest = Color(0xFFD4D7CF),
    outline = Color(0xFF858B8D), outlineVariant = Color(0xFFB5BAB5),
    error = Color(0xFFA32139), onError = Color.White,
    scrim = Color.Black, surfaceTint = Color.Transparent,
)

internal val PsOneShapes = Shapes(
    extraSmall = RoundedCornerShape(6.dp), small = RoundedCornerShape(12.dp),
    medium = RoundedCornerShape(18.dp), large = RoundedCornerShape(24.dp),
    extraLarge = RoundedCornerShape(32.dp),
)
