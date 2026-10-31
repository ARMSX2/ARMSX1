package com.armsx2.config

import org.junit.Assert.*
import org.junit.Test

class ChangedSettingsTest {
    @Test fun factoryDefaultsHaveNoChangedMarkers() {
        assertTrue(Ps1SettingsStore.changedFromDefaults(Ps1Settings()).isEmpty())
    }

    @Test fun changesAreGroupedByTheirPersistedKeysAndClearOnReset() {
        val defaults = Ps1Settings()
        val edited = defaults.copy(internalScale = 3, fastBoot = true, audioVolume = 75)
        assertEquals(setOf("video.internal_scale", "cpu.fast_boot", "audio.volume"),
            Ps1SettingsStore.changedFromDefaults(edited))
        assertTrue(Ps1SettingsStore.changedFromDefaults(edited.copy(internalScale = defaults.internalScale,
            fastBoot = defaults.fastBoot, audioVolume = defaults.audioVolume)).isEmpty())
    }

    @Test fun inheritedGlobalChangesStillDifferFromFactoryDefaults() {
        val global = Ps1Settings(displayAspect = "wide16x9", fpsLimit = 30)
        assertEquals(setOf("video.display_aspect", "runtime.fps_limit"), Ps1SettingsStore.changedFromDefaults(global))
    }
}
