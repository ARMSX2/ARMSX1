package com.armsx2.config

import org.junit.Assert.*
import org.junit.Test

class Ps1PerformanceSettingsTest {
    private fun parse(text: String): Ps1Settings = Ps1SettingsStore.javaClass
        .getDeclaredMethod("parse", String::class.java).apply { isAccessible = true }
        .invoke(Ps1SettingsStore, text) as Ps1Settings
    private fun serialize(s: Ps1Settings): String = Ps1SettingsStore.javaClass
        .getDeclaredMethod("serialize", Ps1Settings::class.java).apply { isAccessible = true }
        .invoke(Ps1SettingsStore, s) as String

    @Test fun missingKeysUseThreadedHardwareDefaultAndRetainCpuDefaults() {
        val s = parse("[cpu]\nexecution_mode=\"cached\"")
        assertEquals(Ps1Settings.CPU_CACHED, s.cpuEngine)
        assertEquals("LUT", s.cpuFastmemMode)
        assertTrue(s.cpuBlockLinking)
        assertTrue(s.softwareGpuThread)
        assertTrue(s.cdReadThread)
        assertFalse(s.threadedGlSubmission)
        assertTrue(s.threadedHardwareBackend)
        assertFalse(s.cpuFastmemRewrite)
        assertEquals("Lazy", s.shaderPrecompile)
    }
    @Test fun allOptionsSurviveSavingAndReloading() {
        val s = Ps1Settings(cpuEngine = Ps1Settings.CPU_CACHED_INTERPRETER,
            cpuFastmemMode = "MMap", cpuBlockLinking = false, cpuInstructionCache = true,
            cpuFastmemRewrite = true, cdReadThread = false, threadedGlSubmission = true,
            threadedHardwareBackend = true,
            softwareGpuThread = false, softwareReadbacks = true, shaderPrecompile = "Enabled",
            texturePreload = true, displayAspectCustom = 1.5f)
        assertEquals(s, parse(serialize(s)))
    }
    @Test fun performanceOptionsParticipateInSparsePerGameOverrides() {
        assertTrue(Ps1SettingsStore.PER_GAME_KEYS.containsAll(listOf(
            "cpu.fastmem_mode", "cpu.block_linking", "cpu.instruction_cache", "cpu.fastmem_rewrite",
            "cpu.cd_read_thread", "video.threaded_gl_submission", "video.threaded_hardware_backend", "video.software_gpu_thread",
            "video.software_readbacks", "video.shader_precompile", "video.texture_preload")))
        val overlay = Ps1SettingsStore.javaClass.getDeclaredMethod("overlay", String::class.java, Map::class.java)
            .apply { isAccessible = true }
        val global = serialize(Ps1Settings(cpuBlockLinking = false))
        val text = overlay.invoke(Ps1SettingsStore, global, mapOf("video.threaded_hardware_backend" to "true")) as String
        val s = parse(text)
        assertTrue(s.threadedHardwareBackend)
        assertFalse(s.threadedGlSubmission)
        assertFalse(s.cpuBlockLinking)
        assertEquals("LUT", s.cpuFastmemMode)
    }
    @Test fun malformedNewValuesUseSafeDefaults() {
        val s = parse("[cpu]\nfastmem_mode=\"invalid\"\nblock_linking=oops\nfastmem_rewrite=oops\n" +
            "[video]\nthreaded_gl_submission=oops\nthreaded_hardware_backend=oops\nsoftware_gpu_thread=oops\nshader_precompile=\"invalid\"")
        assertEquals("LUT", s.cpuFastmemMode)
        assertTrue(s.cpuBlockLinking)
        assertFalse(s.cpuFastmemRewrite)
        assertFalse(s.threadedGlSubmission)
        assertTrue(s.threadedHardwareBackend)
        assertTrue(s.softwareGpuThread)
        assertEquals("Lazy", s.shaderPrecompile)
    }
    @Test fun explicitDisabledHardwareWorkerSurvivesRoundTrip() {
        val s = parse("[video]\nthreaded_hardware_backend=false\noptimal_frame_pacing=false")
        assertFalse(s.threadedHardwareBackend)
        assertFalse(s.optimalFramePacing)
        val roundTrip = parse(serialize(s))
        assertFalse(roundTrip.threadedHardwareBackend)
        assertFalse(roundTrip.optimalFramePacing)
    }
    @Test fun legacyGlWorkerChoiceDoesNotAlsoEnableTheNewDefaultWorker() {
        val s = parse("[video]\nthreaded_gl_submission=true")
        assertTrue(s.threadedGlSubmission)
        assertFalse(s.threadedHardwareBackend)
        val overlay = Ps1SettingsStore.javaClass.getDeclaredMethod("overlay", String::class.java, Map::class.java)
            .apply { isAccessible = true }
        val merged = overlay.invoke(Ps1SettingsStore, serialize(Ps1Settings()),
            mapOf("video.threaded_gl_submission" to "true")) as String
        assertTrue(parse(merged).threadedGlSubmission)
        assertFalse(parse(merged).threadedHardwareBackend)
    }
}
