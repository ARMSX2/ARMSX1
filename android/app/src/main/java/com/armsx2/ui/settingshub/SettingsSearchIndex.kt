// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.ui.settingshub

import com.armsx2.navigation.SettingsCategory

/**
 * Settings-search index: the label of every settings-row widget
 * (ToggleRow/IntSliderRow/SegmentedRow/CollapsibleSection/...) in each settings tab, mapped to its
 * hosting category. Labels that are i18n keys are resolved (and matched against the English source
 * too, so search works in every language); raw-literal labels are matched as-is.
 *
 * Trimmed for the PS1 port. Roughly 180 PCSX2 entries were removed here (GS hacks, VU/EE clamps and
 * round modes, DEV9 networking, SPU2, the emulator OSD toggles, PNACH patches) because the tabs that
 * hosted them are gone: a stale entry is not harmless, it hands the user a search hit that switches
 * to a tab and then fails to find the row, which reads as a broken search. The PS1 rows are i18n
 * keys now that their tabs are: the jump matches on the RENDERED label, so an English literal here
 * would miss every row in every other language.
 */
internal data class SettingsSearchEntry(val text: String, val isI18nKey: Boolean, val category: SettingsCategory)

internal val SETTINGS_SEARCH_INDEX: List<SettingsSearchEntry> = listOf(
    SettingsSearchEntry("app.bootLogo", true, SettingsCategory.General),
    SettingsSearchEntry("app.library.search", true, SettingsCategory.General),
    SettingsSearchEntry("app.library.recents", true, SettingsCategory.General),
    SettingsSearchEntry("app.library.coverSize", true, SettingsCategory.General),
    SettingsSearchEntry("games.overflow.gridNames", true, SettingsCategory.General),
    SettingsSearchEntry("app.backup.export", true, SettingsCategory.General),
    SettingsSearchEntry("app.backup.import", true, SettingsCategory.General),
    SettingsSearchEntry("app.blockHome", true, SettingsCategory.General),
    SettingsSearchEntry("app.theme", true, SettingsCategory.General),
    SettingsSearchEntry("app.toolbarPosition", true, SettingsCategory.General),
    SettingsSearchEntry("app.launcherRotation", true, SettingsCategory.General),
    SettingsSearchEntry("app.bgColor", true, SettingsCategory.General),
    SettingsSearchEntry("app.menuSfx", true, SettingsCategory.General),
    SettingsSearchEntry("update.includeNightly", true, SettingsCategory.General),
    SettingsSearchEntry("pad.analogStart.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.pressureAmount.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.rumble.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.gyro.mode.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.gyro.sensitivity.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.gyro.smoothing.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.gyro.invertX.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.gyro.invertY.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.multitap4.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.leftStick.swapXY.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.leftStick.invertX.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.leftStick.invertY.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.rightStick.swapXY.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.rightStick.invertX.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.rightStick.invertY.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.dpadAsLeftStick.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.touchHaptics.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.onScreenControls.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("touch.editor.multiTouchOn", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.stickFeel.deadzone.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.stickFeel.outerDeadzone.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.stickFeel.antiDeadzone.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.stickFeel.sensitivity.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.stickFeel.acceleration.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.editing.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.leftStick.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.rightStick.label", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.section.playerRumble", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.section.analogSticks", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.section.buttonMapping", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.section.onScreenControls", true, SettingsCategory.Controls),
    SettingsSearchEntry("pad.section.macros", true, SettingsCategory.Controls),
    SettingsSearchEntry("hotkeys.exitToLauncher.label", true, SettingsCategory.Hotkeys),
    SettingsSearchEntry("overlay.toggle.fastForwardPopups", true, SettingsCategory.OnScreen),
    SettingsSearchEntry("overlay.uiSize.label", true, SettingsCategory.OnScreen),
    SettingsSearchEntry("overlay.uiFontSize.label", true, SettingsCategory.OnScreen),

    // --- PS1 core settings ---
    // Video tab — settings.toml [video] + [runtime].display_scale
    SettingsSearchEntry("renderer.gpuBackend.label", true, SettingsCategory.Graphics),
    // The real upscale, and the two rows a user searching for "resolution" or "upscale" is
    // actually looking for. Both existed on the Video tab from the start and neither was indexed.
    SettingsSearchEntry("renderer.hwRasterizer.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.internalRes.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.pgxp.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.widescreenHack.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.textureFilter.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.textureReplacements.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.textureDump.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.textureDir.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.downsample.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.lineDetect.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.deinterlace.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.overscan.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.rotation.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.verticalSync.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.bilinear.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.integerScaling.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.aspect.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.wideUpscale.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.debugPanel.label", true, SettingsCategory.Graphics),
    // Host CPU levers, in the Video tab's Device group next to Sustained performance. Indexed
    // because neither name is one a user would think to look for under "Graphics" — "affinity"
    // and "ADPF" are the words they will type.
    SettingsSearchEntry("renderer.outputScale.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.screenRes.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.lowLatency.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.sustainedPerf.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.adpf.label", true, SettingsCategory.Graphics),
    SettingsSearchEntry("renderer.affinity.label", true, SettingsCategory.Graphics),
    // Emulation tab — settings.toml [cpu] + [console] + [runtime]
    SettingsSearchEntry("perf.cpuMode.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.region.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.skipBios.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.frameLimit.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.emulationSpeed.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.fpsCap.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.frameSkipMode.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.fastForwardSpeed.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.rewind.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.rewindBuffer.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.rewindPrecision.label", true, SettingsCategory.Performance),
    SettingsSearchEntry("perf.runahead.label", true, SettingsCategory.Performance),
    // Audio tab — settings.toml [audio]
    SettingsSearchEntry("audio.master.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.muted.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.swap.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.bufferMs.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.skipReverb.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.backend.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.background.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.muteFastForward.label", true, SettingsCategory.Audio),
    SettingsSearchEntry("audio.ffVolume.label", true, SettingsCategory.Audio),
    // BIOS tab — settings.toml [bios]. Only the model grid is listed: the rest of that tab is
    // info/action rows, and SettingsControllerNav.selectByLabel can only jump to
    // toggle:/segmented:/segmented-grid:/slider: ids, so indexing them would produce hits that
    // switch tabs and then highlight nothing.
    SettingsSearchEntry("bios.model.label", true, SettingsCategory.Bios),
    // Library tab — settings.toml [library]
    SettingsSearchEntry("library.scanSubfolders", true, SettingsCategory.Network),
    // Advanced tab — settings.toml [runtime] logging + [paths]
    SettingsSearchEntry("advanced.logging.label", true, SettingsCategory.Advanced),
    SettingsSearchEntry("advanced.logLevel.label", true, SettingsCategory.Advanced),
    SettingsSearchEntry("advanced.quiet.label", true, SettingsCategory.Advanced),
    // Cheats. The per-cheat rows are NOT indexed and cannot be: their labels are the names in
    // a user's own .cht file, which this list cannot know. The master switch and the three
    // actions are what a search for "cheat" needs to land on. (SettingsCategory.Patches is the
    // Cheats tab — see settingsSections().)
    SettingsSearchEntry("cheats.master.label", true, SettingsCategory.Patches),
    SettingsSearchEntry("cheats.action.paste", true, SettingsCategory.Patches),
    SettingsSearchEntry("cheats.action.import", true, SettingsCategory.Patches),
    SettingsSearchEntry("cheats.action.reload", true, SettingsCategory.Patches),
)
