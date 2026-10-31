package com.armsx2.config

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class GlobalShaderSettingsTest {
    @Test fun globalPresetAndParametersReachEveryGameWithoutAShaderOverride() {
        val preset = "/shaders/crt.slangp"
        val global = Settings(shaderChainEnabled = true, shaderChainPreset = preset,
            shaderChainParams = mapOf(preset to mapOf("Tint" to 0.4f)))
        for (overrides in listOf(JSONObject(), JSONObject().put("affinityMode", 2))) {
            val effective = Settings.merge(global, overrides)
            assertTrue(effective.shaderChainEnabled)
            assertEquals(preset, effective.shaderChainPreset)
            assertEquals(global.shaderChainParams, effective.shaderChainParams)
        }
    }

    @Test fun laterGlobalPresetChangesFlowThroughUnrelatedGameOverrides() {
        val overrides = JSONObject().put("affinityMode", 1)
        val global = Settings(shaderChainEnabled = true, shaderChainPreset = "/shaders/new.slangp")
        assertEquals(global.shaderChainPreset, Settings.merge(global, overrides).shaderChainPreset)
        val pinned = JSONObject().put("shaderChainPreset", "/shaders/game.slangp").put("shaderChainEnabled", false)
        val effective = Settings.merge(global, pinned)
        assertFalse(effective.shaderChainEnabled)
        assertEquals("/shaders/game.slangp", effective.shaderChainPreset)
        assertTrue(global.shaderChainEnabled)
    }

    @Test fun clearingShaderOverridesPreservesOtherGameChoices() {
        val overrides = JSONObject().put("affinityMode", 2).put("shaderChainEnabled", false)
            .put("shaderChainPreset", "/old.slangp").put("shaderChainParams", JSONObject())
        listOf("shaderChainEnabled", "shaderChainPreset", "shaderChainParams").forEach(overrides::remove)
        val global = Settings(shaderChainEnabled = true, shaderChainPreset = "/global.slangp")
        val effective = Settings.merge(global, overrides)
        assertEquals(2, effective.affinityMode)
        assertTrue(effective.shaderChainEnabled)
        assertEquals(global.shaderChainPreset, effective.shaderChainPreset)
    }
}
