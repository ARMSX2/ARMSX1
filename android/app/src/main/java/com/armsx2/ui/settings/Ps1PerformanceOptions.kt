package com.armsx2.ui.settings

import androidx.compose.runtime.Composable

/** Persisted in the same global/per-game layer as every other PS1 option. */
@Composable
internal fun Ps1CpuPerformanceOptions(editor: Ps1SettingsEditor) {
    val s = editor.value
    val fastmem = listOf("LUT", "MMap", "Disabled")
    SegmentedRow(
        label = "Recompiler memory access",
        options = listOf("LUT", "Memory mapped", "Disabled"),
        selectedIndex = fastmem.indexOf(s.cpuFastmemMode).coerceAtLeast(0),
        description = "LUT keeps the current default. Memory mapped access uses fault handling and may improve performance on some devices; it can also cause compatibility problems. Next game start; game compatibility rules may override it.",
        onChange = { index -> editor.update { it.copy(cpuFastmemMode = fastmem[index]) } },
    )
    SettingsDivider()
    ToggleRow("JIT block linking", s.cpuBlockLinking,
        description = "Directly links compiled CPU blocks to reduce dispatcher overhead. Already enabled by default. Next game start.") {
        on -> editor.update { it.copy(cpuBlockLinking = on) }
    }
    SettingsDivider()
    ToggleRow("Emulate instruction cache", s.cpuInstructionCache,
        description = "Improves timing accuracy for games that need it, at a performance cost. Not a speed hack. Next game start.") {
        on -> editor.update { it.copy(cpuInstructionCache = on) }
    }
    SettingsDivider()
    ToggleRow("Fastmem rewrites (advanced)", s.cpuFastmemRewrite,
        description = "Allows the JIT to rewrite fast-memory accesses. May help some workloads but can crash on incompatible devices. Off by default; next game start.") {
        on -> editor.update { it.copy(cpuFastmemRewrite = on) }
    }
    SettingsDivider()
    ToggleRow("Asynchronous disc reads", s.cdReadThread,
        description = "Reads disc sectors on a separate thread to reduce I/O stalls. Already enabled by default. Next game start.") {
        on -> editor.update { it.copy(cdReadThread = on) }
    }
    SettingsDivider()
}

@Composable
internal fun Ps1GpuPerformanceOptions(editor: Ps1SettingsEditor) {
    val s = editor.value
    ToggleRow("Threaded hardware backend (experimental)", s.threadedHardwareBackend,
        description = "Lets CPU emulation overlap OpenGL, ANGLE or Vulkan rendering and presentation using a bounded frame queue. Reads and save states synchronize. Runahead, GunCon and mouse use synchronous frames. Enabled by default; next game start.") {
        on -> editor.update { it.copy(threadedHardwareBackend = on, threadedGlSubmission = if (on) false else it.threadedGlSubmission) }
    }
    SettingsDivider()
    ToggleRow("Threaded GL submission (experimental)", s.threadedGlSubmission,
        description = "Queues hardware OpenGL/ANGLE driver commands on a dedicated worker. Queries and memory transfers synchronize; the queue is bounded. May improve or worsen performance. Off by default; next game start. Not used by native Vulkan or Software; this is not DuckStation's full threaded GPU architecture.") {
        on -> editor.update { it.copy(threadedGlSubmission = on, threadedHardwareBackend = if (on) false else it.threadedHardwareBackend) }
    }
    SettingsDivider()
    ToggleRow("Threaded software rendering", s.softwareGpuThread,
        description = "Uses SwanStation's existing software rasterizer worker. Already enabled by default. This is separate from hardware rendering; next game start.") {
        on -> editor.update { it.copy(softwareGpuThread = on) }
    }
    SettingsDivider()
    ToggleRow("Software VRAM readbacks", s.softwareReadbacks,
        description = "Runs a parallel software renderer to supply accurate VRAM reads for hardware rendering. Can help framebuffer effects but adds CPU work. Next game start.") {
        on -> editor.update { it.copy(softwareReadbacks = on) }
    }
    SettingsDivider()
    val modes = listOf("Lazy", "Enabled", "Disabled")
    SegmentedRow(
        label = "Shader compilation",
        options = listOf("Background / lazy", "Precompile at boot", "On demand"),
        selectedIndex = modes.indexOf(s.shaderPrecompile).coerceAtLeast(0),
        description = "Vulkan can precompile shaders in a background worker. OpenGL/ANGLE's lazy mode compiles when needed. Precompiling at boot trades startup time for fewer first-use stalls. Existing caches are retained; next game start.",
        onChange = { index -> editor.update { it.copy(shaderPrecompile = modes[index]) } },
    )
    SettingsDivider()
    ToggleRow("Preload replacement textures", s.texturePreload,
        description = "Loads installed replacement textures into RAM at startup, reducing later file reads. Uses more memory and only matters when replacements are enabled. Next game start.") {
        on -> editor.update { it.copy(texturePreload = on) }
    }
    SettingsDivider()
}

@Composable
internal fun Ps1FramePacingOptions(editor: Ps1SettingsEditor) {
    val s = editor.value
    ToggleRow("Optimal frame pacing", s.optimalFramePacing,
        description = "Schedules presentation against the frame clock and recent Android display timestamps. Preserves the game clock across display-phase differences; long stalls reset the schedule. Next game start.") {
        on -> editor.update { it.copy(optimalFramePacing = on) }
    }
    SettingsDivider()
    ToggleRow("Sync to host refresh rate", s.syncHostRefresh,
        description = "With VSync, matches a nearby display rate or integer multiple using at most a 1% speed adjustment. Resamples audio to prevent drift. Next game start.") {
        on -> editor.update { it.copy(syncHostRefresh = on) }
    }
    SettingsDivider()
    ToggleRow("Reduce input latency", s.reduceInputLatency,
        description = "With optimal frame pacing, starts emulation closer to the presentation deadline. Slower devices may miss frames; a larger frame-time buffer provides more headroom. Next game start.") {
        on -> editor.update { it.copy(reduceInputLatency = on) }
    }
    SettingsDivider()
    IntSliderRow("Frame-time buffer", s.frameTimeBufferMs, 0, 10,
        description = "Scheduling headroom in milliseconds. A larger buffer starts work earlier; a smaller buffer reduces latency. Next game start.",
        valueFormatter = { "$it ms" }, onChange = { value -> editor.update { it.copy(frameTimeBufferMs = value) } })
    SettingsDivider()
    SegmentedRow("Maximum queued frames", listOf("1", "2", "3"), s.maxQueuedFrames.coerceIn(1, 3) - 1,
        description = "Limits whole frames waiting on the threaded hardware backend and Vulkan submission resources. One reduces buffering; two allows more overlap; three tolerates more variation. Next game start.",
        onChange = { index -> editor.update { it.copy(maxQueuedFrames = index + 1) } })
    SettingsDivider()
}
