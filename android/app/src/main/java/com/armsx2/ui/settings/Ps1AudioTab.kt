package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Ps1Settings
import com.armsx2.i18n.str

/**
 * Audio tab — the `[audio]` table of the PS1 core's `settings.toml`.
 *
 * Ported from ARMSX2's Audio tab, but only the parts that reach something real here. The PS2
 * build drives PCSX2's SPU2 through an `AudioStream` with a time-stretcher, a device enumerator
 * and a DPL2 upmixer; ARMSX1 has an SDL device fed by a queue the emulation thread fills once
 * per frame (`ArmsxSession::queueAudioForFrame`) and a 24-voice SPU in `psx/dev/spu.c`. Each row
 * below names what consumes it. What was deliberately LEFT OUT, and why:
 *
 *  - Audio Synchronization / Time Stretch — SPU2 hands its `AudioStream` to SoundTouch. There is
 *    no time-stretcher in this tree and vendoring one for a fast-forward nicety is not a setting,
 *    it is a project. Fast-forward resamples instead, which shifts pitch; "Mute during
 *    fast-forward" below is the escape hatch.
 *  - Output Latency — a second latency knob on top of the buffer. It is a no-op even in ARMSX2
 *    (`OboeAudioStream` never reads it). Here there is exactly one buffer and "Audio buffer" is it.
 *  - SPU2 SIMD audio (NEON reverb) — a NEON fast path for the PS2 SPU2 reverb FIR. The PS1 reverb
 *    is a completely different (much smaller) all-pass/comb network with no SIMD backend to select.
 *  - Expansion / DPL2 upmix modes — PS2-only, and not surfaced on ARMSX2's Android tab either.
 */
@Composable
fun Ps1AudioTab() {
    val editor = rememberPs1SettingsEditor()
    val s = editor.value
    val scroll = settingsScrollState()
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            // Says nothing about WHICH file this lands in — the scope decides, and the old wording
            // ("saved to settings.toml") described the global file while the screen could be
            // showing a single game. Ps1ScopeNote above states the scope; each row carries its
            // winning layer as a badge.
            str("audio.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        // Consumed by ArmsxSession::applyOutputShaping — a straight gain on the mixed frame.
        val resetVolume: (() -> Unit)? =
            if (s.audioVolume != Ps1Settings().audioVolume) {
                { editor.update { it.copy(audioVolume = Ps1Settings().audioVolume) } }
            } else {
                null
            }
        IntSliderRow(
            label = str("audio.master.label"),
            value = s.audioVolume.coerceIn(0, 200),
            min = 0,
            max = 200,
            description = str("audio.master.description"),
            valueFormatter = { "$it%" },
            onReset = resetVolume,
            onChange = { v -> editor.update { it.copy(audioVolume = v) } },
        )
        SettingsDivider()

        ToggleRow(
            str("audio.muted.label"),
            s.audioMuted,
            description = str("audio.muted.description"),
        ) { v -> editor.update { it.copy(audioMuted = v) } }
        SettingsDivider()

        ToggleRow(
            str("audio.swap.label"),
            s.audioSwapChannels,
            description = str("audio.swap.description"),
        ) { v -> editor.update { it.copy(audioSwapChannels = v) } }
        SettingsDivider()

        // desired.samples in ArmsxSession::create(); 13 ms reproduces the core's historical
        // 588-frame (one CD sector) buffer.
        SegmentedRow(
            label = str("audio.bufferMs.label"),
            options = Ps1Settings.AUDIO_BUFFER_MS.map { "$it ms" },
            // Falls back to the core default's slot rather than slot 0 when settings.toml holds a
            // hand-edited value that is not on the list — slot 0 would silently read as "5 ms".
            selectedIndex = Ps1Settings.AUDIO_BUFFER_MS.indexOf(s.audioBufferMs)
                .takeIf { it >= 0 } ?: Ps1Settings.AUDIO_BUFFER_MS.indexOf(Ps1Settings().audioBufferMs),
            description = str("audio.bufferMs.description"),
            onChange = { idx -> editor.update { it.copy(audioBufferMs = Ps1Settings.AUDIO_BUFFER_MS[idx]) } },
        )
        SettingsDivider()

        // psx_spu_set_reverb_disabled(); bypasses spu_get_reverb_sample(), the SPU's hot spot.
        ToggleRow(
            str("audio.skipReverb.label"),
            s.audioSkipReverb,
            description = str("audio.skipReverb.description"),
        ) { v -> editor.update { it.copy(audioSkipReverb = v) } }
        SettingsDivider()

        // frontend/android_jni.cpp forces openslES as the floor; the core only honours anything
        // else once the SDLAudioManager Context is in place (applyAudioDriverSetting).
        SegmentedRow(
            label = str("audio.backend.label"),
            options = listOf(str("audio.backend.openSles"), str("audio.backend.aaudio")),
            selectedIndex = Ps1Settings.AUDIO_DRIVERS.indexOf(s.audioDriver).coerceAtLeast(0),
            description = str("audio.backend.description"),
            onChange = { idx -> editor.update { it.copy(audioDriver = Ps1Settings.AUDIO_DRIVERS[idx]) } },
        )
        SettingsDivider()

        // [audio] background_playback -> psxe_config_t::audio_background_playback, checked in
        // ArmsxApp::applyHostControlRequests() and by MainActivityRuntime's lifecycle.
        ToggleRow(
            str("audio.background.label"),
            s.audioBackgroundPlayback,
            description = str("audio.background.description"),
        ) { v -> editor.update { it.copy(audioBackgroundPlayback = v) } }
        SettingsDivider()

        Text(
            str("audio.fastForward.header"),
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 15.sp,
            modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
        )
        Text(
            str("audio.fastForward.note"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 6.dp),
        )

        ToggleRow(
            str("audio.muteFastForward.label"),
            s.audioMuteFastForward,
            description = str("audio.muteFastForward.description"),
        ) { v -> editor.update { it.copy(audioMuteFastForward = v) } }
        SettingsDivider()

        val resetFfVolume: (() -> Unit)? =
            if (s.audioFastForwardVolume != Ps1Settings().audioFastForwardVolume) {
                { editor.update { it.copy(audioFastForwardVolume = Ps1Settings().audioFastForwardVolume) } }
            } else {
                null
            }
        IntSliderRow(
            label = str("audio.ffVolume.label"),
            value = s.audioFastForwardVolume.coerceIn(0, 200),
            min = 0,
            max = 200,
            description = str("audio.ffVolume.description"),
            valueFormatter = { "$it%" },
            onReset = resetFfVolume,
            onChange = { v -> editor.update { it.copy(audioFastForwardVolume = v) } },
        )
        SettingsDivider()

        Ps1ActionRow(
            label = str("audio.reset.label"),
            controllerId = "ps1.audio.reset",
            description = str("settings.resetTab.description"),
        ) {
            val d = Ps1Settings()
            editor.update {
                it.copy(
                    audioVolume = d.audioVolume,
                    audioFastForwardVolume = d.audioFastForwardVolume,
                    audioMuted = d.audioMuted,
                    audioMuteFastForward = d.audioMuteFastForward,
                    audioSwapChannels = d.audioSwapChannels,
                    audioSkipReverb = d.audioSkipReverb,
                    audioBufferMs = d.audioBufferMs,
                    audioDriver = d.audioDriver,
                    audioBackgroundPlayback = d.audioBackgroundPlayback,
                )
            }
        }
    }
}
