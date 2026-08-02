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
import androidx.compose.ui.platform.LocalContext
import com.armsx2.config.Ps1Settings
import com.armsx2.core.Ps1Textures
import com.armsx2.i18n.str

/**
 * Video tab — the `[video]` + `[runtime].display_scale` half of the PS1 core's `settings.toml`.
 *
 * Every row here maps 1:1 to a key `frontend/config.c` parses:
 *   gpu_backend, vsync, texture_scale_mode, stretch_mode, display_aspect, wide_upscale,
 *   debug_panel  (+ display_scale from `[runtime]`).
 *
 * There is no upscale multiplier and no blending-accuracy level: the PS1 GPU emulation has
 * neither. `wide_upscale` is the only resolution lever and it only applies to the widescreen
 * output path, hence the guard note on that row.
 *
 * Texture dumping and replacement ARE here (`texture_dump`, `texture_replacements`,
 * `texture_dir`), but they are a rewrite rather than the PS2 feature of the same name — the
 * PlayStation has no texture objects to key off, so the core hashes the VRAM region a
 * primitive samples. See `psx/texrep.h`.
 */
@Composable
fun Ps1VideoTab() {
    val editor = rememberPs1SettingsEditor()
    val s = editor.value
    val scroll = settingsScrollState()
    val context = LocalContext.current
    ControllerAutoScroll(scroll)

    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            // No claim about WHERE this is saved: that depends on the scope, and this text used to
            // say "settings.toml" — i.e. the global file — while the screen was titled with a
            // game's name. Ps1ScopeNote (rendered by Ps1Scoped above this tab) states the scope,
            // and each row is badged with the layer that wins.
            str("renderer.intro"),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 14.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        SegmentedRow(
            label = str("renderer.gpuBackend.label"),
            options = Ps1Settings.GPU_BACKEND_LABELS,
            selectedIndex = Ps1Settings.GPU_BACKENDS.indexOf(s.gpuBackend).coerceAtLeast(0),
            description = str("renderer.gpuBackend.description"),
            onChange = { idx -> editor.update { it.copy(gpuBackend = Ps1Settings.GPU_BACKENDS[idx]) } },
        )
        SettingsDivider()

        // The custom-driver managers, matched to the backend selected above. These were reachable
        // ONLY from the in-game pause menu, so choosing Vulkan here offered no way to install or
        // pick a driver pack — the same screen, two different answers depending on how you got to
        // it. Same components as the pause menu uses, so the two cannot drift apart.
        if (s.gpuBackend == Ps1Settings.GPU_VULKAN) {
            com.armsx2.ui.common.DriverManagerSection()
            SettingsDivider()
        } else if (s.gpuBackend == Ps1Settings.GPU_OPENGL || s.gpuBackend == Ps1Settings.GPU_ANGLE) {
            com.armsx2.ui.common.AngleDriverSection(s.gpuBackend == Ps1Settings.GPU_ANGLE) { on ->
                editor.update {
                    it.copy(gpuBackend = if (on) Ps1Settings.GPU_ANGLE else Ps1Settings.GPU_OPENGL)
                }
            }
            SettingsDivider()
        }

        // Display scale is NOT shown. It reaches applyWindowMetrics() -> SDL_SetWindowSize, which
        // is gated behind SupportsManagedWindowSizing() — false on Android, where the surface is
        // always fullscreen — and it does not raise render resolution either. It did nothing on a
        // phone, so a row for it was a control that could only mislead. The field and the
        // settings.toml key stay: the core still honours them on desktop.

        // The rasteriser is a SEPARATE axis from the backend above: that one picks how the
        // finished frame is presented, this one picks what draws it. Only the hardware
        // rasteriser can render above native resolution.
        ToggleRow(
            str("renderer.hwRasterizer.label"),
            s.hwRasterizer,
            // The old text warned that this "costs roughly twice the CPU even at 1x, and more as
            // the scale rises". That describes the CPU FALLBACK rasteriser, not the one that
            // normally runs: the GLES path draws on the GPU, so raising the scale costs GPU time
            // and essentially no extra CPU (measured: 2x was 90.9 fps against 91.4 at 1x). The
            // warning told users the opposite of the truth about the feature's whole point.
            description = str("renderer.hwRasterizer.description"),
        ) { v -> editor.update { it.copy(hwRasterizer = v) } }
        SettingsDivider()

        ToggleRow(
            str("renderer.pgxp.label"),
            s.pgxp,
            description = str("renderer.pgxp.description"),
        ) { v ->
            editor.update { it.copy(pgxp = v) }
            runCatching { kr.co.iefriends.pcsx2.NativeApp.setPgxpEnabled(v) }
        }
        SettingsDivider()

        ToggleRow(
            str("renderer.maskBit.label"),
            s.accurateMaskBit,
            description = str("renderer.maskBit.description"),
        ) { v -> editor.update { it.copy(accurateMaskBit = v) } }
        SettingsDivider()

        ToggleRow(
            str("renderer.dither.label"),
            s.accurateDither,
            description = str("renderer.dither.description"),
        ) { v -> editor.update { it.copy(accurateDither = v) } }
        SettingsDivider()

        // ---- Display / video feature set -------------------------------------------------
        // Each of these writes settings.toml AND pushes the matching native, so the change is
        // visible on the next frame instead of at the next launch. Every one defaults to
        // off/neutral, and the native default matches, so a fresh install behaves exactly as
        // this build did before they existed.

        ToggleRow(
            str("renderer.widescreenHack.label"),
            s.widescreenHack,
            description = str("renderer.widescreenHack.description"),
        ) { v ->
            editor.update { it.copy(widescreenHack = v) }
            runCatching { kr.co.iefriends.pcsx2.NativeApp.setWidescreenHack(v) }
        }
        SettingsDivider()

        SegmentedRow(
            label = str("renderer.textureFilter.label"),
            options = Ps1Settings.TEXTURE_FILTER_LABELS,
            selectedIndex = Ps1Settings.indexOfOrZero(
                Ps1Settings.TEXTURE_FILTERS, s.textureFilter,
            ),
            description = str("renderer.textureFilter.description"),
            onChange = { idx ->
                val next = Ps1Settings.TEXTURE_FILTERS[idx]
                editor.update { it.copy(textureFilter = next) }
                pushGlVideoOptions(next, s.downsample, s.lineDetect)
            },
        )
        SettingsDivider()

        // ---- Texture replacement / dumping (psx/texrep.h) -------------------------------
        // Deliberately next to Texture filtering, and deliberately on this MAIN tab rather
        // than behind a manager screen: the PS2 build put its (unrelated) texture feature in
        // a drawer entry, where nobody found it.
        //
        // Replacement is listed first because it is the one a player uses; dumping is the
        // authoring half. Both push live — psx_texrep_configure() rebuilds the pack index on
        // the emulation thread, so a pack dropped in while a game runs is picked up without
        // a restart.
        ToggleRow(
            str("renderer.textureReplacements.label"),
            s.textureReplacements,
            description = str("renderer.textureReplacements.description"),
        ) { v ->
            editor.update { it.copy(textureReplacements = v) }
            Ps1Textures.push(context, s.copy(textureReplacements = v))
        }
        SettingsDivider()

        ToggleRow(
            str("renderer.textureDump.label"),
            s.textureDump,
            description = str("renderer.textureDump.description"),
        ) { v ->
            editor.update { it.copy(textureDump = v) }
            Ps1Textures.push(context, s.copy(textureDump = v))
        }
        SettingsDivider()

        // "It is on" is useless without "and the files are HERE". Shown whenever either half
        // is on, and resolved through the same helper the push uses, so the path on screen is
        // the path the core was actually handed — including the per-game serial folder.
        if (s.textureDump || s.textureReplacements) {
            Ps1InfoRow(
                label = str("renderer.textureFolder.label"),
                value = Ps1Textures.resolveDir(context, s),
                description = str("renderer.textureFolder.description"),
            )
            SettingsDivider()
        }

        // Escape hatch for a pack on shared storage. Blank restores the per-game default,
        // which is why this is a text row and not a folder picker: a picker cannot express
        // "go back to automatic".
        Ps1TextRow(
            label = str("renderer.textureDir.label"),
            value = s.textureDir,
            controllerId = "ps1.video.textureDir",
            description = str("renderer.textureDir.description"),
            placeholder = str("renderer.textureDir.placeholder"),
        ) { v ->
            editor.update { it.copy(textureDir = v.trim()) }
            Ps1Textures.push(context, s.copy(textureDir = v.trim()))
        }
        SettingsDivider()

        // Shown UNCONDITIONALLY, even though it does nothing until the internal resolution is
        // above 1x. Hiding a row until some other setting is right is how a feature gets
        // reported missing — the requirement belongs in the description, not in a visibility
        // condition. The active-or-not line below says which of the two it currently is.
        SegmentedRow(
            label = str("renderer.downsample.label"),
            options = Ps1Settings.DOWNSAMPLE_FACTORS.map { Ps1Settings.downsampleLabel(it) },
            selectedIndex = Ps1Settings.DOWNSAMPLE_FACTORS.indexOf(s.downsample)
                .coerceAtLeast(0),
            description = str("renderer.downsample.description") + " " +
                if (s.hwRasterizer && s.internalScale > 1) {
                    str("renderer.downsample.active").format(s.internalScale)
                } else {
                    str("renderer.downsample.inactive")
                },
            onChange = { idx ->
                val next = Ps1Settings.DOWNSAMPLE_FACTORS[idx]
                editor.update { it.copy(downsample = next) }
                pushGlVideoOptions(s.textureFilter, next, s.lineDetect)
            },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("renderer.lineDetect.label"),
            options = Ps1Settings.LINE_DETECT_LABELS,
            selectedIndex = Ps1Settings.indexOfOrZero(Ps1Settings.LINE_DETECTS, s.lineDetect),
            description = str("renderer.lineDetect.description"),
            onChange = { idx ->
                val next = Ps1Settings.LINE_DETECTS[idx]
                editor.update { it.copy(lineDetect = next) }
                pushGlVideoOptions(s.textureFilter, s.downsample, next)
            },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("renderer.deinterlace.label"),
            options = Ps1Settings.DEINTERLACE_LABELS,
            selectedIndex = Ps1Settings.indexOfOrZero(Ps1Settings.DEINTERLACES, s.deinterlace),
            description = str("renderer.deinterlace.description"),
            onChange = { idx ->
                val next = Ps1Settings.DEINTERLACES[idx]
                editor.update { it.copy(deinterlace = next) }
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setDeinterlaceMode(idx) }
            },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("renderer.overscan.label"),
            options = Ps1Settings.OVERSCAN_LABELS,
            selectedIndex = Ps1Settings.indexOfOrZero(Ps1Settings.OVERSCAN_CROPS, s.overscanCrop),
            description = str("renderer.overscan.description"),
            onChange = { idx ->
                val next = Ps1Settings.OVERSCAN_CROPS[idx]
                editor.update { it.copy(overscanCrop = next) }
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setOverscanCrop(idx) }
            },
        )
        SettingsDivider()

        SegmentedRow(
            label = str("renderer.rotation.label"),
            options = Ps1Settings.DISPLAY_ROTATION_LABELS,
            selectedIndex = Ps1Settings.DISPLAY_ROTATIONS.indexOf(s.displayRotation)
                .coerceAtLeast(0),
            description = str("renderer.rotation.description"),
            onChange = { idx ->
                val next = Ps1Settings.DISPLAY_ROTATIONS[idx]
                editor.update { it.copy(displayRotation = next) }
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setDisplayRotation(next) }
            },
        )
        SettingsDivider()

        if (s.hwRasterizer) {
            val resetScaleInternal: (() -> Unit)? =
                if (s.internalScale != Ps1Settings().internalScale) {
                    { editor.update { it.copy(internalScale = Ps1Settings().internalScale) } }
                } else {
                    null
                }
            IntSliderRow(
                label = str("renderer.internalRes.label"),
                value = s.internalScale.coerceIn(1, 8),
                min = 1,
                max = 8,
                description = str("renderer.internalRes.description"),
                valueFormatter = { "${it}x" },
                onReset = resetScaleInternal,
                onChange = { v -> editor.update { it.copy(internalScale = v) } },
            )
            SettingsDivider()
        }

        // ---- Device output --------------------------------------------------------------------
        // HOST levers: they act on the phone's output surface and clock policy, not on the
        // emulated console. Stored in the lifted Settings/prefs rather than settings.toml, since
        // EmulationSurface and the Activity are what consume them.
        run {
            val cfg = com.armsx2.ui.InGameOverlay.settingsState
            val host = cfg.value
            fun applyHost(next: com.armsx2.config.Settings) {
                com.armsx2.ui.InGameOverlay.saveSettings(next)
                runCatching {
                    com.armsx2.runtime.MainActivityRuntime.surface.value?.applyOutputScale()
                }
            }

            SegmentedRow(
                label = str("renderer.outputScale.label"),
                options = listOf(
                    str("renderer.outputScale.screen"),
                    str("renderer.outputScale.native3x"),
                    str("renderer.outputScale.native2x"),
                    str("renderer.outputScale.native1x"),
                ),
                selectedIndex = when (host.hwScaler) { 3 -> 1; 2 -> 2; 1 -> 3; else -> 0 },
                description = str("renderer.outputScale.description"),
                onChange = { idx ->
                    applyHost(host.copy(hwScaler = when (idx) { 1 -> 3; 2 -> 2; 3 -> 1; else -> 0 }))
                },
            )
            SettingsDivider()

            val presets = listOf("auto", "2560x1440", "1920x1080", "1280x720")
            SegmentedRow(
                label = str("renderer.screenRes.label"),
                options = listOf(str("common.auto"), "1440p", "1080p", "720p"),
                selectedIndex = presets.indexOf(host.screenResOverride).coerceAtLeast(0),
                description = str("renderer.screenRes.description"),
                onChange = { idx -> applyHost(host.copy(screenResOverride = presets[idx])) },
            )
            SettingsDivider()

            ToggleRow(
                str("renderer.lowLatency.label"),
                host.vsyncQueueSize == 0,
                description = str("renderer.lowLatency.description"),
            ) { on ->
                com.armsx2.ui.InGameOverlay.saveSettings(
                    host.copy(vsyncQueueSize = if (on) 0 else 2),
                )
                runCatching {
                    com.armsx2.runtime.MainActivityRuntime.surface.value?.applyFrameRatePreference()
                }
            }
            SettingsDivider()

            val sustained = androidx.compose.runtime.remember {
                androidx.compose.runtime.mutableStateOf(
                    com.armsx2.runtime.MainActivityRuntime.prefs
                        .getBoolean("ui.sustainedPerf", false),
                )
            }
            ToggleRow(
                str("renderer.sustainedPerf.label"),
                sustained.value,
                description = str("renderer.sustainedPerf.description"),
            ) { on ->
                sustained.value = on
                com.armsx2.runtime.MainActivityRuntime.prefs.edit()
                    .putBoolean("ui.sustainedPerf", on).apply()
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
                    runCatching {
                        (com.armsx2.runtime.MainActivityRuntime.surface.value?.context
                            as? android.app.Activity)
                            ?.window?.setSustainedPerformanceMode(on)
                    }
                }
            }
            SettingsDivider()

            // The exact opposite lever to the one above, which is why they sit together:
            // Sustained performance CAPS the clock for thermal steadiness, ADPF asks for the
            // clock the frame's work actually needs. Native reports the emulation thread's WORK
            // per frame — not the frame's wall clock, which would include the limiter's sleep
            // and read as permanent max demand. frontend/perf_hint.c.
            val adpf = androidx.compose.runtime.remember {
                androidx.compose.runtime.mutableStateOf(
                    com.armsx2.runtime.MainActivityRuntime.prefs.getBoolean("ui.adpf", false),
                )
            }
            ToggleRow(
                str("renderer.adpf.label"),
                adpf.value,
                description = str("renderer.adpf.description"),
            ) { on ->
                adpf.value = on
                com.armsx2.runtime.MainActivityRuntime.prefs.edit()
                    .putBoolean("ui.adpf", on).apply()
                runCatching { kr.co.iefriends.pcsx2.NativeApp.setAdpfEnabled(on) }
            }
            SettingsDivider()

            // 0 off · 1 performance cores · 2 all cores. Stored in the lifted Settings (which
            // already carried affinityMode and already pushes it before runVMThread); native
            // also applies it live, so the row does not need a reboot to take effect.
            val affinityIndex = when (host.affinityMode) {
                1, 7 -> 1
                2 -> 2
                else -> 0
            }
            SegmentedRow(
                label = str("renderer.affinity.label"),
                options = listOf(
                    str("common.off"),
                    str("renderer.affinity.performanceCores"),
                    str("renderer.affinity.allCores"),
                ),
                selectedIndex = affinityIndex,
                description = str("renderer.affinity.description"),
                onChange = { idx ->
                    com.armsx2.ui.InGameOverlay.saveSettings(host.copy(affinityMode = idx))
                    runCatching { kr.co.iefriends.pcsx2.NativeApp.setAffinityMode(idx) }
                },
            )
            SettingsDivider()
        }

        ToggleRow(
            str("renderer.verticalSync.label"),
            s.vsync,
            description = str("renderer.verticalSync.description"),
        ) { v -> editor.update { it.copy(vsync = v) } }
        SettingsDivider()

        ToggleRow(
            str("renderer.bilinear.label"),
            s.textureScaleMode,
            description = str("renderer.bilinear.description"),
        ) { v -> editor.update { it.copy(textureScaleMode = v) } }
        SettingsDivider()

        ToggleRow(
            str("renderer.integerScaling.label"),
            s.integerScaling,
            description = str("renderer.integerScaling.description"),
        ) { v -> editor.update { it.copy(integerScaling = v) } }
        SettingsDivider()

        // Stretch is an ENTRY here, not a separate switch — same shape as the in-game menu.
        // As two controls they could contradict each other: "Stretch on" plus a highlighted "4:3"
        // showed a ratio that was not in effect, and this screen and the pause menu disagreed
        // about the same setting.
        SegmentedRow(
            label = str("renderer.aspect.label"),
            options = listOf(str("renderer.aspect.stretch")) + Ps1Settings.ASPECT_LABELS,
            selectedIndex = if (s.stretchMode) {
                0
            } else {
                Ps1Settings.ASPECTS.indexOf(s.displayAspect).coerceAtLeast(0) + 1
            },
            description = str("renderer.aspect.description"),
            onChange = { idx ->
                editor.update {
                    if (idx == 0) {
                        it.copy(stretchMode = true)
                    } else {
                        it.copy(stretchMode = false, displayAspect = Ps1Settings.ASPECTS[idx - 1])
                    }
                }
            },
        )
        SettingsDivider()

        if (!s.stretchMode && s.displayAspect == Ps1Settings.ASPECT_CUSTOM) {
            val presetIndex = Ps1Settings.ASPECT_CUSTOM_PRESETS.indexOfFirst {
                kotlin.math.abs(it.second - s.displayAspectCustom) < 0.001f
            }
            SegmentedRow(
                label = str("renderer.customRatio.label"),
                options = Ps1Settings.ASPECT_CUSTOM_PRESETS.map { it.first },
                selectedIndex = presetIndex.coerceAtLeast(0),
                description = str("renderer.customRatio.description").format(
                    s.displayAspectCustom, Ps1Settings.ASPECT_CUSTOM_MIN, Ps1Settings.ASPECT_CUSTOM_MAX,
                ),
                onChange = { idx ->
                    editor.update {
                        it.copy(displayAspectCustom = Ps1Settings.ASPECT_CUSTOM_PRESETS[idx].second)
                    }
                },
            )
            SettingsDivider()
        }

        SegmentedRow(
            label = str("renderer.wideUpscale.label"),
            options = Ps1Settings.WIDE_UPSCALES,
            selectedIndex = Ps1Settings.WIDE_UPSCALES.indexOf(s.wideUpscale).coerceAtLeast(0),
            // Same caveat as Display scale: wide_upscale only feeds SDL_SetWindowSize on desktop.
            // It is NOT an internal-resolution upscaler.
            description = str("renderer.wideUpscale.description"),
            onChange = { idx -> editor.update { it.copy(wideUpscale = Ps1Settings.WIDE_UPSCALES[idx]) } },
        )
        SettingsDivider()

        ToggleRow(
            str("renderer.debugPanel.label"),
            s.debugPanel,
            description = str("renderer.debugPanel.description"),
        ) { v -> editor.update { it.copy(debugPanel = v) } }
        SettingsDivider()

        Ps1ActionRow(
            label = str("renderer.reset.label"),
            controllerId = "ps1.video.reset",
            description = str("settings.resetTab.description"),
        ) {
            val d = Ps1Settings()
            editor.update {
                it.copy(
                    gpuBackend = d.gpuBackend,
                    hwRasterizer = d.hwRasterizer,
                    internalScale = d.internalScale,
                    displayScale = d.displayScale,
                    vsync = d.vsync,
                    textureScaleMode = d.textureScaleMode,
                    stretchMode = d.stretchMode,
                    displayAspect = d.displayAspect,
                    wideUpscale = d.wideUpscale,
                    debugPanel = d.debugPanel,
                    widescreenHack = d.widescreenHack,
                    textureFilter = d.textureFilter,
                    downsample = d.downsample,
                    deinterlace = d.deinterlace,
                    overscanCrop = d.overscanCrop,
                    displayRotation = d.displayRotation,
                    lineDetect = d.lineDetect,
                    textureDump = d.textureDump,
                    textureReplacements = d.textureReplacements,
                    textureDir = d.textureDir,
                )
            }
            // Reset has to reach the natives too, or the live overrides keep the old values
            // until the next launch and the tab shows one thing while the screen shows another.
            runCatching {
                kr.co.iefriends.pcsx2.NativeApp.setWidescreenHack(d.widescreenHack)
                kr.co.iefriends.pcsx2.NativeApp.setDeinterlaceMode(
                    Ps1Settings.indexOfOrZero(Ps1Settings.DEINTERLACES, d.deinterlace),
                )
                kr.co.iefriends.pcsx2.NativeApp.setOverscanCrop(
                    Ps1Settings.indexOfOrZero(Ps1Settings.OVERSCAN_CROPS, d.overscanCrop),
                )
                kr.co.iefriends.pcsx2.NativeApp.setDisplayRotation(d.displayRotation)
            }
            pushGlVideoOptions(d.textureFilter, d.downsample, d.lineDetect)
            // Texture dumping/replacement is live too, so the reset has to tear the native
            // subsystem down as well or it keeps running against the old pack.
            Ps1Textures.push(context, d)
        }
    }
}

/**
 * The three GLES-rasterizer options travel to the core as ONE call — the backend stores them
 * together and there is no read-modify-write on the Kotlin side — so every caller has to pass
 * all three, including the two it is not changing.
 */
private fun pushGlVideoOptions(textureFilter: String, downsample: Int, lineDetect: String) {
    runCatching {
        kr.co.iefriends.pcsx2.NativeApp.setGlVideoOptions(
            Ps1Settings.indexOfOrZero(Ps1Settings.TEXTURE_FILTERS, textureFilter),
            downsample,
            Ps1Settings.indexOfOrZero(Ps1Settings.LINE_DETECTS, lineDetect),
        )
    }
}
