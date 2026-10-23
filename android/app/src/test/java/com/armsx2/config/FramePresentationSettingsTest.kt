package com.armsx2.config

import org.junit.Assert.*
import org.junit.Test

class FramePresentationSettingsTest {
    private fun parse(text: String): Ps1Settings {
        val method = Ps1SettingsStore.javaClass.getDeclaredMethod("parse", String::class.java).apply { isAccessible = true }
        return method.invoke(Ps1SettingsStore, text) as Ps1Settings
    }
    private fun serialize(s: Ps1Settings): String {
        val method = Ps1SettingsStore.javaClass.getDeclaredMethod("serialize", Ps1Settings::class.java).apply { isAccessible = true }
        return method.invoke(Ps1SettingsStore, s) as String
    }
    @Test fun defaultsDoNotEnableFlashingOrAdditionalShaderWork() {
        assertEquals("off", parse("").framePresentation)
        assertEquals(0, parse("").downsample)
    }
    @Test fun roundTripsNewVideoSettingsAndIncludesThemInOverrides() {
        val input = Ps1Settings(downsample = -1, framePresentation = "shader_subframes", presentationMultiplier = 3)
        val result = parse(serialize(input))
        assertEquals(-1, result.downsample)
        assertEquals(input.framePresentation, result.framePresentation)
        assertEquals(3, result.presentationMultiplier)
        assertTrue(Ps1SettingsStore.PER_GAME_KEYS.containsAll(listOf("video.frame_presentation", "video.presentation_multiplier", "video.downsample")))
    }
    @Test fun oldBoxValuesRemainReadableAndInvalidModesFailClosed() {
        assertEquals(3, parse("[video]\ndownsample=3").downsample)
        assertEquals(1, parse("[video]\ndownsample=true").downsample)
        val result = parse("[video]\nframe_presentation=\"invalid\"\npresentation_multiplier=999")
        assertEquals("off", result.framePresentation)
        assertEquals(4, result.presentationMultiplier)
    }
}
