package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Ps1Settings
import com.armsx2.i18n.str

/**
 * Emulation tab — the PS1 core's `[cpu]`, `[console]` and `[runtime]` frame-pacing sections.
 *
 * The PS2 build's Performance tab (EE cycle rate/skip, VU clamp and round modes, MTVU,
 * INTC/wait-loop hacks, frame skip…) has no counterpart: `frontend/config.c` exposes exactly
 * `execution_mode` and `region` for the core proper, and both are read once while it boots.
 *
 * The speed rows below are different: they are the frame pacer, they apply live (through
 * `Ps1Pacing` → `NativeApp.setSpeedLimits` → `ArmsxSession::targetFrameRate`), and they are the
 * same four values the in-game pause menu edits.
 */
@Composable
fun Ps1EmulationTab() {
    val editor = rememberPs1SettingsEditor()
    val s = editor.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            str("perf.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        SegmentedRow(
            label = str("perf.cpuMode.label"),
            options = listOf(str("perf.cpuMode.cached"), str("perf.cpuMode.interpreter")),
            selectedIndex = Ps1Settings.CPU_ENGINES.indexOf(s.cpuEngine).coerceAtLeast(0),
            description = str("perf.cpuMode.description"),
            onChange = { idx -> editor.update { it.copy(cpuEngine = Ps1Settings.CPU_ENGINES[idx]) } },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("perf.region.label"),
            // NTSC / PAL are the broadcast standards' own names, left as-is in every language.
            options = listOf(str("common.auto"), "NTSC", "PAL"),
            selectedIndex = Ps1Settings.REGIONS.indexOf(s.region).coerceAtLeast(0),
            description = str("perf.region.description"),
            onChange = { idx -> editor.update { it.copy(region = Ps1Settings.REGIONS[idx]) } },
        )
        SettingsDivider()

        ToggleRow(
            str("perf.skipBios.label"),
            s.fastBoot,
            description = str("perf.skipBios.description"),
        ) { v -> editor.update { it.copy(fastBoot = v) } }
        SettingsDivider()

        ToggleRow(
            str("perf.frameLimit.label"),
            s.frameLimit,
            description = str("perf.frameLimit.description"),
        ) { v -> editor.update { it.copy(frameLimit = v) } }
        SettingsDivider()

        SegmentedRow(
            label = str("perf.emulationSpeed.label"),
            options = Ps1Settings.SPEED_PERCENTS.map { "$it%" },
            selectedIndex = Ps1Settings.SPEED_PERCENTS.indexOf(s.speedPercent)
                .takeIf { it >= 0 } ?: Ps1Settings.SPEED_PERCENTS.indexOf(100),
            description = str("perf.emulationSpeed.description"),
            onChange = { idx -> editor.update { it.copy(speedPercent = Ps1Settings.SPEED_PERCENTS[idx]) } },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("perf.fpsCap.label"),
            options = Ps1Settings.FPS_LIMITS.map { if (it == 0) str("common.off") else "$it" },
            selectedIndex = Ps1Settings.FPS_LIMITS.indexOf(s.fpsLimit).takeIf { it >= 0 } ?: 0,
            description = str("perf.fpsCap.description"),
            onChange = { idx -> editor.update { it.copy(fpsLimit = Ps1Settings.FPS_LIMITS[idx]) } },
        )
        SettingsDivider()

        // The second entry point for frame skip; the in-game pause menu's Performance pane is the
        // other. Both write [runtime] frame_skip and both live-apply through Ps1Pacing — fixing
        // only one of a pair like this is how a feature stays half-broken on this project.
        SegmentedRow(
            label = str("perf.frameSkipMode.label"),
            options = Ps1Settings.FRAME_SKIPS.map { Ps1Settings.frameSkipLabel(it) },
            selectedIndex = Ps1Settings.FRAME_SKIPS.indexOf(s.frameSkip).takeIf { it >= 0 } ?: 0,
            description = str("perf.frameSkipMode.description"),
            onChange = { idx -> editor.update { it.copy(frameSkip = Ps1Settings.FRAME_SKIPS[idx]) } },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("perf.fastForwardSpeed.label"),
            options = Ps1Settings.FAST_FORWARD_SPEEDS.map { Ps1Settings.fastForwardSpeedLabel(it) },
            selectedIndex = Ps1Settings.FAST_FORWARD_SPEEDS.indexOf(s.fastForwardSpeed)
                .takeIf { it >= 0 } ?: Ps1Settings.FAST_FORWARD_SPEEDS.indexOf(2f),
            description = str("perf.fastForwardSpeed.description"),
            onChange = { idx ->
                editor.update { it.copy(fastForwardSpeed = Ps1Settings.FAST_FORWARD_SPEEDS[idx]) }
            },
        )
        SettingsDivider()

        // ---- Rewind ------------------------------------------------------------------
        //
        // The buffer length is shown WITH its memory cost, every time, because on a handheld
        // that is the setting: one PS1 snapshot is ~3.5 MB, so 60 s at 4/s is ~840 MB and
        // would simply be killed by the OS. The figure comes from the core
        // (NativeApp.rewindSnapshotBytes) rather than a constant here, so it is the real
        // measured size as soon as a game has run.
        val snapshotBytes = remember { com.armsx2.core.Ps1Emulation.snapshotBytes() }
        val rewindCost = Ps1Settings.rewindBufferLabel(s.rewindSeconds, s.rewindFrequency, snapshotBytes)
        // What the memory budget really allows. Shown when it is less than what was asked
        // for, so a 60s buffer that the core will only keep ~28s of says so on the row
        // instead of quietly under-delivering.
        val rewindReal = Ps1Settings.rewindEffectiveSeconds(s.rewindSeconds, s.rewindFrequency, snapshotBytes)
        val rewindCapNote =
            if (rewindReal < s.rewindSeconds) {
                " " + str("perf.rewindBuffer.capped").format(rewindReal)
            } else {
                ""
            }

        ToggleRow(
            str("perf.rewind.label"),
            s.rewind,
            description = str("perf.rewind.description").format(rewindCost),
            idSuffix = "ps1.rewind",
        ) { v -> editor.update { it.copy(rewind = v) } }
        SettingsDivider()

        SegmentedRow(
            label = str("perf.rewindBuffer.label"),
            options = Ps1Settings.REWIND_SECONDS.map { "${it}s" },
            selectedIndex = Ps1Settings.REWIND_SECONDS.indexOf(s.rewindSeconds).takeIf { it >= 0 }
                ?: Ps1Settings.REWIND_SECONDS.indexOf(10),
            description = str("perf.rewindBuffer.description").format(rewindCost, rewindCapNote),
            onChange = { idx -> editor.update { it.copy(rewindSeconds = Ps1Settings.REWIND_SECONDS[idx]) } },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("perf.rewindPrecision.label"),
            options = Ps1Settings.REWIND_FREQUENCIES.map { "$it/s" },
            selectedIndex = Ps1Settings.REWIND_FREQUENCIES.indexOf(s.rewindFrequency)
                .takeIf { it >= 0 } ?: Ps1Settings.REWIND_FREQUENCIES.indexOf(2),
            description = str("perf.rewindPrecision.description"),
            onChange = { idx ->
                editor.update { it.copy(rewindFrequency = Ps1Settings.REWIND_FREQUENCIES[idx]) }
            },
        )
        SettingsDivider()

        // ---- Runahead ----------------------------------------------------------------
        SegmentedRow(
            label = str("perf.runahead.label"),
            options = Ps1Settings.RUNAHEAD_FRAMES.map { if (it == 0) str("common.off") else "$it" },
            selectedIndex = Ps1Settings.RUNAHEAD_FRAMES.indexOf(s.runahead).takeIf { it >= 0 } ?: 0,
            description = str("perf.runahead.description"),
            onChange = { idx -> editor.update { it.copy(runahead = Ps1Settings.RUNAHEAD_FRAMES[idx]) } },
        )
        SettingsDivider()

        Ps1ActionRow(
            label = str("perf.reset.label"),
            controllerId = "ps1.emulation.reset",
            description = str("perf.reset.description"),
        ) {
            val d = Ps1Settings()
            editor.update {
                it.copy(
                    cpuEngine = d.cpuEngine,
                    region = d.region,
                    frameLimit = d.frameLimit,
                    speedPercent = d.speedPercent,
                    fpsLimit = d.fpsLimit,
                    frameSkip = d.frameSkip,
                    fastForwardSpeed = d.fastForwardSpeed,
                    rewind = d.rewind,
                    rewindSeconds = d.rewindSeconds,
                    rewindFrequency = d.rewindFrequency,
                    runahead = d.runahead,
                )
            }
        }
    }
}
