package com.armsx2.ui.settings

import com.armsx2.navigation.SettingsCategory
import org.junit.Assert.assertTrue
import org.junit.Test

class Ps1PerformanceRowsTest {
    @Test fun emulationTabResetIncludesTheNewCpuOptions() {
        assertTrue(ps1KeysForCategory(SettingsCategory.Performance).containsAll(listOf(
            "cpu.fastmem_mode", "cpu.block_linking", "cpu.instruction_cache", "cpu.fastmem_rewrite", "cpu.cd_read_thread")))
    }
    @Test fun videoTabResetIncludesTheNewGpuOptions() {
        assertTrue(ps1KeysForCategory(SettingsCategory.Graphics).containsAll(listOf(
            "video.threaded_gl_submission", "video.threaded_hardware_backend", "video.software_gpu_thread", "video.software_readbacks",
            "video.shader_precompile", "video.texture_preload")))
    }
}
