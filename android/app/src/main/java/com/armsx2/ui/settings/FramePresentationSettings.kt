package com.armsx2.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.material3.Text
import androidx.compose.material3.MaterialTheme
import kotlinx.coroutines.delay
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.ui.Modifier
import com.armsx2.config.Ps1Settings

@Composable
internal fun FramePresentationSettings(s: Ps1Settings, onMode: (String) -> Unit, onMultiplier: (Int) -> Unit) {
    val status by produceState(initialValue="Checking presentation status") {
        while(true){value=runCatching { kr.co.iefriends.pcsx2.NativeApp.framePresentationStatus() }.getOrDefault("Presentation status unavailable");delay(500)}
    }
    val modes = listOf("off", "bfi", "shader_subframes")
    Column(Modifier.fillMaxWidth()) {
    SegmentedRow(
        label = "High-refresh presentation (experimental)",
        options = listOf("Off", "Black frame insertion", "Shader subframes"),
        selectedIndex = modes.indexOf(s.framePresentation).coerceAtLeast(0),
        description = "Requires a matching display refresh rate: 2× needs 120 Hz for NTSC or " +
            "100 Hz for PAL. Inactive at incompatible refresh rates, during fast-forward or rewind. " +
            "Black frame insertion causes flicker and reduces brightness; avoid it if sensitive " +
            "to flashing images. Shader subframes require an enabled, subframe-aware shader " +
            "and increase GPU load. Neither changes game speed.",
        onChange = { i -> onMode(modes[i]) },
    )
    Text(status,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
    if (s.framePresentation != "off") {
        SegmentedRow(
            label = "Presentation multiplier",
            options = listOf("2×", "3×", "4×"),
            selectedIndex = s.presentationMultiplier.coerceIn(2, 4) - 2,
            description = "Display refresh must equal the game's frame rate × this value. " +
                "BFI presents one game image followed by black frames. Default is Off.",
            onChange = { i -> onMultiplier(i + 2) },
        )
    }
    }
}
