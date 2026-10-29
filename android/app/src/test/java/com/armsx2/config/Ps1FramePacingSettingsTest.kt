package com.armsx2.config

import com.armsx2.navigation.SettingsCategory
import com.armsx2.ui.settings.ps1KeysForCategory
import org.junit.Assert.*
import org.junit.Test

class Ps1FramePacingSettingsTest {
    private fun parse(text: String) = Ps1SettingsStore.javaClass
        .getDeclaredMethod("parse", String::class.java).apply { isAccessible = true }
        .invoke(Ps1SettingsStore, text) as Ps1Settings
    private fun serialize(s: Ps1Settings) = Ps1SettingsStore.javaClass
        .getDeclaredMethod("serialize", Ps1Settings::class.java).apply { isAccessible = true }
        .invoke(Ps1SettingsStore, s) as String
    @Test fun pacingSurvivesRoundTrip() {
        val expected = Ps1Settings(optimalFramePacing = true, syncHostRefresh = true,
            reduceInputLatency = true, frameTimeBufferMs = 4, maxQueuedFrames = 3,
            displayAspectCustom = 1.5f)
        assertEquals(expected, parse(serialize(expected)))
    }
    @Test fun missingAndMalformedConfigsUseVerifiedDefaults() {
        val old = parse("")
        assertTrue(old.optimalFramePacing)
        assertTrue(old.threadedHardwareBackend)
        assertFalse(old.threadedGlSubmission)
        assertFalse(old.syncHostRefresh)
        assertFalse(old.reduceInputLatency)
        val bad = parse("[video]\noptimal_frame_pacing=oops\nframe_time_buffer_ms=1000\nmax_queued_frames=-9")
        assertTrue(bad.optimalFramePacing)
        assertEquals(10, bad.frameTimeBufferMs)
        assertEquals(1, bad.maxQueuedFrames)
        assertEquals(2, old.frameTimeBufferMs)
        assertEquals(2, old.maxQueuedFrames)
    }
    @Test fun pacingBelongsToPerGameOverridesAndVideoReset() {
        val keys = listOf("video.optimal_frame_pacing", "video.sync_host_refresh", "video.reduce_input_latency",
            "video.frame_time_buffer_ms", "video.max_queued_frames")
        assertTrue(Ps1SettingsStore.PER_GAME_KEYS.containsAll(keys))
        assertTrue(ps1KeysForCategory(SettingsCategory.Graphics).containsAll(keys))
        val overlay = Ps1SettingsStore.javaClass.getDeclaredMethod("overlay", String::class.java, Map::class.java)
            .apply { isAccessible = true }
        val merged = overlay.invoke(Ps1SettingsStore, serialize(Ps1Settings()),
            mapOf("video.optimal_frame_pacing" to "true", "video.max_queued_frames" to "1")) as String
        assertTrue(parse(merged).optimalFramePacing)
        assertEquals(1, parse(merged).maxQueuedFrames)
    }
}
