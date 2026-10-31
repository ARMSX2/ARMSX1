package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Modifier
import com.armsx2.BuildConfig
import kr.co.iefriends.pcsx2.NativeApp

private val pacingMode = mutableIntStateOf(0)

/** Per-process test mode, deliberately not part of normal/per-game user settings. */
@Composable
internal fun PacingDiagnosticSection() {
    if (!BuildConfig.PACING_DIAGNOSTICS) return
    Column(Modifier.fillMaxWidth()) {
        SegmentedRow(
            label = "Frame pacing diagnostic A/B",
            options = listOf("A: Baseline timer", "B: VSync + timer"),
            selectedIndex = pacingMode.intValue,
            description = "OpenGL/ANGLE test only. Baseline is the normal behaviour. " +
                "B adds a VSync wait while keeping the speed limiter; it may improve or worsen pacing. " +
                "Vulkan is unchanged. BFI/shader subframes must be Off. Resets to A when the app restarts.",
            onChange = {
                if (runCatching { NativeApp.setPacingDiagnosticMode(it) }.isSuccess) pacingMode.intValue = it
            },
        )
        Text("Reproduce the same battle in A, then B, at the same resolution. " +
            "Save a diagnostic report immediately after each run in Settings → App. " +
            "The report labels actual display timestamps as unavailable if the driver cannot supply them. " +
            "Do not enable broad debug logging, BFI or heavy shaders for the baseline comparison.",
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
