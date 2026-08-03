package com.armsx2.config

import com.armsx2.i18n.I18n

/**
 * The PS1 core's real, persisted configuration — a 1:1 Kotlin mirror of the `settings.toml`
 * schema read by `frontend/config.c`. This replaces ARMSX2's ~2500-line PS2 `Settings.kt`; every
 * field here maps to a concrete TOML key the native emulator actually honours (verified against
 * config.c). Values the PS1 core doesn't have (VU clamps, upscale multipliers, blend levels,
 * patches, …) simply don't exist.
 *
 * Defaults match the native `g_default_settings` template exactly.
 */
data class Ps1Settings(
    // [cpu]
    val cpuEngine: String = CPU_CACHED,          // cached | interpreter
    // Skip BIOS. Boots the game's executable directly at the BIOS shell entry, skipping the
    // startup animation. The [cpu] table owns it because it is the CPU's entry state that gets
    // rewritten. Default true — the animation is not something anyone asks to sit through.
    val fastBoot: Boolean = true,
    // PGXP: full-precision GTE vertices + perspective-correct texturing in the hardware
    // rasteriser. Fixes the PS1's wobbling polygons / swimming textures. Default OFF — it
    // deliberately changes geometry, so the 1x parity gate only means anything with this off.
    val pgxp: Boolean = false,
    // [console]
    val region: String = REGION_AUTO,            // auto | ntsc | pal
    // [bios]
    val biosSearchPath: String = "bios",
    val preferredModel: String = "SCPH-1001",
    val biosOverrideFile: String = "",
    // [runtime]
    val displayScale: Int = 3,                   // 1..6
    val loggingEnabled: Boolean = false,
    val logLevel: Int = 2,                        // 0..5
    val quiet: Boolean = true,
    // Frame pacing. One policy in four keys, all consumed by ArmsxSession::targetFrameRate():
    // frameLimit gates the limiter, speedPercent scales the game's own rate, fpsLimit is an
    // optional absolute ceiling on top, and fastForwardSpeed replaces speedPercent while
    // fast-forward is engaged. Also pushed live through NativeApp.setSpeedLimits().
    val frameLimit: Boolean = true,
    val speedPercent: Int = 100,                 // 10..1000
    val fpsLimit: Int = 0,                       // absolute fps cap, 0 = off
    val fastForwardSpeed: Float = 2f,            // 1.0..16.0, or 0 = uncapped
    // Frame skip. In the same table and pushed by the same call, but NOT a pacing value: the
    // four above decide how fast the machine runs, this decides how many of the frames it
    // produces are put on screen. Emulation, audio and guest timing are identical either way.
    //   0     off (default, matching frontend/config.c — a mismatch here makes the native gate
    //         dead code)
    //   -1    adaptive: drop a present only when the pacer reports the previous frame overran,
    //         never more than two in a row, and never while the target rate is uncapped
    //   1..5  fixed: present one frame, then skip N
    val frameSkip: Int = 0,
    // [audio]
    val audioVolume: Int = 100,                  // 0..200 %
    val audioFastForwardVolume: Int = 100,       // 0..200 %, used while fast-forwarding
    val audioMuted: Boolean = false,
    val audioMuteFastForward: Boolean = false,
    val audioSwapChannels: Boolean = false,
    val audioSkipReverb: Boolean = false,
    val audioBufferMs: Int = 13,                 // 2..100
    val audioDriver: String = AUDIO_OPENSLES,    // opensles | aaudio | default
    // Keep emulating and playing with the app off-screen / the screen off. Default OFF, and the
    // native default in frontend/config.c matches — a mismatch here is how a native gate turns
    // into dead code. On, the lifecycle skips both the pause and the audio-stream suspend.
    val audioBackgroundPlayback: Boolean = false,
    // [paths]
    val expansionRom: String = "",
    val defaultPsxExe: String = "",
    // [video]
    val vsync: Boolean = true,
    // GLES by default, not software: "opengl" binds EGL directly to the Compose Surface's
    // ANativeWindow and removes the full-resolution CPU blit the software present path does.
    // MUST match config.c's cfg->gpu_backend default (2 = opengl) — a Kotlin default that
    // disagrees with the core's is how a setting ends up looking set while the core runs the
    // other path.
    val gpuBackend: String = GPU_OPENGL,         // software | sdl-accelerated | opengl | angle | vulkan
    val textureScaleMode: Boolean = false,       // bilinear filtering
    val debugPanel: Boolean = false,
    val stretchMode: Boolean = false,
    val displayAspect: String = ASPECT_CLASSIC,  // classic | square | wide16x9 | custom
    // Width/height for displayAspect == custom. Out-of-range values are IGNORED by the core
    // rather than clamped — silently presenting a ratio the user did not ask for is exactly
    // how "custom does nothing" gets reported.
    val displayAspectCustom: Float = 16f / 9f,
    // Snap the presented image to a whole multiple of the emulated resolution — every source
    // pixel becomes an exact NxN block, so none is duplicated or dropped and scrolling 2D stops
    // shimmering. Off by default because it deliberately SHRINKS the picture: a 1920x1080 window
    // fits only 4x of a 240-line frame (960 tall), and that is a trade to opt into.
    val integerScaling: Boolean = false,
    val wideUpscale: String = "480p",            // 480p | 720p | 1080p | 1440p | 2160p
    // Rasteriser, NOT presentation — orthogonal to gpuBackend above. `software` is the original
    // native-resolution rasteriser; `hardware` is the scale-aware one that can render above
    // native. Default off: it costs roughly 2x the rasterisation CPU even at 1x, and more as the
    // square of the scale.
    // ON. It is the ONLY thing that makes upscaling possible, so defaulting it off meant the
    // Upscale row silently did nothing until you found a second switch on another screen.
    //
    // It was briefly reverted while the GLES rasteriser had visible seams along polygon edges
    // above 1x; that was a coverage-granularity bug in the fragment shader (the bounding box
    // tested the native pixel index while the edge tests sampled subpixel positions) and is
    // fixed. At 1x this path is pixel-identical to software for essentially no extra CPU.
    //
    // MUST match config.c's cfg->renderer default; a Kotlin default that disagrees with the
    // core's is how a setting ends up looking enabled while the core runs the other path.
    val hwRasterizer: Boolean = true,
    // Internal render resolution multiplier for the hardware rasteriser, 1..8. Ignored entirely
    // while hwRasterizer is false. Deliberately NOT clamped against another range on the way in:
    // an over-eager coerceIn is exactly how a new value gets silently eaten here.
    val internalScale: Int = 1,
    // GPU accuracy flags. Both CHANGE OUTPUT and both default off, matching frontend/config.c.
    // They MUST be mirrored here: Ps1SettingsStore rewrites [video] from an explicit key list,
    // so a native key with no Kotlin field is silently dropped from settings.toml the first
    // time anything is saved — and because these two alter what is drawn, that shows up as
    // rendering that changes between runs for no visible reason.
    // Hardware truth, free for games that never use the features (see config.c) — the
    // canonical dependent is Silent Hill's light/darkness compositing. Kotlin default must
    // match the C default or the serializer silently re-imposes the old value every launch.
    val accurateMaskBit: Boolean = true,
    val accurateDither: Boolean = true,
    // ---- Display / video feature set. Every one defaults to OFF or the neutral mode, and each
    // default MUST equal frontend/config.c's — a Kotlin default that disagrees with the core's
    // turns the native gate into dead code, which is how five settings on this project shipped
    // doing nothing. Ps1SettingsStore rewrites [video] from an explicit key list, so every one
    // of these also needs a serialize line or it is dropped from settings.toml on first save.
    //
    // Widescreen hack: scales the GTE's X projection so 3D geometry fills 16:9 instead of being
    // stretched into it. A hack, not emulation — 2D and UI never go through the GTE.
    val widescreenHack: Boolean = false,
    // Texture filtering in the GLES rasterizer, applied to textured POLYGONS. nearest is what
    // has always shipped and the only mode the 1x parity gate is meaningful under.
    val textureFilter: String = FILTER_NEAREST,   // nearest | bilinear | xbr
    // Box downsampling factor: render at the internal scale, then average NxN back down.
    // 0 = off. The core reduces the request to the largest divisor of the internal scale, so
    // it does nothing at 1x. NOT coerced against a local range here on purpose.
    val downsample: Int = 0,
    // Deinterlacing for 480-line modes. weave is the historical behaviour.
    val deinterlace: String = DEINTERLACE_WEAVE,  // weave | bob | adaptive
    val overscanCrop: String = OVERSCAN_NONE,     // none | small | full
    // Quarter turns are the core's internal form; settings.toml and this field hold DEGREES.
    val displayRotation: Int = 0,                 // 0 | 90 | 180 | 270
    // Widen thin/degenerate polygons games draw as lines so they do not vanish.
    val lineDetect: String = LINE_DETECT_DISABLED, // disabled | quads | basic
    // Texture dumping / replacement (psx/texrep.h). Both default OFF and the native
    // subsystem allocates nothing at all in that state — it is the hottest path in the
    // emulator, so "off" has to mean one never-taken branch, not a disabled feature.
    // Dumps are written to <textureDir>/dump, packs are read from <textureDir>/replacements.
    val textureDump: Boolean = false,
    val textureReplacements: Boolean = false,
    // BASE folder for both. Empty means "derive it": Ps1Textures.push() fills in a per-game
    // path under the app's data root, because the core does not know the disc serial.
    val textureDir: String = "",
    // [input]
    // Which mode the emulated pad reports when a game boots.
    //
    // Default OFF = digital, matching real hardware. This was briefly defaulted ON for
    // discoverability (the DualShock's ANALOG button has no standard Android keycode, so
    // sticks do nothing until the user finds the binding) and that turned out to break
    // input outright in pre-DualShock games: Crash Bandicoot (1996) predates the pad and
    // mis-handles one that reports model 0x73 at boot, leaving BOTH the touch overlay and
    // a physical pad dead. Discoverability is not worth breaking 1996-era titles — turn it
    // on per taste, and the live toggle (pad code 200 / the "Analog (toggle)" bind) still
    // works either way.
    val analogModeDefault: Boolean = false,
    // Multitap in controller port 1 — four players share one port (psx/input/multitap.c).
    // Default OFF, and not just out of caution: a tap answers the pad poll with its OWN id
    // (0x80) instead of a controller's, so a game that does not understand multitaps reports
    // NO CONTROLLER AT ALL. Real hardware has exactly that problem, which is why a real
    // SCPH-1070 carries a physical 1-player/multitap switch. Live: NativeApp.setMultitapEnabled
    // swaps the device in the port while a game runs; this field is what a relaunch reads.
    val multitap: Boolean = false,
    // [emulation]
    // Rewind: a ring of save states taken every Nth frame, stepped backwards on demand.
    //
    // rewindSeconds x rewindFrequency is a MEMORY figure, not just a duration: one PS1
    // snapshot is ~3.5 MB, so the default 10 s at 2/s is ~70 MB and 60 s at 4/s would be
    // ~840 MB. Nothing at all is allocated while this is false — see psx/rewind.h, which also
    // owns the 1..60 / 1..8 ranges. Default OFF for that reason alone.
    val rewind: Boolean = false,
    val rewindSeconds: Int = 10,
    val rewindFrequency: Int = 2,
    // Runahead: re-simulate N frames every frame so the picture reacts to a press N frames
    // sooner. 0 = off and is the default. This is a CPU cost, not a memory one, and it is paid
    // on every single frame: one state save, one state load, and N extra emulated frames. On a
    // device that is already at 100% speed it does not hide latency, it cuts the frame rate.
    val runahead: Int = 0,
    // [cheats]
    // GameShark codes applied to R3000A memory once per emulated frame (psx/cheats.c). NOT the
    // PS2 patch machinery: PNACH is EE-address-shaped and keyed by <SERIAL>_<CRC>, and this
    // machine has neither. Every published PS1 cheat is `800AB3C4 0063`, so that is the format
    // the core reads, out of a `.cht` catalogue.
    //
    // These three are PER-GAME in practice and that is the whole reason enablement lives here
    // rather than in a store of its own: [Ps1SettingsStore]'s per-game layer already exists, has
    // exactly one merge point, and is the only place the app records "this game differs".
    //
    // `cheatsEnabled` is the master switch; `cheatsFile` is an absolute path to the catalogue
    // (empty = this game has none, which is the default and costs the core nothing);
    // `cheatsEnabledCodes` names which of its entries are armed.
    //
    // Global defaults are deliberately inert. A cheat list that could win at the GLOBAL tier is
    // how the sibling PS2 project armed a name like "Infinite Health" in every game the user
    // owned — it shipped a one-time repair function for exactly that. Here the names are only
    // ever matched against the ONE file named by `cheatsFile`, which is itself per-game, so a
    // stray global list can arm nothing.
    val cheatsEnabled: Boolean = false,
    val cheatsFile: String = "",
    val cheatsEnabledCodes: List<String> = emptyList(),
    // [library]
    val folders: List<String> = emptyList(),
    val recursiveFolders: List<String> = emptyList(),
) {
    companion object {
        const val CPU_CACHED = "cached"
        const val CPU_INTERPRETER = "interpreter"
        val CPU_ENGINES = listOf(CPU_CACHED, CPU_INTERPRETER)

        const val REGION_AUTO = "auto"
        val REGIONS = listOf(REGION_AUTO, "ntsc", "pal")

        const val GPU_SOFTWARE = "software"
        const val GPU_ACCELERATED = "sdl-accelerated"
        // GPU present backends (frontend/render_gl.cpp / render_vk.cpp). On Android "opengl"
        // binds EGL directly to the Compose Surface's ANativeWindow, which removes the
        // full-resolution CPU blit the software path does. Vulkan is EXPERIMENTAL.
        const val GPU_OPENGL = "opengl"
        /** ANGLE is Google's GLES-on-Vulkan translator, bundled as libEGL_angle.so +
         *  libGLESv2_angle.so. It is an IMPLEMENTATION of the OpenGL backend, not a peer of
         *  Vulkan — selecting it runs the same GLES 3.0 present path against ANGLE's EGL
         *  instead of the system driver. It is a top-level token here purely so the renderer
         *  list stays one flat choice; frontend/config.c maps it to gpu_backend=opengl plus
         *  gl_driver=angle. */
        const val GPU_ANGLE = "angle"
        const val GPU_VULKAN = "vulkan"
        /** Single source of truth for BOTH renderer pickers (full Settings → Video, and the
         *  in-game pause menu → Display). Same set, same order, same labels in both — they
         *  used to disagree, and the in-game one was a lifted PCSX2 control that never
         *  reached this core at all. */
        val GPU_BACKENDS = listOf(GPU_SOFTWARE, GPU_ACCELERATED, GPU_OPENGL, GPU_ANGLE, GPU_VULKAN)
        /** Localized. A `get()` rather than a stored list so a live language switch re-reads it —
         *  a `val` would freeze whatever language was current when this class first loaded. */
        val GPU_BACKEND_LABELS: List<String>
            get() = listOf(
                I18n.get("renderer.gpuBackend.software"),
                I18n.get("renderer.gpuBackend.sdlAccel"),
                I18n.get("renderer.gpuBackend.systemGl"),
                I18n.get("renderer.gpuBackend.angle"),
                I18n.get("renderer.gpuBackend.vulkan"),
            )

        const val ASPECT_CLASSIC = "classic"
        const val ASPECT_CUSTOM = "custom"
        val ASPECTS = listOf(ASPECT_CLASSIC, "square", "wide16x9", ASPECT_CUSTOM)
        val ASPECT_LABELS: List<String>
            get() = listOf(
                I18n.get("renderer.aspect.classic"),
                I18n.get("renderer.aspect.square"),
                I18n.get("renderer.aspect.wide"),
                I18n.get("common.custom"),
            )

        /** Bounds the core accepts for a custom ratio (frontend/config.c). */
        const val ASPECT_CUSTOM_MIN = 0.5f
        const val ASPECT_CUSTOM_MAX = 4.0f

        /** Handy presets for the custom row; the value stays freely adjustable between them. */
        val ASPECT_CUSTOM_PRESETS = listOf(
            "4:3" to 4f / 3f,
            "3:2" to 3f / 2f,
            "16:10" to 16f / 10f,
            "16:9" to 16f / 9f,
            "18:9" to 2f,
            "21:9" to 64f / 27f,
        )

        val WIDE_UPSCALES = listOf("480p", "720p", "1080p", "1440p", "2160p")

        // ---- Display / video feature set. Tokens are the ones frontend/config.c parses;
        // an unrecognised token there falls back to the default, so these must match exactly.
        const val FILTER_NEAREST = "nearest"
        val TEXTURE_FILTERS = listOf(FILTER_NEAREST, "bilinear", "xbr")
        val TEXTURE_FILTER_LABELS: List<String>
            get() = listOf(
                I18n.get("renderer.textureFilter.nearest"),
                I18n.get("renderer.textureFilter.bilinear"),
                I18n.get("renderer.textureFilter.xbr"),
            )

        const val DEINTERLACE_WEAVE = "weave"
        val DEINTERLACES = listOf(DEINTERLACE_WEAVE, "bob", "adaptive")
        val DEINTERLACE_LABELS: List<String>
            get() = listOf(
                I18n.get("renderer.deinterlace.weave"),
                I18n.get("renderer.deinterlace.bob"),
                I18n.get("renderer.deinterlace.adaptive"),
            )

        const val OVERSCAN_NONE = "none"
        val OVERSCAN_CROPS = listOf(OVERSCAN_NONE, "small", "full")
        val OVERSCAN_LABELS: List<String>
            get() = listOf(
                I18n.get("common.none"),
                I18n.get("renderer.overscan.small"),
                I18n.get("renderer.overscan.full"),
            )

        const val LINE_DETECT_DISABLED = "disabled"
        val LINE_DETECTS = listOf(LINE_DETECT_DISABLED, "quads", "basic")
        val LINE_DETECT_LABELS: List<String>
            get() = listOf(
                I18n.get("common.off"),
                I18n.get("renderer.lineDetect.quads"),
                I18n.get("renderer.lineDetect.basic"),
            )

        /** Rotation in degrees clockwise, in the order the UI shows them. */
        val DISPLAY_ROTATIONS = listOf(0, 90, 180, 270)
        val DISPLAY_ROTATION_LABELS = listOf("0°", "90°", "180°", "270°")

        /** Box downsampling factors offered by the UI; 0 = off. The core clamps the request
         *  down to a divisor of the internal scale, so an unusable pick degrades to off
         *  rather than being rejected. */
        val DOWNSAMPLE_FACTORS = listOf(0, 2, 3, 4)

        fun downsampleLabel(factor: Int): String =
            if (factor < 2) I18n.get("common.off") else "1/$factor"

        /** Index helpers so a value written by hand into settings.toml that this build does
         *  not know about shows as the default instead of crashing a picker. */
        fun indexOfOrZero(options: List<String>, value: String): Int =
            options.indexOf(value).coerceAtLeast(0)

        /** Rewind buffer lengths offered by the UI, in seconds. The native side owns the
         *  1..60 range (psx/rewind.h) — do NOT coerceIn against this list. */
        val REWIND_SECONDS = listOf(5, 10, 15, 30, 60)

        /** Snapshots per second. More = finer-grained rewind and proportionally more memory. */
        val REWIND_FREQUENCIES = listOf(1, 2, 4)

        /** Runahead frame counts, 0 = off. The native side owns the 0..5 range. */
        val RUNAHEAD_FRAMES = listOf(0, 1, 2, 3, 4, 5)

        /** Bytes one rewind snapshot takes when the core has not measured one yet. Mirrors
         *  PSX_REWIND_ESTIMATED_SNAPSHOT_BYTES; the real figure comes from
         *  NativeApp.rewindSnapshotBytes() as soon as a game has run. */
        const val REWIND_ESTIMATED_SNAPSHOT_BYTES = 3_670_016L

        /** Hard ceiling the core puts on the whole ring, whatever the settings ask for; past
         *  it the DEPTH shrinks rather than the allocation failing. MUST match
         *  PSX_REWIND_MAX_BYTES in psx/rewind.h — a label that promises more history than the
         *  core will keep is worse than no label. */
        const val REWIND_MAX_BUFFER_BYTES = 384L * 1024L * 1024L

        /** "~70 MB" for a seconds/frequency pair, using the core's own snapshot size when it
         *  has one. This is the number the rewind rows put in front of the user — a buffer
         *  length that does not say what it costs is how a handheld runs out of memory.
         *  Capped at [REWIND_MAX_BUFFER_BYTES], because that is what the core will really
         *  hold. */
        fun rewindBufferLabel(seconds: Int, frequency: Int, snapshotBytes: Long): String {
            val bytes = (seconds.toLong() * frequency.toLong() *
                (if (snapshotBytes > 0) snapshotBytes else REWIND_ESTIMATED_SNAPSHOT_BYTES))
                .coerceAtMost(REWIND_MAX_BUFFER_BYTES)
            val mb = bytes / (1024L * 1024L)
            return if (mb >= 1024) String.format(java.util.Locale.US, "%.1f GB", mb / 1024f)
            else "$mb MB"
        }

        /** Seconds of history the memory budget really allows for a given frequency — a
         *  60 s buffer at 4/s asks for ~840 MB and only ~110 s worth fits, so the row has to
         *  say so instead of quietly keeping less than it offered. */
        fun rewindEffectiveSeconds(seconds: Int, frequency: Int, snapshotBytes: Long): Int {
            val each = if (snapshotBytes > 0) snapshotBytes else REWIND_ESTIMATED_SNAPSHOT_BYTES
            val affordable = (REWIND_MAX_BUFFER_BYTES / each / frequency.coerceAtLeast(1)).toInt()
            return minOf(seconds, affordable.coerceAtLeast(1))
        }

        val DISPLAY_SCALES = (1..6).toList()
        val LOG_LEVELS = (0..5).toList()

        // Android SDL audio backends. openslES is the floor and the default: it is the only
        // one that stays entirely inside the native API, so it needs nothing from
        // org.libsdl.app.SDLActivity (which does not exist — Compose owns the Activity).
        // AAudio is reachable only because frontend/android_jni.cpp now hands SDLAudioManager
        // a Context; the core silently falls back to openslES if that did not land.
        const val AUDIO_OPENSLES = "opensles"
        const val AUDIO_AAUDIO = "aaudio"
        val AUDIO_DRIVERS = listOf(AUDIO_OPENSLES, AUDIO_AAUDIO)

        /** Device buffer sizes offered by the Audio tab, in ms. 13 ms is the core's historical
         *  588-frame buffer and stays the default. */
        val AUDIO_BUFFER_MS = listOf(5, 8, 13, 20, 30, 50, 80)

        /** Fast-forward multipliers, in the order the UI shows them. 0 = uncapped. */
        val FAST_FORWARD_SPEEDS = listOf(1.5f, 2f, 3f, 4f, 0f)

        fun fastForwardSpeedLabel(speed: Float): String = when {
            speed <= 0f -> I18n.get("common.unlimited")
            speed == speed.toInt().toFloat() -> "${speed.toInt()}×"
            else -> "${speed}×"
        }

        /** Emulation-speed percentages offered by the UI. */
        val SPEED_PERCENTS = listOf(25, 50, 75, 90, 100, 110, 125, 150, 200)

        /** Absolute frame-rate caps offered by the UI; 0 = off. */
        val FPS_LIMITS = listOf(0, 20, 30, 45, 50, 60, 75, 90, 120)

        /** Frame-skip modes, in the order the UI shows them. Auto sits next to Off because it is
         *  the one most people want: it costs nothing until the device actually falls behind.
         *  The native side owns the -1..5 range — do NOT coerceIn against this list. */
        val FRAME_SKIPS = listOf(0, -1, 1, 2, 3, 4, 5)

        fun frameSkipLabel(skip: Int): String = when {
            skip < 0 -> I18n.get("common.auto")
            skip == 0 -> I18n.get("common.off")
            else -> "1/${skip + 1}"
        }

        /** Console models the native BIOS auto-selector understands (from config.c g_models_text),
         *  in the `SCPH-XXXX` form config's `preferred_model` expects. */
        val MODELS = listOf(
            "SCPH-1000", "SCPH-1001", "SCPH-1002",
            "SCPH-3000", "SCPH-3500",
            "SCPH-5000", "SCPH-5500", "SCPH-5501", "SCPH-5502", "SCPH-5552",
            "SCPH-7000", "SCPH-7001", "SCPH-7002", "SCPH-7003", "SCPH-7501", "SCPH-7502",
            "SCPH-9002",
            "SCPH-100", "SCPH-101", "SCPH-102A", "SCPH-102B", "SCPH-102C",
        )
    }
}
