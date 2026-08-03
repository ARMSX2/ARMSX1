// SPDX-License-Identifier: GPL-3.0+
package com.armsx2.i18n

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.armsx2.runtime.MainActivityRuntime
import org.json.JSONObject
import androidx.core.content.edit

/**
 * Live UI localization.
 *
 * Android's res/values-xx system can't switch language without recreating the Activity, which is
 * no good for a live picker. So we roll our own: English is the source of truth (the [EN] map,
 * baked in code); every other language is a flat key→text JSON in assets/i18n/<code>.json,
 * lazy-loaded on first use. The current language is a Compose state read by [ProvideStrings], so
 * flipping it recomposes the whole UI subtree — a true live switch, no restart.
 *
 * Reads fall back English-ward: a missing key or a missing translation shows the English string
 * (never a raw key), so a half-translated locale degrades gracefully instead of showing "tab.audio".
 *
 * All non-English JSONs are AI-TRANSLATED — a full per-language localization pass (Fable 5),
 * replacing the original Google-MT batch; users can report bad strings and we patch the JSON.
 * Adding a language = drop one JSON in assets/i18n + one row in [languages]. Nothing else changes.
 */
data class AppLanguage(
    val code: String,
    val englishName: String,
    val nativeName: String,
    val rtl: Boolean = false,
)

object I18n {
    const val SYSTEM_CODE = "system"

    /** System default first, then English (source of truth) and translations. */
    val languages: List<AppLanguage> = listOf(
        AppLanguage(SYSTEM_CODE, "System", "System"),
        AppLanguage("en", "English", "English"),
        AppLanguage("es", "Spanish", "Español"),
        AppLanguage("pt-BR", "Portuguese (Brazil)", "Português (Brasil)"),
        AppLanguage("fr", "French", "Français"),
        AppLanguage("de", "German", "Deutsch"),
        AppLanguage("it", "Italian", "Italiano"),
        AppLanguage("ru", "Russian", "Русский"),
        AppLanguage("uk", "Ukrainian", "Українська"),
        AppLanguage("ku", "Kurdish", "Kurdî"),
        AppLanguage("pl", "Polish", "Polski"),
        AppLanguage("tr", "Turkish", "Türkçe"),
        AppLanguage("ja", "Japanese", "日本語"),
        AppLanguage("ko", "Korean", "한국어"),
        AppLanguage("zh-CN", "Chinese (Simplified)", "简体中文"),
        AppLanguage("zh-TW", "Chinese (Traditional)", "繁體中文"),
        AppLanguage("ar", "Arabic", "العربية", rtl = true),
        AppLanguage("id", "Indonesian", "Indonesia"),
        AppLanguage("vi", "Vietnamese", "Tiếng Việt"),
        AppLanguage("th", "Thai", "ไทย"),
        AppLanguage("fa", "Persian", "فارسی", rtl = true),
    )

    private const val PREF_KEY = "ui.language"

    /** Current language code, as Compose state — mutating it drives live recomposition. */
    var current by mutableStateOf("en")
        private set

    /** Picker selection. `system` resolves to an actual [current] language. */
    var selected by mutableStateOf(SYSTEM_CODE)
        private set

    /** code → (key → translated text). English is never stored here (it lives in [EN]). */
    private val tables = HashMap<String, Map<String, String>>()

    /** Call once at startup (after MainActivityRuntime.prefs is ready). Restores the saved language. */
    fun init(context: Context) {
        val saved = runCatching { MainActivityRuntime.prefs.getString(PREF_KEY, null) }.getOrNull()
        val selection = if (saved != null && languages.any { it.code == saved }) saved else SYSTEM_CODE
        applySelection(context, selection)
    }

    /** Switch language live + persist. Loads the JSON if not already cached. */
    fun setLanguage(context: Context, code: String) {
        if (languages.none { it.code == code }) return
        applySelection(context, code)
        runCatching { MainActivityRuntime.prefs.edit { putString(PREF_KEY, code) } }
    }

    /** Re-resolve only when the picker is following the device language. */
    fun refreshSystemLanguage(context: Context) {
        if (selected == SYSTEM_CODE) applySelection(context, SYSTEM_CODE)
    }

    private fun applySelection(context: Context, selection: String) {
        val code = if (selection == SYSTEM_CODE) resolveSystemLanguage(context) else selection
        if (code != "en") ensureLoaded(context, code)
        selected = selection
        current = code
    }

    private fun resolveSystemLanguage(context: Context): String {
        val locale = context.resources.configuration.locales[0] ?: return "en"
        val supported = languages.filterNot { it.code == SYSTEM_CODE }
        supported.firstOrNull { it.code.equals(locale.toLanguageTag(), ignoreCase = true) }?.let { return it.code }

        if (locale.language.equals("zh", ignoreCase = true)) {
            val traditional = locale.script.equals("Hant", ignoreCase = true) ||
                locale.country.uppercase() in setOf("TW", "HK", "MO")
            return if (traditional) "zh-TW" else "zh-CN"
        }
        if (locale.language.equals("pt", ignoreCase = true)) return "pt-BR"

        return supported.firstOrNull {
            it.code.substringBefore('-').equals(locale.language, ignoreCase = true)
        }?.code ?: "en"
    }

    private fun ensureLoaded(context: Context, code: String) {
        if (code == "en" || tables.containsKey(code)) return
        val map = runCatching {
            val text = context.assets.open("i18n/$code.json").bufferedReader().use { it.readText() }
            val obj = JSONObject(text)
            val out = HashMap<String, String>(obj.length())
            val it = obj.keys()
            while (it.hasNext()) { val k = it.next(); out[k] = obj.getString(k) }
            out as Map<String, String>
        }.getOrDefault(emptyMap())
        tables[code] = map
    }

    /** Look up a key. Falls back to the English string, then to the key itself. */
    fun get(key: String): String {
        if (current != "en") tables[current]?.get(key)?.let { if (it.isNotEmpty()) return it }
        return EN[key] ?: key
    }

    val isRtl: Boolean get() = languages.firstOrNull { it.code == current }?.rtl == true
}

/**
 * Composable string lookup: `Text(str("tab.audio"))`.
 *
 * Reading [I18n.current] (a snapshot State) subscribes THIS composition to language changes, so
 * [I18n.setLanguage] live-recomposes every str() call site — no CompositionLocal/provider wiring
 * anywhere. Non-composable call sites (Toasts, dialogs built outside composition) use
 * [I18n.get] directly instead.
 */
@Composable
fun str(key: String): String {
    I18n.current // subscribe: any language switch recomposes this call site
    return I18n.get(key)
}

/**
 * English source of truth. This is the canonical key set; every assets/i18n/<code>.json mirrors
 * these keys. SEEDED with a pilot set (settings tabs + common actions + the App/Language tab); the
 * full ~400–500 keys land during the file-by-file extraction pass. Keep keys dotted + stable —
 * renaming a key orphans it in every translation JSON.
 */
val EN: Map<String, String> = mapOf(
    "driver.unavailable" to "Custom GPU drivers require a Vulkan renderer, which this emulator does not use.",
    "about.title" to "About app",
    "friends.title" to "Friends",
    "friends.explain" to "Link Discord to show what you're playing and see which friends are in ARMSX1. Uses your existing Discord friends — ARMSX1 keeps no account and runs no server. Only while the app is open.",
    "friends.connect" to "Connect Discord",
    "friends.connecting" to "Connecting to Discord…",
    // Separate from the line above on purpose: "connecting" while the user is actually meant to be
    // finishing a sign-in in their browser tells them nothing about what is waiting on them.
    "friends.authorizing" to "Waiting for Discord sign-in in your browser…",
    "friends.cancel" to "Cancel",
    "friends.cancelled" to "Sign-in cancelled. Tap Connect Discord to try again.",
    "friends.error.helper" to
        "The Discord helper didn't start. Close ARMSX1 completely and reopen it, then try again.",
    "friends.error.authorize" to
        "Discord sign-in didn't finish — the browser was closed, or Discord never came back to " +
        "ARMSX1. Tap Connect Discord to try again.",
    "friends.error.connect" to
        "Couldn't reach Discord. Check your internet connection, then tap Connect Discord to try again.",
    "friends.connected" to "Discord connected",
    "friends.disconnect" to "Disconnect",
    "friends.playingNow" to "In ARMSX1 now",
    "friends.nobody" to "None of your Discord friends are in ARMSX1 right now.",
    "friends.notify" to "Notify me in game",
    "friends.notify.desc" to "Show a message over the game when a friend starts playing ARMSX1.",
    "friends.inLibrary" to "In the library",
    "friends.playing" to "In a game",
    "friends.nowOnline" to "is now online",
    "friends.unavailable" to "This build was made without the Discord SDK.",
    "news.title" to "What's new",
    "news.refresh" to "Refresh",
    "news.loading" to "Loading release notes…",
    "news.unavailable" to "Release notes are unavailable right now.",
    "news.offline" to "Showing saved notes — couldn't reach GitHub.",
    "news.contributors" to "Contributors",
    "news.prerelease" to "Pre-release",
    "news.noNotes" to "No notes for this release.",
    "news.showMore" to "Show more",
    "news.showLess" to "Show less",
    "about.tagline" to "Fast, modern PlayStation emulation for Android.",
    "about.appVersion" to "App version",
    "about.coreVersion" to "Emulator version",
    "about.device" to "Device",
    "about.androidVersion" to "Android version",
    "about.repository.title" to "GitHub repository",
    "about.repository.description" to "Open the source code, releases, and issue tracker.",
    "about.build.title" to "Build information",
    "about.hardware.title" to "Device information",
    "about.soc" to "Chipset",
    "about.gpu" to "GPU",
    "about.cpuCores" to "CPU cores",
    "about.memory" to "Memory",
    "about.display" to "Display",
    "about.architecture" to "Architecture",
    "about.pageSize" to "Memory page",
    // --- drawer About section: external links ---
    "about.section.header" to "About",
    "about.discord" to "Discord",
    "about.github" to "GitHub",
    "about.website" to "Website",
    // Shown when NOTHING on the device claims the link (no browser, no target app) — a silent
    // no-op would leave the user tapping a dead row forever.
    "about.openFailed" to "No app available to open this link.",
    // --- settings tabs ---
    "tab.app" to "App",
    "tab.info" to "Info",
    "info.noGame.title" to "No game selected",
    "info.noGame.body" to "Open this from a game's settings to see its details.",
    "info.title" to "Name",
    "info.serial" to "Serial",
    "info.crc" to "CRC",
    "info.customName.label" to "Custom name",
    "info.customName.set" to "Set custom name",
    "info.customName.change" to "Change name",
    "info.customName.clear" to "Reset name",
    "info.customName.placeholder" to "Game name…",
    "info.cover.label" to "Custom cover art",
    "info.setCover" to "Set cover",
    "info.changeCover" to "Change cover",
    "info.removeCover" to "Remove cover",
    "info.exportSettings" to "Export settings",
    "info.importSettings" to "Import settings",
    "info.region" to "Region",
    "info.container" to "Container",
    "info.platform" to "Platform",
    "info.compatibility" to "Compatibility",
    "info.playTime" to "Play time",
    "info.lastPlayed" to "Last played",
    "info.path" to "Path",
    "tab.performance" to "Performance",
    "tab.renderer" to "Renderer",
    "tab.fixes" to "Advanced",
    "tab.audio" to "Audio",
    "tab.controls" to "Controls",
    "tab.hotkeys" to "Hotkeys",
    "tab.overlay" to "On-Screen",
    "tab.patches" to "Patches",
    "tab.skins" to "Skins",
    // --- App tab / language ---
    "app.libraryMusic" to "Library Music",
    "app.libraryMusic.volume" to "Library Music Volume",
    "app.libraryMusic.choose" to "Choose Music File…",
    "app.libraryMusic.reset" to "Reset to Default",
    "app.libraryMusic.current" to "Playing: %s",
    "app.libraryMusic.default" to "Playing the built-in track.",
    "app.libraryMusic.desc" to "Play ambient music on the game library screen, like a console dashboard. Stops automatically when a game starts, and stays quiet if something else is already playing audio.",
    "app.pauseMusic" to "In-Game Pause Music",
    "app.pauseMusic.volume" to "Pause Music Volume",
    "app.pauseMusic.choose" to "Choose Music File…",
    "app.pauseMusic.reset" to "Reset to Default",
    "app.pauseMusic.current" to "Playing: %s",
    "app.pauseMusic.default" to "Playing the built-in track.",
    "app.pauseMusic.desc" to "Play music while the in-game pause menu is open, including the settings, achievement and memory card screens. Stops the moment the game resumes, and stays quiet if something else is already playing audio.",
    "app.menuSfx" to "Menu Sound Effects",
    "app.menuSfx.desc" to "Sound effects for menu actions — select, back, menu open, toggles, sliders. Comes with a built-in set; import a folder of named clips below to use your own instead.",
    "app.menuSfx.volume" to "Sound Effects Volume",
    "app.menuSfx.choose" to "Import Sound Pack…",
    "app.menuSfx.reset" to "Use Built-in",
    "app.menuSfx.current" to "Sound pack: %s",
    "app.menuSfx.none" to "Using the built-in sound set.",
    "app.menuSfx.hint" to "Pick a folder with clips named nav, select, submenu, menu, back, toggle_on, toggle_off, reset, slider, sleep, wake (any of .ogg / .wav / .mp3). Keep them short — a fraction of a second.",
    "app.welcomeBack" to "Welcome Back!",
    "app.menuSfx.imported" to "Imported %d sound effect(s).",
    "app.menuSfx.importNone" to "No matching files. Name them select, back, menu, toggle_on, toggle_off, reset, or slider.",
    "app.credits.music" to "Music: \"Calm Ambient 1 (Synthwave 4k)\" — The Cynic Project / cynicmusic.com / pixelsphere.org (CC0)",
    "app.credits.sfx" to "Menu sounds: \"Interface SFX Pack 1\" — obsydianx.itch.io (CC0)",
    "app.language" to "Language",
    "app.language.desc" to "Choose the app language. Applies instantly.",
    "app.language.system" to "System language",
    "app.language.machineNote" to "Translations except English are AI-translated. Spot a bad one? Let us know and we'll fix it.",
    "app.library.search" to "Library search",
    "app.library.search.desc" to "Show the game search field on the library home screen.",
    "app.library.recents" to "Recently played games",
    "app.library.recents.desc" to "Show the Recently Played section on the library home screen.",
    "app.library.gridNames.desc" to "Show each game's name under its cover in grid view.",
    "app.library.opacity" to "Library opacity",
    "ra.website.open" to "Create an account or browse sets on retroachievements.org \u2197",
    "ra.library.header" to "Library progress",
    "ra.library.desc" to "Show achievement progress on every game in your library, including ones you have never played.",
    // %s is replaced by ra.library.keyHelp.link, rendered as a tappable link. Keep the placeholder.
    "ra.library.keyHelp" to "While authenticated, visit %s on the RetroAchievements website. Find the \"Keys\" section on the page. Copy the web API key value and input it to sync your library.",
    "ra.library.keyHelp.link" to "your control panel",
    "ra.library.apiKey" to "Web API key",
    "ra.library.sync" to "Sync library",
    "ra.library.syncing" to "Syncing…",
    "ra.library.notReady" to "Sign in and scan your library first.",
    "app.backup.export" to "Back up app data",
    "app.backup.export.desc" to "Save states, memory cards, artwork, per-game settings, controller profiles, patches and all settings into one .zip. Games and BIOS are not included.",
    "app.backup.import" to "Restore app data",
    "app.backup.import.desc" to "Load a backup .zip. Files with the same name are replaced, and the app restarts.",
    "app.covers" to "Download cover art",
    "app.covers.desc" to "Fetch box art for every game in your library and keep it on disk, so covers show up straight away and work offline.",
    "app.covers.done" to "Downloaded %d covers",
    "app.covers.upToDate" to "All covers already downloaded",
    "app.covers.none" to "No games found to fetch covers for",
    "app.reset" to "Reset app",
    "app.reset.desc" to "Restore every setting to its default. Your games and saves are kept.",
    "app.reset.title" to "Reset the whole app?",
    "app.reset.message" to "Careful — this resets EVERY setting to default: graphics, controls, hotkeys, touch layouts, per-game settings and patches. Setup will run again.\n\nYour games, BIOS, saves, memory cards, save states and texture packs are NOT deleted. Export a backup first if you want your settings back.\n\nContinue?",
    "app.reset.confirm" to "Reset everything",
    "app.backup.working" to "Working…",
    "app.backup.exported" to "Backup saved — %s.",
    "app.backup.imported" to "Restored %s. Restarting…",
    "app.backup.failed" to "Backup failed: %s",
    "app.clearCache" to "Clear cached data",
    "app.clearCache.desc" to "Delete compiled shader caches and cover-art thumbnails. They rebuild automatically.",
    "app.clearCache.done" to "Cleared %s of cached data.",
    "app.clearCache.doneSmall" to "Cleared cached data.",
    "app.clearCache.empty" to "Cache is already empty.",
    "app.theme" to "Theme",
    "update.title" to "App updates",
    "update.currentVersion" to "Installed",
    "update.check" to "Check for updates",
    "update.checking" to "Checking…",
    "update.upToDate" to "You’re on the latest version.",
    "update.onNightly" to "You’re on a nightly build — updates come through the nightly channel.",
    "update.available" to "Update available:",
    "update.install" to "Download & install",
    "update.notesUnavailable" to "No release notes.",
    "update.downloading" to "Downloading…",
    "update.checkFailed" to "Couldn’t check for updates",
    "update.downloadFailed" to "Download failed",
    "update.checkOnLaunch" to "Check on launch",
    "update.checkOnLaunch.desc" to "Automatically check GitHub for a new version each time ARMSX1 starts.",
    "update.includeNightly" to "Include nightly versions",
    "update.includeNightly.desc" to "Also offer nightly (pre-release) builds, not just stable releases. Off by default. A nightly build can’t downgrade back to a stable release in-app — reinstall a stable APK manually if you want to switch back.",
    "update.later" to "Later",
    "app.theme.system" to "System",
    "app.theme.materialyou" to "Material You",
    "app.theme.rgb" to "RGB",
    "app.theme.custom" to "Custom",
    "app.theme.custom.r" to "Red",
    "app.theme.custom.g" to "Green",
    "app.theme.custom.b" to "Blue",
    "app.bgColor" to "Background Color",
    "app.bgColor.desc" to "Recolor the animated library background. The white waves ride over your color; the theme accent above stays separate. Updates live as you drag. Applies to both the 3D GL wave and the simpler 2D wave below.",
    "app.bg.simple" to "Simple animated background",
    "app.bg.simple.desc" to "Use the lightweight 2D animated wave background instead of the 3D GL one \u2014 the same backdrop devices that can't run the 3D wave already use. Lighter on the GPU, and recolorable with the options above. No effect if you've set a custom background image.",
    "app.bgColor.rgb" to "RGB Cycle",
    "app.bgColor.rgb.desc" to "Continuously drift the background through the colour spectrum, like RGB peripherals. Overrides the fixed colour below. Same limitation as above: no effect where the fallback background is in use.",
    "app.theme.light" to "Light",
    // "Dark" renamed to "Blue" — it was always the blue-tinted dark theme, which only became
    // confusing once the other hues existed. Keys are the lowercased ThemeMode names.
    "app.theme.blue" to "Blue",
    "app.theme.purple" to "Purple",
    "app.theme.pink" to "Pink",
    "app.theme.red" to "Red",
    "app.theme.orange" to "Orange",
    "app.theme.green" to "Green",
    "app.theme.teal" to "Teal",
    "app.theme.cyan" to "Cyan",
    "app.theme.black" to "Black",
    "app.theme.oled" to "OLED",
    "app.theme.oledBase" to "OLED black",
    "app.keyboard.systemIme" to "Use system keyboard",
    "app.keyboard.systemIme.desc" to "Type with your phone's own keyboard instead of the built-in one when searching or renaming games. The built-in keyboard is recommended if you play with a controller, because it can be navigated with the D-pad.",
    "app.bootLogo" to "Boot animation",
    "app.bootLogo.desc" to "Play the ARMSX1 intro video when the app starts.",
    "app.toolbarPosition" to "Library toolbar",
    "app.toolbarPosition.top" to "Top",
    "app.toolbarPosition.bottom" to "Bottom",
    "app.launcherRotation" to "Screen rotation for the launcher",
    "app.launcherRotation.device" to "Device",
    "app.launcherRotation.landscape" to "Landscape",
    "app.launcherRotation.portrait" to "Portrait",
    "app.launcherRotation.auto" to "Auto",
    "app.launcherRotation.desc" to "Rotation for the launcher/library only. Games use the rotation on the Renderer settings page.",
    // --- common actions ---
    "action.play" to "Play",
    "action.resume" to "Resume",
    "action.fastForward" to "Fast-Forward",
    "action.fastForward.detail" to "Resume at max speed",
    "action.fastForward.on" to "On — tap to turn off",
    "action.settings" to "Settings",
    "action.allSettings" to "All Settings",
    "action.back" to "Back",
    "action.cancel" to "Cancel",
    "action.ok" to "OK",
    "action.save" to "Save",
    "action.edit" to "Edit",
    "patches.editor.new" to "New patch file",
    "patches.editor.paste" to "Paste",
    "patches.editor.placeholder" to "patch=1,EE,00000000,extended,00000000",
    "action.delete" to "Delete",
    "action.reset" to "Reset",
    "action.apply" to "Apply",
    "action.import" to "Import",
    "action.importFolder" to "Import Folder",
    "action.export" to "Export",
    "action.search" to "Search",
    "action.close" to "Close",
    "action.scrollTop" to "Scroll to top",
    "action.swapDisc" to "Swap Disc",
    "action.swapDisc.detail" to "Change disc without closing",
    "action.confirm" to "Confirm",
    // --- scope toggle (global / per-game) ---
    "scope.global" to "Global",
    "scope.game" to "Game",
    "settings.scope.label" to "Configuring",
    // --- Audio tab ---
    "audio.header.description" to "PS1 audio output. Volume/mute apply instantly; the rest reapply the moment you change them.",
    "audio.volume.label" to "Volume",
    "audio.volume.description" to "Above 100% boosts/amplifies SPU2 output — handy for quiet games, but very high levels can distort/clip.",
    "audio.mute.label" to "Mute",
    "audio.synchronization.label" to "Audio Synchronization",
    "audio.synchronization.description" to "Time Stretch — keeps audio pitch stable when emulation speed varies. Off = a bit less CPU, but pitch/clock can drift under load.",
    "audio.buffer.label" to "Audio Buffer",
    "audio.buffer.description" to "Bigger buffer = fewer crackles/dropouts but more latency. Raise this if audio stutters on low-end devices.",
    "audio.outputLatency.label" to "Output Latency",
    "audio.outputLatency.description" to "Target output latency. Lower is more responsive; higher is safer against dropouts.",
    "audio.fastForwardVolume.label" to "Fast-Forward Volume",
    "audio.fastForwardVolume.description" to "Output volume while fast-forwarding.",
    "audio.swapChannels.label" to "Swap Left/Right Channels",
    "audio.swapChannels.description" to "Swaps the stereo output (L↔R). Useful when a device's Type-C port forces reverse-landscape and flips the physical speakers (e.g. the Clamp gamepad), which otherwise reverses the stereo image in racing games. Applies instantly.",
    "audio.spu2Simd.label" to "SPU2 SIMD audio (experimental)",
    "audio.spu2Simd.description" to "NEON fast path for reverb audio processing — frees up CPU, which can help performance on CPU-limited devices. Off (default) uses the standard path with unchanged audio. Reboot the game to switch.",
    "audio.openSles.label" to "OpenSL ES audio (compatibility)",
    "audio.openSles.description" to "Uses the older OpenSL ES output path instead of AAudio. A compatibility fallback for devices where the default audio glitches, crackles, or won't initialize — at the cost of slightly higher latency. Most devices should leave this off (AAudio low-latency). Applies instantly.",
    "audio.lightweight.label" to "Lightweight audio (skip reverb)",
    "audio.lightweight.description" to "Skips SPU2 reverb processing to save CPU on low-end devices. This removes all echo and spatial reverb (caves, halls and ambience sound flat), so only enable it if you need the extra performance and SPU2 SIMD audio isn't enough. Off (default) plays full audio. Applies instantly.",
    // --- Recompiler (JIT) tab ---
    // --- extracted UI strings (Increment 3, recovered from git diff) ---
    "action.no" to "No",
    "action.restore" to "RESTORE",
    "action.yes" to "Yes",
    "backend.applyNote" to "Graphics API / driver changes apply on the next renderer start.",
    "backend.applyRestart" to "Apply & Restart",
    "backend.driver.active" to "Active",
    "backend.driver.browseOnline" to "Browse online",
    "backend.driver.download" to "Download",
    "backend.driver.gpuModel" to "GPU",
    "backend.driver.gpuUnknown" to "Unknown",
    "backend.driver.recommended" to "Recommended",
    "backend.driver.recommendSystem" to "System driver (built-in)",
    "backend.driver.hideDownloads" to "Hide downloads",
    "backend.driver.import" to "Import",
    "backend.driver.get" to "Get",
    "backend.driver.fetching" to "Fetching…",
    "backend.driver.none" to "No drivers found",
    "backend.driver.installedOk" to "Installed",
    "backend.driver.downloadFailed" to "Download failed",
    "backend.driver.importFailed" to "Import failed",
    "backend.driver.default" to "Default",
    "backend.driver.fetchError" to "Couldn't reach github.com/K11MCH1/AdrenoToolsDrivers. Check your connection and try again.",
    "backend.driver.hideOnline" to "Hide online",
    "backend.driver.importError" to "Couldn't import that file. It needs an AdrenoToolsDrivers-style .zip (meta.json + libvulkan_freedreno.so at the root).",
    "backend.driver.importZip" to "Import .zip",
    "backend.driver.importing" to "Importing…",
    "backend.driver.install" to "Install",
    "backend.driver.installed" to "Installed",
    "backend.driver.installing" to "Installing…",
    "backend.driver.loadingList" to "Loading driver list…",
    "backend.driver.systemVulkan" to "System Vulkan driver",
    "backend.driver.systemGl" to "System OpenGL driver",
    "backend.driver.angleSubtitle" to "GLES-on-Vulkan · non-Adreno GPUs",
    "backend.driver.use" to "Use",
    "backend.glDriver.label" to "OpenGL Driver",
    "backend.glDriver.description" to "Choose how the OpenGL renderer runs. ANGLE translates OpenGL ES to Vulkan with a bundled driver — helps on non-Adreno GPUs (Mali / Xclipse / PowerVR) whose native OpenGL driver is weak or buggy. Restart the game to apply.",
    "backend.gpuDriver.description" to "Replace the system Vulkan driver with Mesa Turnip or another Adreno driver. Recommended for Adreno-6XX devices on stale OEM drivers.",
    "backend.gpuDriver.label" to "GPU Driver",
    "backend.graphicsApi.label" to "Graphics API",
    "backend.renderer.auto" to "Auto",
    "backend.renderer.software" to "Software",
    "common.off" to "Off",
    "common.on" to "On",
    "fixes.dithering.label" to "Dithering",
    "fixes.zoom.label" to "Display Zoom",
    "fixes.zoom.desc" to "Zooms into the picture so it fills more of the screen, trimming all four edges equally so nothing stretches. 100% is off. Use this instead of the manual crops below for a quick, even zoom on tablets and modern screens; while it's above 100% the manual crops are ignored.",
    "fixes.crop.header" to "Overscan Crop",
    "fixes.crop.left" to "Crop Left",
    "fixes.crop.top" to "Crop Top",
    "fixes.crop.right" to "Crop Right",
    "fixes.crop.bottom" to "Crop Bottom",
    "fixes.crop.desc" to "Trims pixels from the edges of the image before scaling, to hide garbage or black bars a TV would have cut off. Measured in native console pixels.",
    "fixes.integerScaling.label" to "Integer Scaling",
    "fixes.opt.aggr" to "Aggr.",
    "fixes.opt.aggrPlus" to "Aggr.+",
    "fixes.opt.aggressive" to "Aggressive",
    "fixes.opt.auto" to "Auto",
    "fixes.opt.forced" to "Forced",
    "fixes.opt.full" to "Full",
    "fixes.opt.half" to "Half",
    "fixes.opt.inside" to "Inside",
    "fixes.opt.lower" to "Lower",
    "fixes.opt.max" to "Max",
    "fixes.opt.merge" to "Merge",
    "fixes.opt.native" to "Native",
    "fixes.opt.normal" to "Normal",
    "fixes.opt.normalPlus" to "Normal+",
    "fixes.opt.nwTex" to "NW-Tex",
    "fixes.opt.off" to "Off",
    "fixes.opt.on" to "On",
    "fixes.opt.special" to "Special",
    "fixes.opt.sprite" to "Sprite",
    "fixes.opt.sprites" to "Sprites",
    "fixes.opt.triangle" to "Triangle",
    "fixes.opt.upper" to "Upper",
    "games.card.bootWithoutDisc" to "Boot without a disc",
    "games.card.refresh" to "Refresh",
    "games.card.rescanRomsFolder" to "Re-scan ROMs folder",
    "games.card.scanning" to "Scanning…",
    "games.card.startBios" to "Start BIOS",
    "games.empty.noFolders.body" to "Use the Settings cog to add one or more.",
    "games.empty.noFolders.title" to "No ROMs folders configured",
    "games.exit.message" to "Are you sure you want to close the app?",
    "games.exit.title" to "Exit ARMSX1?",
    "games.info.inGameMenu.body" to "While in a game, tap the ⏸ button in the top-right corner to open the pause overlay. If you've turned on \"Tap to reveal pause\", the first tap surfaces the button and the next one opens the menu. On a controller, you can bind hotkeys for the menu and many other toggles in Settings.",
    "games.info.inGameMenu.title" to "In-game menu",
    "games.info.navigating.body" to "Scroll up and down to move between shelves. Scroll left and right to reveal more games on a shelf.",
    "games.info.navigating.title" to "Navigating",
    "games.info.openDataFolder.body" to "Manage memory cards, custom textures, and other files. Tap to open it in ",
    "games.info.openDataFolder.title" to "Open data folder",
    "games.info.perGameSettings.body" to "On a controller, press the X button on a highlighted cover. On touch, long-press a game cover.",
    "games.info.perGameSettings.title" to "Per-game settings",
    "games.info.tapToClose" to "Tap anywhere or press B to close",
    "games.info.title" to "Library Help",
    "games.library.totalGames" to "Total games",
    "games.nav.library" to "LIBRARY",
    "games.overflow.coverStyle" to "Cover style",
    "games.overflow.customNames" to "Custom names",
    "games.overflow.englishTitles" to "English titles",
    "games.overflow.gridNames" to "Game names in grid",
    "games.overflow.showHidden" to "Show hidden games",
    "games.addToHome" to "Add to home screen",
    "games.addToHome.unsupported" to "This launcher doesn't support adding shortcuts to the home screen.",
    "games.hide" to "Hide from library",
    "games.unhide" to "Unhide",
    "games.removeRecent" to "Remove from Recently Played",
    "games.recent.clearAll" to "Clear All",
    "games.recent.clearAll.title" to "Clear Recently Played?",
    "games.recent.clearAll.message" to "This empties the Recently Played shelf. Your games, saves and settings are not touched — titles come back as you play them again.",
    "games.overflow.openNavigation" to "Open navigation",
    "games.overflow.setup" to "Setup / Change Folders",
    "games.overflow.sortRecent" to "Sort by recently played",
    "games.overflow.sortTitle" to "Sort by title",
    "games.noCover" to "No cover",
    "games.noSerial" to "No serial",
    "games.scanningRoms" to "Scanning ROMs…",
    "games.search.hint" to "D-pad move · A type · Done = keep filter · B close & clear",
    "games.search.placeholder" to "Search games…",
    "games.section.library" to "Library",
    "games.section.recentlyPlayed" to "Recently Played",
    "games.section.allGames" to "All Games",
    "games.toast.backgroundImportFailed" to "Could not import background",
    "games.toast.backgroundImported" to "Library background imported",
    "games.toast.backgroundReset" to "Library background reset",
    "games.toast.chooseFolderFirst" to "Choose a game folder first",
    "games.toast.couldntLoadElf" to "Couldn't load ELF",
    "games.toast.scanAlreadyRunning" to "Library scan already running",
    "games.toast.scanningLibrary" to "Scanning library...",
    "games.toolbar.auto" to "Auto",
    "games.toolbar.background" to "Background",
    "games.toolbar.bios" to "BIOS",
    "games.toolbar.cards" to "Cards",
    "games.toolbar.cover2d" to "Cover 2D",
    "games.toolbar.cover3d" to "Cover 3D",
    "games.toolbar.elf" to "ELF",
    "games.section.app" to "Application",
    "games.toolbar.exit" to "Exit",
    "games.toolbar.less" to "Less",
    "games.toolbar.more" to "More",
    "games.toolbar.off" to "Off",
    "games.toolbar.on" to "On",
    "games.toolbar.recent" to "Recent",
    "games.toolbar.resetBg" to "Reset BG",
    "games.toolbar.rows" to "Rows",
    "games.toolbar.covers3d" to "3D covers",
    "games.background.choose" to "Choose background image…",
    "games.background.clear" to "Clear background",
    "games.toolbar.scan" to "Scan",
    "games.toolbar.search" to "Search",
    "games.toolbar.setup" to "Setup",
    "games.unknownSerial" to "Unknown serial",
    "hotkeys.capturePrompt" to "Press a button, push a stick, or 2 together…",
    "hotkeys.clear" to "Clear",
    "hotkeys.exitToLauncher.description" to "When a game was launched from another app (e.g. ES-DE), Close Game returns to that app instead of the ARMSX1 library.",
    "hotkeys.exitToLauncher.label" to "Exit to launcher on close (external games)",
    "hotkeys.backOpensMenu.label" to "Back button opens the menu",
    "hotkeys.backOpensMenu.description" to "In-game, the Android Back button or gesture opens the pause menu. If your Back button is taken over by another app (e.g. Assistant), map a spare button to the “Menu / Pause” hotkey above instead.",
    "hotkeys.header" to "Controller hotkeys",
    "hotkeys.help" to "Bind physical buttons (back paddles work too) to in-game actions — ",
    "hotkeys.notSet" to "Not set",
    "memcard.cancelNew" to "Cancel New",
    "memcard.cardName.label" to "Card name (A to type)",
    "memcard.create" to "Create",
    "memcard.delete.confirm" to "Sure?",
    "memcard.delete.body" to "Remove \"%s\" from ARMSX1?\n\nThis deletes the card that ARMSX1 keeps in its own memory-card folder. If you imported it from a file on your device, that original file is not touched.",
    "memcard.empty" to "No memory-card files found yet.",
    "memcard.globalDefault" to "Global default",
    "memcard.help" to "Import File adds a memory card (then tap Slot 1 or Slot 2 to use it). Import Folder imports memory cards found in a folder, or the folder itself as a folder memory card. Export saves a card out to Downloads / Drive / anywhere for backup or moving to another device. Delete removes a card and clears its slot.",
    "memcard.importFile" to "Import File",
    "memcard.importFolder" to "Import Folder",
    "memcard.marker.bothSlots" to "● SLOTS 1 & 2",
    "memcard.marker.slot1" to "● SLOT 1",
    "memcard.marker.slot2" to "● SLOT 2",
    "memcard.newCard" to "+ New Card",
    "memcard.newCard.title" to "New Memory Card",
    "memcard.perGame.description" to "Boot each game with its own Slot 1 card (named after its serial, auto-created). Restart the game to apply.",
    "memcard.perGame.label" to "Per-Game Memory Cards",
    "memcard.perGameSlot1.description" to "Pick a card for this game only (overrides the auto serial card). Restart the game to apply.",
    "memcard.importFolder" to "Import folder card",
    "memcard.restart" to "Restart",
    "memcard.slot1" to "Slot 1",
    "memcard.slot1.active" to "✓ Slot 1",
    "memcard.thisGame" to "This game",
    "memcard.thisGame.active" to "✓ This game",
    "memcard.useGlobal" to "Use global",
    "memcard.slot2" to "Slot 2",
    "memcard.slot2.active" to "✓ Slot 2",
    "memcard.status.coreStarting" to "Core settings are still starting up.",
    "memcard.status.defaultSlotsEnabled" to "Default memory-card slots enabled.",
    "memcard.status.enterName" to "Enter a card name first.",
    "memcard.title" to "Memory Cards",
    "memcard.toast.created" to "Memory card created",
    "memcard.toast.folderImported" to "Folder memory card imported",
    "memcard.toast.imported" to "Memory card imported",
    "memcard.useDefaultSlots" to "Use Default Slots",
    "overlay.interfaceScaling.description" to "Interface scaling — resize the library and menu UI to fit your screen. ",
    "overlay.intro.description" to "Show or hide parts of the performance overlay. Turning GPU off also ",
    "overlay.toggle.controlInputs" to "Control inputs (bottom-left)",
    "overlay.toggle.cpuUsage" to "CPU usage",
    "overlay.toggle.emulationSpeed" to "Emulation speed %",
    "overlay.toggle.emulatorVersion" to "Emulator version",
    "overlay.toggle.fastForwardPopups" to "Fast-Forward pop-ups",
    "overlay.toggle.fps" to "FPS",
    "overlay.master.label" to "On-screen display",
    "overlay.simple.label" to "Simple OSD (FPS only)",
    "overlay.toggle.frameTimesGraph" to "Frame times graph",
    "overlay.toggle.gpuPipelineStats" to "GPU pipeline stats (VSI/PSI, Vulkan only)",
    "overlay.toggle.gpuUsage" to "GPU usage (saves perf when off)",
    "overlay.toggle.gsStatistics" to "GS statistics",
    "overlay.toggle.hardwareInfo" to "Hardware info (CPU/GPU model)",
    "overlay.toggle.internalResolution" to "Internal resolution",
    "overlay.toggle.onScreenNotifications" to "On-screen notifications (shader compile, saves, etc.)",
    "overlay.toggle.settingsSummary" to "Settings summary (bottom-right)",
    "overlay.toggle.vps" to "VPS (vblanks/sec)",
    "overlay.uiFontSize.description" to "Scales menu/library text. 100% = default.",
    "overlay.uiFontSize.label" to "UI Font Size",
    "overlay.uiSize.description" to "Scales menu/library padding and control sizes. 100% = default.",
    "overlay.osdColor.label" to "OSD Color",
    "overlay.osdColor.description" to "Color of the on-screen display text (FPS, stats and notifications). Speed warnings stay red/green so they still stand out.",
    "overlay.osdColor.default" to "White",
    "overlay.osdColor.green" to "Green",
    "overlay.osdColor.cyan" to "Cyan",
    "overlay.osdColor.yellow" to "Yellow",
    "overlay.osdColor.orange" to "Orange",
    "overlay.osdColor.red" to "Red",
    "overlay.osdColor.pink" to "Pink",
    "overlay.osdColor.purple" to "Purple",
    "overlay.osdSize.label" to "OSD Size",
    "overlay.osdSize.description" to "Size of the on-screen display drawn over the game — FPS, stats and notifications. Does not affect the app's menus. 100% = the core's default; ARMSX1 ships 65% so it doesn't crowd a handheld screen.",
    "overlay.uiSize.label" to "UI Size (borders)",
    "pad.action.bind" to "Bind",
    "pad.action.clear" to "Clear",
    "pad.action.edit" to "Edit",
    "pad.controller.notBound" to "Controller: not bound",
    "pad.dpadAsLeftStick.description" to "Make the physical D-pad drive the left analog stick (full deflection) so it works in games that only read the analog stick. The D-pad stops sending digital presses while this is on.",
    "pad.dpadAsLeftStick.label" to "D-pad acts as Left Stick",
    "pad.editTouchLayout" to "Edit On-Screen Touch Layout",
    "pad.controllerMapping" to "Controller mapping",
    "perf.frameLimit.label" to "Frame limit",
    "perf.ffSpeed.label" to "Fast-Forward Speed",
    "common.unlimited" to "Unlimited",
    "pad.editing.description" to "Configure Player 1 or Player 2's button mapping. Player 2 = the 2nd controller that joins in-game.",
    "pad.editing.label" to "Editing",
    "pad.usb.section" to "USB Devices",
    "pad.usb.port" to "Port",
    "pad.usb.none" to "Not Connected",
    "pad.usb.subtype" to "Variant",
    "pad.usb.help" to "Attach an emulated USB device to a port \u2014 Buzz buzzers, a Rock Band drum kit, Keyboardmania, a DJ turntable, GunCon 2 and more. Your normal controls drive it, no extra mapping needed. Restart the game after changing a port: the game checks what is plugged in when it boots.",
    "pad.lightgun.section" to "Lightgun (GunCon 2)",
    "pad.lightgun.enable.label" to "Enable GunCon 2",
    "pad.lightgun.enable.description" to "Attaches an emulated GunCon 2 to a USB port and aims it with the touchscreen. Restart the game after changing this \u2014 the game probes the port when it boots.",
    "pad.lightgun.port.label" to "USB port",
    "pad.lightgun.port1" to "Port 1",
    "pad.lightgun.port2" to "Port 2",
    "pad.lightgun.port.description" to "Most lightgun games expect the gun in Port 1.",
    "pad.lightgun.help" to "Touch to aim and fire. Touching near a screen edge fires OFF-SCREEN, which is how these games reload. A/B/C, Start, Select and Cal (recalibrate) appear down the right edge.",
    "pad.gesture.section" to "Gesture Control",
    "pad.gesture.enable.label" to "Enable gesture control",
    "pad.gesture.enable.description" to "Swipes and a double-tap on empty screen area fire a button. Only where there is no on-screen control \u2014 a finger that lands on a button, stick or D-pad still drives that control.",
    "pad.gesture.none" to "None",
    "pad.gesture.up" to "Swipe Up",
    "pad.gesture.down" to "Swipe Down",
    "pad.gesture.left" to "Swipe Left",
    "pad.gesture.right" to "Swipe Right",
    "pad.gesture.sensitivity.label" to "Swipe distance",
    "pad.gesture.sensitivity.description" to "How far a finger must travel to count as a swipe, as a percentage of the shorter screen edge. Lower = more sensitive.",
    "pad.gesture.doubleTap" to "Double tap",
    "pad.gesture.doubleTapMode.label" to "Double-tap mode",
    "pad.gesture.doubleTapMode.tap" to "Tap",
    "pad.gesture.doubleTapMode.hold" to "Hold",
    "pad.gesture.doubleTapMode.description" to "Tap: a quick press, for something one-shot like NFS nitro. Hold: the button stays down until you double-tap again, for something you want to keep on like an ARPG camera lock.",
    "pad.gyro.section" to "Motion / Gyroscope",
    "pad.gyro.mode.label" to "Gyroscope control",
    "pad.gyro.mode.off" to "Off",
    "pad.gyro.mode.aim" to "Aim (look)",
    "pad.gyro.aimStick.label" to "Aim uses stick",
    "pad.gyro.aimStick.right" to "Right",
    "pad.gyro.aimStick.left" to "Left (e.g. RE4)",
    "pad.gyro.mode.steering" to "Steering (tilt)",
    "pad.gyro.sensitivity.label" to "Gyro sensitivity",
    "pad.gyro.smoothing.label" to "Gyro smoothing",
    "pad.gyro.invertX.label" to "Invert gyro X",
    "pad.gyro.invertY.label" to "Invert gyro Y",
    "pad.gyro.unavailable" to "This device has no motion sensor for that mode.",
    "pad.gyro.tiltFallback.steering" to "No gyroscope — using the accelerometer. Tilt works normally for steering. Recenter to set the neutral position.",
    "pad.gyro.tiltFallback.aim" to "No gyroscope — using the accelerometer. Aim is tilt-based: twist to look left/right, tip to look up/down. Recenter to set the neutral position.",
    "pad.instruction.tapThenPress" to "Tap an action, then press a physical controller button.",
    "pad.leftStick.description" to "What the left analog stick sends: Analog (default), Face, or Custom (bind each direction below).",
    "pad.leftStick.invertX.description" to "Mirror the left stick horizontally — fixes \"left is right\".",
    "pad.leftStick.invertX.label" to "Left Stick — Invert X",
    "pad.leftStick.invertY.description" to "Mirror the left stick vertically — fixes \"down is up\".",
    "pad.leftStick.invertY.label" to "Left Stick — Invert Y",
    "pad.leftStick.label" to "Left Stick",
    "pad.leftStick.swapXY.description" to "Swap the left stick's horizontal and vertical axes (for a stick that reads rotated 90°).",
    "pad.leftStick.swapXY.label" to "Left Stick — Swap X/Y",
    "pad.leftStickFeel.title" to "Left Stick Feel",
    "pad.macro.notSet" to "Not set",
    "pad.macroConfig.intro" to "Tap the buttons this macro should press together.",
    "pad.macro.frequency.description" to "Hold: the buttons stay pressed while you hold the macro. Any other value turns it into a turbo — the buttons toggle on and off at that rate, so mash-heavy games don't cost you a thumb. Lower = faster.",
    "pad.macro.frequency.every" to "Every %d frames",
    "pad.macro.frequency.hold" to "Hold",
    "pad.macro.frequency.label" to "Frequency",
    "pad.macros.header" to "Macros (combo buttons — touch + physical)",
    "pad.onScreenControls.description" to "On-screen touch buttons. Never = always hidden (for physical-controls devices — also hides the settings cog so nothing overlaps R1). 1–10s = auto-hide after that long without a touch. Auto = show on touch, hide when you use a controller.",
    "pad.multiTouch.description" to "How far from a button's center a touch still counts as a press, so you can hit two adjacent on-screen buttons at once. Higher = more reach (easier rolls and combos).",
    "pad.onScreenControls.label" to "On-screen controls",
    "pad.player1" to "Player 1",
    "pad.player2" to "Player 2",
    "pad.pressButton" to "Press a button...",
    "pad.pressControllerButton" to "Press a controller button…",
    "pad.rightStick.description" to "What the right analog stick sends: Analog (default), Face, or Custom (bind each direction below).",
    "pad.rightStick.invertX.description" to "Mirror the right stick horizontally — fixes \"left is right\".",
    "pad.rightStick.invertX.label" to "Right Stick — Invert X",
    "pad.rightStick.invertY.description" to "Mirror the right stick vertically — fixes \"down is up\".",
    "pad.rightStick.invertY.label" to "Right Stick — Invert Y",
    "pad.rightStick.label" to "Right Stick",
    "pad.rightStick.swapXY.description" to "Swap the right stick's horizontal and vertical axes (for a stick that reads rotated 90°).",
    "pad.rightStick.swapXY.label" to "Right Stick — Swap X/Y",
    "pad.rightStickFeel.title" to "Right Stick Feel",
    "pad.multitap.description" to "Enable Multitap so up to 8 controllers can play. Connect your gamepads, then launch the game — the first pad is Player 1, the second is Player 2, and any others fill the multitap slots. Leave off for normal 1–2 player games.",
    "pad.multitap.label" to "Multitap (up to 8 players)",
    "pad.rumble.description" to "Master switch for controller rumble and the device's built-in vibration. Turn off to silence all haptics.",
    "pad.rumble.label" to "Rumble / Vibration",
    "pad.hapticStrength.description" to "Scales all vibration — controller rumble and on-screen touch haptics alike. Below 100% tames a strong motor; above 100% boosts a weak one.",
    "pad.hapticStrength.label" to "Vibration Strength",
    "pad.scopeHint.global" to "○ Editing GLOBAL controls (all games).",
    "pad.scopeHint.globalWithGameHint" to "○ Editing GLOBAL controls (all games). Switch to Game up top for a per-game map.",
    "pad.padProfiles.info" to "Save the button map, stick modes and stick binds above under a name, then pick it again later. A profile applies to the player and scope you're editing (shown above), so you can save one map and apply it per game. Stick feel — deadzone, sensitivity, rumble — isn't part of a profile: it describes your pad, not the game. Profiles save to the inputprofiles folder, so they survive moving your data folder.",
    "pad.padProfiles.namePlaceholder" to "Profile name",
    "pad.padProfiles.none" to "No saved profiles yet.",
    "pad.padProfiles.saveAs" to "Save As",
    "pad.padProfiles.saveNewLabel" to "Save this map as a new profile:",
    "pad.section.analogSticks" to "Analog Sticks",
    "pad.section.buttonMapping" to "Button Mapping",
    "pad.section.macros" to "Macros",
    "pad.section.onScreenControls" to "On-Screen Controls",
    "pad.section.padProfiles" to "Mapping Profiles",
    "pad.section.playerRumble" to "Player & Rumble",
    "pad.stickFeel.acceleration.description" to "Non-linear response curve: small tilts stay precise for aiming, full tilt ramps up to full speed. 0 = linear (off); higher = more curve.",
    "pad.stickFeel.acceleration.label" to "Acceleration",
    "pad.stickFeel.antiDeadzone.description" to "Smallest output sent to the game, to cancel a game's OWN built-in stick deadzone (e.g. Cold Fear / Area 51 ignore the stick until ~45%, then aim jumps). Set near the game's deadzone so any stick movement responds immediately and the full travel maps smoothly above it. 0 = off.",
    "pad.stickFeel.antiDeadzone.label" to "Anti-Deadzone",
    "pad.stickFeel.deadzone.description" to "Fraction of physical analog travel ignored near center (applied to the stick's radial distance, so diagonals behave like cardinals). Output re-normalizes past it, so movement still ramps smoothly from 0 — which also means the on-screen effect can be masked by a game's OWN built-in deadzone (Area 51 ignores input below ~45% no matter what you set here; use Anti-Deadzone for that). 0 = off — raw hardware values pass through, including any stick drift.",
    "pad.stickFeel.deadzone.label" to "Deadzone",
    "pad.stickFeel.responseCurve.label" to "Response Curve",
    "pad.stickFeel.responseCurve.description" to "Reshapes stick response: higher settings make small tilts gentler for finer aim while full tilt still reaches max. Tames over-sensitive hall-effect sticks (e.g. GTA: San Andreas).",
    "pad.stickFeel.curve.linear" to "Linear",
    "pad.stickFeel.curve.light" to "Light",
    "pad.stickFeel.curve.medium" to "Medium",
    "pad.stickFeel.curve.strong" to "Strong",
    "pad.stickFeel.outerDeadzone.description" to "Fraction of travel near the EDGE mapped to full output, so a stick that can't physically reach its corners still hits 100% (short-throw / handheld sticks like the Odin). 0 = off.",
    "pad.stickFeel.outerDeadzone.label" to "Outer Deadzone",
    "pad.stickFeel.sensitivity.description" to "Linear scale on this stick (native Analog + Custom analog directions). Under 100% = finer/slower, over 100% = faster. Per-stick: tune camera aim without slowing movement.",
    "pad.stickFeel.sensitivity.label" to "Sensitivity",
    "pad.stickTarget.analogDefault" to "Analog (default)",
    "pad.stickTarget.hotkeys" to "Hotkeys",
    "pad.stickTarget.intro" to "Choose what this stick direction sends. Works regardless of which physical buttons are bound.",
    "pad.stickTarget.ps2Buttons" to "PS1 Buttons",
    "pad.testRumble.player1" to "Test rumble — Player 1",
    "pad.testRumble.player2" to "Test rumble — Player 2",
    "pad.touchHaptics.description" to "Vibrate briefly when you press an on-screen button (like PPSSPP / Azahar). Separate from controller rumble.",
    "pad.touchHaptics.label" to "Touch Haptics",
    "pad.pauseTapToReveal.label" to "Tap to reveal pause",
    "pad.pauseTapToReveal.description" to "Keep the on-screen pause button (top-right ⏸) hidden until you tap its corner, which surfaces it for a few seconds — tap again to open the pause menu. Off shows the button all the time. Either way that corner always reaches the menu, so this can't lock you out. Drag/resize it in the overlay editor.",
    "patches.disclaimer" to "Cheats and patches written for a different emulator can freeze, crash or glitch games here. If a game misbehaves, turn patches and cheats off and test again before reporting it — if it only happens with them on, it's the patch.",
    "patches.bundled.header" to "Bundled with ARMSX1",
    "patches.bundled.explain" to "These ship inside ARMSX1 and are applied to this game automatically — you didn't install them. Ones with no name are always on and can't be switched off individually.",
    "patches.bundled.alwaysOn" to "always on, no name to switch",
    "patches.bundled.extract" to "Copy to my patches (makes them editable)",
    "patches.bundled.extracted" to "Copied. The bundled version is now ignored — switch the cheats off individually above.",
    "patches.bundled.extractFailed" to "Couldn't copy the bundled patch file.",
    "patches.action.allOff" to "All off",
    "patches.action.allOn" to "All on",
    "patches.action.apply" to "APPLY",
    "patches.action.cancel" to "CANCEL",
    "patches.action.cancelMixed" to "Cancel",
    "patches.action.delete" to "Delete",
    "patches.action.dontAskAgain" to "DON'T ASK AGAIN",
    "patches.action.edit" to "Edit",
    "patches.action.execute" to "Execute",
    "patches.action.no" to "NO",
    "patches.action.none" to "None",
    "patches.action.save" to "SAVE",
    "patches.action.selectAll" to "Select all",
    "patches.action.yes" to "YES",
    "patches.applyAtBoot" to "Patches apply at boot — restart the game after changing these.",
    "patches.button.browseOnline" to "⤓  Browse online — patches & cheats",
    "patches.button.enterCodes" to "+ Enter codes",
    "patches.button.fetching" to "Fetching…",
    "patches.button.importPnach" to "+ Import .pnach",
    "patches.button.myLocalToggle" to "☰  My local patches & cheats  (toggle on/off)",
    "patches.cheats.label" to "Cheats",
    "patches.cheats.labelHardcore" to "Cheats — disabled in Hardcore",
    "patches.dialog.enterCodesTitle" to "Enter PNACH Codes",
    "patches.dialog.myLocal" to "My local patches & cheats",
    "patches.dialog.noActiveCrc" to "No active CRC found; start the game first for auto-naming.",
    "patches.dialog.patchesAndCheats" to "Patches & cheats",
    "patches.editHint" to "Tick the cheats to keep on. Unticked ones are commented out (kept in the file).",
    "patches.enablePatches.label" to "Enable Patches",
    "patches.field.name" to "Name",
    "patches.field.pnachLines" to "PNACH patch= lines",
    "patches.hardcoreNoticeCheatsDisabled" to "Cheats are disabled while RetroAchievements Hardcore mode is active. Patches still apply.",
    "patches.hostFs.description" to "Lets the game read files from the host: namespace — needed for some ELF / homebrew and advanced mods.",
    "patches.hostFs.label" to "HostFS (host: filesystem)",
    "patches.installedFilesHint" to "Installed files (including cheats you copied into the cheats folder). ",
    "patches.import.folder" to "Import folder of cheats\u2026",
    "patches.import.noneFound" to "No .pnach or .txt cheat files in that folder.",
    "patches.installedHeader" to "Installed patches & cheats (.pnach)",
    "patches.local.noCheats" to "No individually-labelled cheats in this file.",
    "patches.online.header" to "Browse online",
    "patches.online.fetch" to "Find patches & cheats for this game",
    "patches.online.loading" to "Searching the community repos…",
    "patches.online.install" to "Install selected",
    "patches.section.patches" to "Patches",
    "patches.section.cheats" to "Cheats",
    "patches.noFilesInstalled" to "No patch or cheat files installed yet.",
    "patches.noInterlacing.label" to "No-Interlacing Patches",
    "patches.pasteImportHint" to "Paste/import PNACH patch= lines. Hardcore achievements disables cheats.",
    "patches.startOrLongPress" to "Start a game, or long-press one in your library, to browse its patches.",
    "patches.status.bootOrLongPress" to "Boot the game, or long-press it in your library, to browse its patches.",
    "patches.status.cheatsDisabledHardcore" to "Cheats are disabled while RetroAchievements Hardcore mode is active.",
    "patches.status.noLocalEntries" to "No local patches or cheats with individual entries yet. Import a .pnach, or copy one into the cheats/patches folder.",
    "patches.status.searchingDatabase" to "Searching the patch database…",
    "patches.warning.message" to "Using patch codes can have unpredictable effects on games, causing ",
    "patches.warning.title" to "Patch codes",
    "patches.widescreen.label" to "Auto-Apply Widescreen (16:9) Patches",
    "patches.widescreen.description" to "Automatically applies a widescreen patch to EVERY game that has one — you don't pick them per game. Games that weren't built for 16:9 can come out stretched, cropped, or missing on-screen text, so turn this off if a game looks wrong. Applies at boot — restart the game after changing this.",
    // ---- PS1 cheats ----------------------------------------------------------
    // GameShark codes from a .cht file, applied by psx/cheats.c. Distinct from the `patches.*`
    // keys above, which belonged to the removed PS2 PNACH manager. Keys follow the same
    // convention as the rest of this map: <feature>.<thing>.label / .description.
    "cheats.title" to "Cheats",
    "cheats.section.forGame" to "Cheats for this game",
    "cheats.noGame.title" to "No game selected",
    "cheats.noGame.body" to "Cheats belong to one game. Long-press a game in your library and choose Cheats, or start a game and open this from the pause menu.",
    "cheats.info.file" to "Cheat file",
    "cheats.info.entries" to "Cheats in file",
    "cheats.info.noFile" to "no file yet",
    "cheats.info.armed" to "Active right now",
    "cheats.info.armed.description" to "Counted by the emulator itself, not by what's ticked here — if these disagree, the emulator is right.",
    "cheats.master.label" to "Enable cheats",
    "cheats.master.labelHardcore" to "Cheats — disabled in Hardcore",
    "cheats.master.description" to "Master switch for this game. Off leaves your selection intact but applies nothing.",
    "cheats.empty.body" to "No cheats in this game's file yet. Paste codes below, or import a .cht file — GameShark / Action Replay codes like \"800AB3C4 0063\" are the format.",
    "cheats.row.lines" to "%d codes",
    "cheats.row.unsupported" to "contains a code type this build can't run",
    "cheats.hardcore.title" to "Cheats are off in Hardcore mode",
    "cheats.hardcore.body" to "RetroAchievements Hardcore mode disables every cheat. Your selection is kept and comes back when Hardcore is off.",
    "cheats.mismatch.title" to "This list doesn't match the emulator",
    "cheats.mismatch.body" to "This screen read %d cheats from the file but the emulator loaded %d. Reload from file; if they still differ, please report it — the emulator's count is the one that matters.",
    "cheats.orphans.title" to "Some enabled cheats are missing",
    "cheats.orphans.body" to "These are switched on but no longer in the file, so they do nothing: %s. That usually means the cheat was renamed or the file was replaced.",
    "cheats.orphans.clear" to "Forget them",
    "cheats.note.nextLaunch" to "This game isn't running, so your selection applies the next time you start it.",
    "cheats.action.paste" to "Paste codes",
    "cheats.action.paste.description" to "Add GameShark / Action Replay codes. One code per line: an 8-digit address then its value.",
    "cheats.action.paste.placeholder" to "800AB3C4 0063",
    "cheats.action.import" to "Import a cheat file",
    "cheats.action.import.description" to "Add a .cht or .txt code list to this game's cheats.",
    "cheats.action.reload" to "Reload from file",
    "cheats.action.reload.description" to "Re-read the cheat file — use this after editing it outside the app.",
    "cheats.pastedGroup" to "Pasted codes",
    "perf.displayFpsCap.description" to "Caps the PRESENTED (display) frame rate — emulation keeps full speed, so this is a display cap, not true emulation FPS. Any value works (use with Speed Limit % to fine-tune per game). 0 = off; values between the clean rates (60/30/20/15) pace a little unevenly.",
    "perf.displayFpsCap.label" to "Display FPS Cap",
    "perf.screenRes.label" to "Screen resolution override",
    "perf.screenRes.auto" to "Auto",
    "perf.screenRes.description" to "Force a fixed 16:9 output resolution instead of the detected screen size. Fixes 16:10 or mis-detected panels (e.g. a 1080p screen reported as 1920×1200) that squish 16:9 games and widescreen patches. Auto uses the device screen.",
    "perf.fix.skipBios" to "Skip BIOS",
    // Per-setting descriptions for the GameDB Fixes toggles (restored after the UI rework).
    "perf.fix.skipBios.desc" to "Boots the game directly, skipping the console startup/BIOS animation. Safe to leave on.",
    "perf.frameSkip.description" to "Drops frames from the SCREEN, never from the emulation — the game, its timing and its audio run exactly as they would with this off. Auto drops one only when the device falls behind, so it costs nothing while it keeps up; 1/2 and below always drop, which is choppier but predictable. To slow the game itself, use Emulation speed or Frame rate cap instead.",
    "perf.frameSkip.label" to "Frame Skip",
    // Per-setting descriptions for the Advanced Speedhacks toggles (restored after the UI rework).
    "perf.speedLimit.description" to "Emulation speed as % of native (100 = full speed). Affects audio pitch, game timing and RetroAchievements (hardcore stays at/above 100%). This is NOT a display cap — pair it with Display FPS Cap for per-game tuning. Best left at 100 unless a game needs it.",
    "perf.speedLimit.label" to "Speed Limit %",
    "perf.lowLatencyMode.label" to "Low Latency Mode",
    "perf.lowLatencyMode.description" to "Uses a zero-frame GS queue and requests a matching high-refresh display mode to reduce controller-to-screen latency. Off by default: it leaves the graphics thread no slack, so it can make games less smooth even on fast devices. Worth trying if input feels laggy, and turning back off if the frame pacing suffers.",
    "perf.sustainedPerformance.description" to "Holds a steady, thermally-sustainable GPU/CPU clock for long play sessions — reduces mid-session throttling, heat and battery drain on handhelds. Trade-off: it caps the peak clock, so demanding games that rely on short bursts of max speed may run a little slower. Off = full peak clocks (default).",
    "perf.sustainedPerformance.label" to "Sustained Performance",
    "perf.adpf.description" to "EXPERIMENTAL, off by default. Reports each frame's active CPU work to Android's performance-hint system (ADPF) so the scheduler can raise the emulation threads' CPU frequency toward the frame deadline, countering the governor under-clocking emulation's bursty load. Helps most when a game can't quite hit full speed and is CPU-bound. This basic path hints CPU scheduling only — no explicit GPU timing — and the measured work can still include a stall behind the GS/present queue. May raise power draw and heat; gains vary by device (needs Android 13+) and can be zero if your governor is already aggressive. Measure with your own A/B before trusting it.",
    "perf.adpf.label" to "CPU clock hint (ADPF)",
    "ra.account.logout" to "Logout",
    "ra.account.signedIn" to "Signed in",
    "ra.mode.casual" to "CASUAL",
    "ra.mode.hardcore" to "HARDCORE",
    "ra.options.header" to "Options",
    "ra.title" to "RetroAchievements",
    "ra.subset.base" to "Base Set",
    "ra.filter.all" to "All",
    "ra.filter.unlocked" to "Unlocked",
    "ra.filter.locked" to "Locked",
    "ra.filter.missable" to "Missable",
    "ra.server.reset" to "Reset achievements server",
    "ra.server.reset.desc" to "Clears a custom/offline server override — fixes sign-in failing with 'No response'.",
    "settings.search.placeholder" to "Search settings…",
    "settings.search.noResults" to "No matching settings",
    "ra.viewAchievements" to "Achievements & Options",
    "ra.hardcore.enable.title" to "Enable hardcore mode?",
    "ra.hardcore.enable.body" to "This restarts the game now and turns off save states, cheats, and speed changes. Achievements you earn will count for hardcore.",
    "ra.hardcore.enable.confirm" to "Enable & Restart",
    "ra.hardcore.disable.title" to "Switch to casual mode?",
    "ra.hardcore.disable.body" to "Save states and cheats become available again, but new unlocks won't count as hardcore until you re-enable it.",
    "ra.hardcore.disable.confirm" to "Switch to Casual",
    "ra.options.inGameIndicators" to "In-Game Indicators",
    "ra.options.inGameIndicators.desc" to "On-screen indicators for challenge and measured-progress achievements.",
    "ra.options.leaderboardNotifications" to "Leaderboard Notifications",
    "ra.options.leaderboardNotifications.desc" to "Announce when you start or submit a leaderboard entry.",
    "ra.options.leaderboardTrackers" to "Leaderboard Trackers",
    "ra.options.leaderboardTrackers.desc" to "Live trackers while a leaderboard attempt is active.",
    "ra.options.notifDuration" to "Notification Duration",
    "ra.options.notifDuration.desc" to "How long achievement-unlock pop-ups stay on screen.",
    "ra.options.lbDuration" to "Leaderboard Duration",
    "ra.options.lbDuration.desc" to "How long leaderboard pop-ups stay on screen.",
    "ra.options.notifLocation" to "Notification Location",
    "ra.options.indicatorLocation" to "Indicator Location",
    "ra.options.encoreMode" to "Encore Mode",
    "ra.options.encoreMode.desc" to "Re-notify achievements you've already unlocked as you earn them again this session. For replaying a game and seeing the pop-ups.",
    "ra.options.soundEffects" to "Sound Effects",
    "ra.options.soundVolume" to "Sound Volume",
    "ra.options.soundVolume.desc" to "Volume of the achievement unlock sound.",
    "ra.options.spectatorMode" to "Spectator Mode",
    "ra.options.spectatorMode.desc" to "Track achievements without sending any unlocks to the server — nothing is recorded to your account.",
    "ra.options.unofficialTestMode" to "Test Unofficial Achievements",
    "ra.options.unofficialTestMode.desc" to "Load achievements from unofficial/in-development sets. These aren't tracked by RetroAchievements, so they unlock every session.",
    "ra.options.soundEffects.desc" to "Play a sound when an achievement unlocks.",
    "ra.options.unlockSound" to "Achievement unlock sound",
    "ra.options.unlockSound.default" to "Default — tap to choose a custom sound (e.g. Xbox 360)",
    "ra.options.unlockNotifications" to "Unlock Notifications",
    "ra.options.unlockNotifications.desc" to "Show a popup when you unlock an achievement.",
    "ra.status.loading.body" to "Fetching achievement list.",
    "ra.status.loading.title" to "Loading…",
    "ra.status.noAchievements.body" to "This game has no RetroAchievements set, or the title isn't recognised.",
    "ra.status.noAchievements.title" to "No achievements",
    "ra.status.notSignedIn.body" to "Tap (or press A) to sign in to RetroAchievements and track unlocks for the games you play.",
    "ra.status.notSignedIn.title" to "Not signed in",
    "ra.tier.common" to "Common",
    "ra.tier.epic" to "Epic",
    "ra.tier.legendary" to "Legendary",
    "ra.tier.rare" to "Rare",
    "ra.tier.uncommon" to "Uncommon",
    "ra.time.justNow" to "just now",
    "ra.typeChip.missable" to "Missable",
    "ra.typeChip.progression" to "Progression",
    "ra.typeChip.win" to "Win",
    "ralogin.loginFailed" to "Login failed.",
    "ralogin.password.label" to "Password (A to type)",
    "ralogin.passwordNotStored" to "Password isn't stored — rcheevos exchanges it for an auth token.",
    "ralogin.signIn" to "Sign in",
    "ralogin.signingIn" to "Signing in…",
    "ralogin.title" to "Sign in to RetroAchievements",
    "ralogin.username.label" to "Username (A to type)",
    "renderer.angleOpenGL.label" to "OpenGL via ANGLE",
    "renderer.angleOpenGL.description" to "Run the OpenGL renderer through ANGLE, which translates OpenGL ES to Vulkan using a bundled driver. Useful on devices whose native OpenGL ES driver is broken or slow (e.g. some MediaTek Mali). Only takes effect when the renderer is set to OpenGL. Restart the game to apply.",
    "renderer.clearShaderCache.alreadyEmpty" to "Shader cache is already empty.",
    "renderer.clearShaderCache.description" to "Wipes the compiled Vulkan + GL shader/pipeline caches. Use if a game renders corrupt after a driver swap or update — the next launch rebuilds them clean.",
    "renderer.clearShaderCache.label" to "Clear Shader Cache",
    "renderer.cas.description" to "AMD FidelityFX Contrast-Adaptive Sharpening — crisps up the image.",
    "renderer.cas.label" to "CAS Sharpening",
    "renderer.cas.sharpen" to "Sharpen",
    "renderer.cas.sharpenResize" to "Sharpen + Resize",
    "renderer.cas.sharpness.label" to "CAS Sharpness",
    "renderer.fxaa.description" to "Fast post-process anti-aliasing — smooths jagged edges with a light blur.",
    "renderer.fxaa.label" to "FXAA",
    "renderer.displayFilter.label" to "Display Filter",
    "renderer.displayMode.description" to "Controls how the game image fits the screen.",
    "renderer.displayMode.label" to "Display Mode",
    "renderer.customAspect.label" to "Custom Aspect Ratio",
    "renderer.customAspect.description" to "Width divided by height, used when Display Mode (or FMV Aspect) is set to Custom. 1.78 is 16:9, 2.22 is 20:9, 2.17 is 19.5:9.",
    "renderer.orientation.autoRotate" to "Auto-Rotate",
    "renderer.orientation.description" to "Locks the app's screen orientation. \"Device\" follows your system auto-rotate setting.",
    "renderer.orientation.device" to "Device",
    "renderer.orientation.label" to "Emulation Screen Orientation",
    "renderer.portraitPosition.label" to "Portrait Render Position",
    "renderer.portraitPosition.top" to "Top",
    "renderer.portraitPosition.center" to "Center",
    "renderer.portraitPosition.description" to "In portrait, place the game at the top of the screen (leaving the bottom free for touch controls) or vertically centered. Only affects portrait orientation.",
    "perf.affinity.label" to "Affinity Control Mode",
    "perf.affinity.disabled" to "Disabled",
    "perf.affinity.performanceCores" to "Performance Cores",
    "perf.affinity.description" to "Experimental. Pins the emulator's EE, VU and GS threads to specific CPU cores, in the priority order you pick (the first listed gets the fastest core). \"Performance Cores\" keeps all three on the big cluster without fixing individual cores. Disabled is recommended and is the default — Android's scheduler normally places these threads better than manual pinning, but GS-heavy games can benefit from putting GS first. Applies on the next boot.",
    "renderer.orientation.landscape" to "Landscape",
    "renderer.orientation.portrait" to "Portrait",
    "renderer.section.displayEffects" to "Display Effects",
    "renderer.section.displayResolution" to "Display & Resolution",
    "renderer.shaderChain.description" to "Runs a RetroArch (.slangp) shader chain over the final image — CRT masks, handheld LCD looks, scalers. Costs GPU time and stacks on top of the other display effects.",
    "renderer.shaderChain.empty" to "No .slangp presets found. Download a pack under Shader Packs below, or drop one into this folder by hand and reopen this list:",
    "renderer.shaderChain.label" to "RetroArch Shaders",
    "renderer.shaderChain.params.countModified" to "%d changed",
    "renderer.shaderChain.params.description" to "Tweak this preset's own settings",
    "renderer.shaderChain.params.empty" to "This preset doesn't expose any adjustable parameters.",
    "renderer.shaderChain.params.hint" to "D-Pad ↑↓ pick  ·  ←→ adjust  ·  A resets  ·  B closes",
    "renderer.shaderChain.params.label" to "Shader Parameters",
    "renderer.shaderChain.params.loading" to "Reading preset…",
    "renderer.shaderChain.params.modified" to "Changed",
    "renderer.shaderChain.params.namePlaceholder" to "Preset name…",
    "renderer.shaderChain.params.resetAll" to "Reset all to preset defaults",
    "renderer.shaderChain.params.resetAll.confirmBody" to "Every parameter goes back to the preset's own defaults. Your changes here can't be undone — if you want to keep them, cancel and use \"Save as new preset\" first.",
    "renderer.shaderChain.params.resetAll.confirmTitle" to "Reset all parameters?",
    "renderer.shaderChain.params.saveAs" to "Save as new preset…",
    "renderer.shaderChain.pass" to "pass",
    "renderer.shaderChain.passes" to "passes",
    "renderer.shaderChain.passesUnknown" to "cost unknown",
    "renderer.shaderChain.preset.label" to "Shader Preset",
    "renderer.shaderChain.preset.none" to "None",
    "renderer.shaderChain.uncategorised" to "Uncategorised",
    "renderer.shaderPack.cancelled" to "Download cancelled",
    "renderer.shaderPack.companion.description" to "Ready-made presets built on top of the RetroArch pack's shaders. They install into it and need it first — they contain no shaders of their own.",
    "renderer.shaderPack.companion.needsBase" to "Install RetroArch · Slang Shaders first",
    "renderer.shaderPack.import.browse" to "Browse",
    "renderer.shaderPack.import.description" to "Already have a pack? Install it from a folder or a .zip. It lands in the same place as the download above, so its presets show up in the Shader Preset list just the same.",
    "renderer.shaderPack.import.failed" to "No .slangp presets found in that folder or .zip.",
    "renderer.shaderPack.import.folder" to "Import from folder",
    "renderer.shaderPack.import.folder.description" to "Pick the pack's top folder",
    "renderer.shaderPack.import.label" to "Import a pack",
    "renderer.shaderPack.import.zip" to "Import from .zip",
    "renderer.shaderPack.import.zip.description" to "Pick a .zip of a shader pack",
    "renderer.shaderPack.description" to "Download RetroArch (.slangp) shader packs. Installed presets show up in the Shader Preset list above. Large download — Wi-Fi recommended.",
    "renderer.shaderPack.downloadFailed" to "Download failed",
    "renderer.shaderPack.downloading" to "Downloading…",
    "renderer.shaderPack.extracting" to "Extracting…",
    "renderer.shaderPack.hint" to "Turn on RetroArch Shaders and pick a preset to use these.",
    "renderer.shaderPack.installedOk" to "Installed",
    "renderer.shaderPack.label" to "Shader Packs",
    "renderer.shaderPack.noneInstalled" to "No shader packs installed",
    "renderer.shaderPack.presets" to "presets",
    "renderer.shaderPack.starting" to "Starting…",
    "renderer.upscale.description" to "Internal resolution. Higher values are sharper but can expose game-specific bloom or alignment artifacts.",
    "renderer.upscale.label" to "Upscale",
    "renderer.vsync.description" to "Sync presentation to the display refresh — less tearing/smoother, slightly more latency. Restart the game to apply.",
    "savestate.autoLoadOnBoot" to "Auto-load last state on boot",
    "savestate.autoSaveInterval.description" to "Save automatically while you play, so a crash or a flat battery costs at most this much progress. It writes the same auto-save slot as the option above, so your numbered slots stay yours. Saving pauses the game for a moment, so a short interval is felt — 5 minutes is a good starting point.",
    "savestate.autoSaveInterval.every" to "Every %d min",
    "savestate.autoSaveInterval.label" to "Auto-save while playing",
    "savestate.autoSaveInterval.off" to "Off",
    "savestate.autoSaveOnExit" to "Auto-save on exit",
    "savestate.autosave.savedOnExit" to "(saved on exit)",
    "savestate.autosave.screenshotDesc" to "Autosave screenshot",
    "savestate.autosave.title" to "Autosave",
    "savestate.backup" to "Backup",
    "savestate.empty.description" to "Create a save state while a game is running, then manage or back it up here.",
    "savestate.empty.title" to "No save states yet",
    "savestate.noBackupFound" to "No backup found",
    "savestate.noSavesToBackUp" to "No saves to back up",
    "savestate.restore" to "Restore",
    "savestate.restoreBackup.confirmMessage" to "Replaces this game's current slots with the last backup. Current saves are overwritten.",
    "savestate.restoreBackup.confirmTitle" to "Restore backup?",
    "savestate.slot.emptyTapToSave" to "(empty — tap to save here)",
    "savestate.title.loadManage" to "Load / Manage Saves",
    "pad.pressureAmount.label" to "Pressure modifier amount",
    "pad.pressureAmount.description" to
        "How hard the pressure modifier presses, for pressure-sensitive games " +
        "(Metal Gear Solid, GTA). Applies to the on-screen PRESSURE button and to the " +
        "\"Pressure Modifier (hold)\" binding. Lower = softer press.",
    "app.library.coverSize" to "Cover size",
    "app.blockHome" to "Block Home button while playing",
    "app.blockHome.desc" to
        "Pins the screen while a game runs, so a controller's Home or Guide button can't minimise " +
        "it. Android asks you to confirm the first time. To leave, hold Back + Recents — or just " +
        "quit to the library, which unpins automatically.",
    "savestate.error.hardcore" to
        "Save states are disabled while RetroAchievements Hardcore Mode is on. Turn Hardcore off " +
        "in the RetroAchievements settings to use them (this forfeits hardcore points for the session).",
    "savestate.error.memcardBusy" to
        "The game is still writing to the memory card, so the state was not saved. " +
        "Resume the game for a second or two, then try again — the card stays busy for as " +
        "long as the game is paused.",
    "savestate.error.save" to "Couldn't save to that slot. Check the log for @@ANDROID_SAVESTATE@@.",
    "savestate.error.load" to "Couldn't load that slot.",
    "savestate.title.save" to "Save State",
    // Memory-card divergence. Says what happens to the player's save, in the player's terms —
    // "the card is ahead of the state" is the diagnosis, not the consequence, and the
    // consequence is what they need in order to answer the question.
    "savestate.cardWarning.newer.title" to "Memory card is newer than this save state",
    "savestate.cardWarning.newer.body" to
        "You saved to the memory card after this save state was made. Loading it takes the game " +
        "back to before that card save — and undoes it.\n\n" +
        "Some games cope with this. Others refuse to load their save file afterwards, or behave " +
        "as though the memory card is damaged.",
    "savestate.cardWarning.diverged.title" to "Memory card doesn't match this save state",
    "savestate.cardWarning.diverged.body" to
        "The memory card has changed since this save state was made — it may have been erased, " +
        "imported, or written by a different session. There's no way to tell which one is newer.\n\n" +
        "Loading can leave the game and the card disagreeing about what has been saved, which " +
        "some games report as a damaged card.",
    "savestate.cardWarning.loadAnyway" to "Load anyway",
    "savestate.cardWarning.dontWarn" to "Skip memory card mismatch warning",
    "savestate.cardWarning.dontWarn.description" to
        "Don't ask before loading a save state whose memory card has changed since it was made. " +
        "The warning exists because loading such a state can undo an in-game save and make some " +
        "games refuse their own save file. Off by default.",
    "setup.aspect.stretch" to "Stretch",
    "setup.bios.error.noneFound" to "No valid PS1 BIOS files found in that folder.",
    "setup.bios.multipleFound" to "BIOS files ready — switch anytime in BIOS settings.",
    "setup.bios.noneSelected" to "No BIOS folder selected yet — use the Pick BIOS Folder button below.",
    "setup.bios.scanning" to "Scanning…",
    "setup.bios.selectTitle" to "Select BIOS",
    "setup.bios.unknownZone" to "Unknown",
    "setup.bios.useSelected" to "Use Selected BIOS",
    "setup.button.addAnotherFolder" to "Add Another Folder",
    "setup.button.addFolder" to "Add Folder",
    "setup.button.applyFinish" to "Apply & Finish",
    "setup.button.choose" to "Choose",
    "setup.button.chooseDataLocation" to "Choose Data Location",
    "setup.button.clear" to "Clear",
    "setup.button.letsGo" to "Let's Go",
    "setup.button.pickBiosFolder" to "Pick BIOS Folder",
    "setup.button.pickCustomFolder" to "Pick Custom Folder",
    "setup.button.pickDifferentFolder" to "Pick a different folder",
    "setup.button.pickRomsFolder" to "Pick ROMs Folder",
    "setup.button.remove" to "Remove",
    "setup.button.scanDifferentFolder" to "Scan Different Folder",
    "setup.button.scanFolder" to "Scan Folder",
    "setup.button.scanning" to "Scanning...",
    "setup.button.selectFolder" to "Select Folder",
    "setup.button.skip" to "Skip",
    "setup.gpuDriver.active" to "✓ Active",
    "setup.gpuDriver.add" to "Add Driver",
    "setup.gpuDriver.availableTitle" to "Available drivers",
    "setup.gpuDriver.default.detail" to "Use the device's stock Vulkan ICD",
    "setup.gpuDriver.default.label" to "Default",
    "setup.gpuDriver.default.sublabel" to "System driver",
    "setup.gpuDriver.description" to "Replace the system Vulkan driver with Mesa Turnip or another Adreno driver. Recommended for Adreno-6XX users on stale OEM drivers. Takes effect on the next game launch.",
    "setup.gpuDriver.error.importFailed" to "Couldn't import that file. It needs to be an AdrenoToolsDrivers-style .zip with meta.json + libvulkan_freedreno.so at the root.",
    "setup.gpuDriver.error.unreachable" to "Couldn't reach github.com/K11MCH1/AdrenoToolsDrivers. Check your connection and try again.",
    "setup.gpuDriver.install" to "Install",
    "setup.gpuDriver.installing" to "Installing…",
    "setup.gpuDriver.loading" to "Loading driver list…",
    "setup.gpuDriver.noneAvailable" to "No drivers available right now.",
    "setup.gpuDriver.pickLocalZip" to "Pick local .zip",
    "setup.gpuDriver.select" to "Select",
    "setup.gpuDriver.sourceNote" to "Source: github.com/K11MCH1/AdrenoToolsDrivers — Mesa Turnip and friends. Each driver lands under app-private storage.",
    "setup.gpuDriver.title" to "GPU Driver",
    "setup.page.bios.title" to "Select your BIOS",
    "setup.page.renderer.title" to "Choose renderer",
    "setup.page.roms.title" to "Select ROMs folder",
    "setup.page.systemDir.title" to "System data folder",
    "setup.page.welcome.title" to "Welcome",
    "setup.perf.fast" to "Fast",
    "setup.perf.lowEnd" to "Low-End",
    "setup.perf.optimal" to "Optimal",
    "setup.recommended.antiBlur" to "Anti-Blur",
    "setup.recommended.aspectRatio" to "Aspect Ratio",
    "setup.recommended.deinterlacing" to "Deinterlacing",
    "setup.recommended.internalResolution" to "Internal Resolution",
    "setup.recommended.performance" to "Performance",
    "setup.recommended.renderer" to "Renderer",
    "setup.recommended.subtitle" to "Optional — set these now or change them anytime in Settings.",
    "setup.recommended.title" to "Recommended Settings",
    "setup.recommended.widescreenPatches" to "Widescreen Patches",
    "setup.restart.message" to "Moving app data to a new location takes effect after a restart. ",
    "setup.restart.now" to "Restart now",
    "setup.restart.title" to "Restart required",
    "setup.status.biosSelected" to "BIOS selected",
    "setup.status.internalStorage" to "Internal storage (main device)",
    "setup.status.notSelected" to "Not selected",
    "setup.status.oneFolderSelected" to "1 folder selected",
    "setup.status.scanningBios" to "Scanning BIOS folder...",
    "setup.step.appData.description.allFiles" to "Where memory cards, save states, and configs are stored. Choose Internal, an SD card, or a custom folder. (Game ROMs are added separately.)",
    "setup.step.appData.description.play" to "Where memory cards, save states, and configs are stored. Internal uses your main device storage; SD Card uses a memory card if one is present. (Game ROMs are added separately.)",
    "setup.step.appData.title" to "App Data Folder",
    "setup.step.bios.description" to "Pick a folder of PS1 BIOS files to start playing — every BIOS inside is added, along with any matching .mec and .nvm files.",
    "setup.step.bios.title" to "BIOS Location",
    "bios.boot.title" to "Boot BIOS",
    "bios.perGame.menu" to "Per-game BIOS",
    "bios.thisGame" to "This game",
    "bios.thisGame.active" to "✓ This game",
    "bios.useGlobal" to "Use global",
    "setup.step.rom.description" to "Pick one or more folders where you keep your PS1 games. Supports ISO, CHD, BIN, IMG, MDF, and GZ.",
    "setup.step.rom.title" to "ROM Location",
    "setup.storageChooser.customFolder" to "Custom Folder…",
    "setup.storageChooser.description" to "Internal lives in app-private storage (wiped on uninstall). SD Card creates ",
    "setup.storageChooser.grantAllFiles" to "Grant All-Files Access…",
    "setup.storageChooser.internal" to "Internal (app-private)",
    "setup.storageChooser.customShort" to "Custom folder",
    "setup.storageChooser.customSubtitle" to "Pick any folder (grants all-files access)",
    "setup.storageChooser.internalShort" to "Internal",
    "setup.storageChooser.title" to "App data location",
    "setup.systemDir.appPrivateSubtitle" to "App-private Android/data folder",
    "setup.systemDir.customFolder" to "Custom folder",
    "setup.systemDir.error.grantAllFiles" to "Couldn't write to that folder. Grant All-Files Access, then pick it again.",
    "setup.systemDir.error.noSdCard" to "No SD card detected — staying on Internal storage.",
    "setup.systemDir.error.notWritable" to "That folder can't be used for writable emulator data on this Android version. ",
    "setup.systemDir.error.tryAnother" to "That folder can't be used for writable emulator data. Try another.",
    "setup.systemDir.intro" to "ARMSX1 stores memory cards, save states, configs, and shader data in app-private storage by default. ",
    "setup.systemDir.noneSelected" to "No system data folder selected yet. Use the app-private default or pick a custom folder.",
    "setup.systemDir.sdCard" to "SD Card",
    "setup.systemDir.selectedLabel" to "Selected:",
    "setup.systemDir.usingAppPrivate" to "Using App-Private Folder",
    "setup.toggle.off" to "Off",
    "setup.toggle.on" to "On",
    "setup.welcome.heading" to "Welcome to ARMSX1 setup!",
    "setup.button.next" to "Next",
    "setup.welcome.subheading" to "Hit Next to get started",
    "skins.activeSkin" to "Active skin",
    "skins.activeSkin.game" to "Active skin (this game)",
    "skins.builtinDefault" to "Built-in (default)",
    "skins.perGame.description" to "Off, this game uses whatever skin you picked for all games. On, it keeps its own — handy for giving a light-gun or racing game its own look without touching the rest of your library.",
    "skins.perGame.label" to "Use a different skin for this game",
    "skins.description" to "Import a folder or .zip of ic_controller_*.png images (iOS-format skin packs work). ",
    "skins.browse" to "Download skins…",
    "skins.browse.hide" to "Hide downloadable skins",
    "skins.browse.loading" to "Loading skins…",
    "skins.browse.failed" to "Couldn't reach the skin repository. Check your connection and try again.",
    "skins.browse.get" to "Get",
    "skins.browse.installing" to "…",
    "skins.browse.installFailed" to "Download failed, or that pack had no usable button images.",
    "skins.importFolder" to "Import skin folder…",
    "skins.importZip" to "Import skin .zip…",
    "skins.importing" to "Importing…",
    "skins.status.importedAndSelected" to "Imported and selected.",
    "skins.title" to "Custom Controller Skins",
    "touch.dpad.down.description" to "DPad down",
    "touch.dpad.left.description" to "DPad left",
    "touch.dpad.right.description" to "DPad right",
    "touch.dpad.up.description" to "DPad up",
    "touch.editor.discard" to "Discard",
    "touch.editor.floatingStickOff" to "Floating Stick Off",
    "touch.editor.floatingStickOn" to "Floating Stick On",
    "touch.editor.fullHalfSticksOff" to "Half-Screen Sticks Off",
    "touch.editor.fullHalfSticksOn" to "Half-Screen Sticks On",
    "touch.editor.glidingOff" to "Gliding Off",
    "touch.editor.glidingOn" to "Gliding On",
    "touch.editor.gridOff" to "Grid Off",
    "touch.editor.gridOn" to "Grid On",
    "touch.editor.hide" to "Hide",
    "touch.editor.multiTouchOff" to "Multi-Touch Off",
    "touch.editor.multiTouchOn" to "Multi-Touch On",
    "touch.editor.profiles" to "Profiles",
    "touch.editor.scopeGame" to "Editing this game's touch layout",
    "touch.editor.scopeGlobal" to "Editing Global Default touch layout",
    "touch.editor.show" to "Show",
    "touch.editor.tapHoldOff" to "Tap-Hold Off",
    "touch.editor.tapHoldOn" to "Tap-Hold On",
    "touch.pause.editLabel" to "PAUSE",
    "touch.profiles.infoGame" to "Choosing a profile here also sets it as this game's layout. Profiles save to the inputprofiles folder.",
    "touch.profiles.infoGlobal" to "Profiles save to the inputprofiles folder, so they're portable and survive moving your data folder.",
    "touch.profiles.namePlaceholder" to "Profile name",
    "touch.profiles.saveAs" to "Save As",
    "touch.profiles.saveNewLabel" to "Save current layout as new profile:",
    "touch.profiles.title" to "Touch Control Profiles",
    "touch.settingsButton.description" to "Open in-game settings",
    "touch.stateAction.load" to "LOAD",
    "touch.stateAction.save" to "SAVE",
    "touch.stateAction.screenshot" to "SHOT",

    // ---- PS1 settings tabs ------------------------------------------------------------------
    // The PS1-core tabs (Video / Emulation / Audio / BIOS / Advanced) were written after the
    // ARMSX2 UI was lifted, so every row was a raw English literal and stayed English in every
    // language. Keys follow the same convention as the rest of this map: <tab>.<setting>.label
    // and .description, options as <tab>.<setting>.<option>.
    "common.auto" to "Auto",
    "common.none" to "None",
    "common.custom" to "Custom",
    "action.show" to "Show",
    "action.hide" to "Hide",
    "action.clear" to "Clear",
    "settings.resetTab.description" to "Reset every option on this tab to the core's defaults.",
    // --- settings tab titles (the PS1 tabs were literals in categoryTitle) ---
    "tab.video" to "Video",
    "tab.emulation" to "Emulation",
    "tab.bios" to "BIOS",
    "tab.library" to "Library",
    "tab.interface" to "Interface",

    // --- Video tab ---
    "renderer.intro" to
        "Display and GPU options for the PlayStation core. Most are read when a game starts; " +
        "Display mode and Stretch apply to a running game immediately.",
    "renderer.gpuBackend.label" to "Graphics backend",
    "renderer.gpuBackend.description" to
        "How the finished frame is PRESENTED — separate from the rasteriser below, which is what " +
        "draws it. OpenGL ES draws straight to the surface via EGL and skips the CPU copy the " +
        "software path does; ANGLE runs that same path on Google's GLES-on-Vulkan translator, " +
        "which is worth trying when a device's own GLES driver is weak or buggy. Falls back " +
        "automatically if a backend is unavailable — the in-game Display tab shows which one is " +
        "actually running.",
    "renderer.gpuBackend.software" to "Software",
    "renderer.gpuBackend.sdlAccel" to "SDL accel",
    "renderer.gpuBackend.systemGl" to "OpenGL ES (system)",
    "renderer.gpuBackend.angle" to "OpenGL ES (ANGLE)",
    "renderer.gpuBackend.vulkan" to "Vulkan",
    "renderer.hwRasterizer.label" to "Hardware rasterizer",
    "renderer.hwRasterizer.description" to
        "Draws the PlayStation's graphics on the GPU so the internal resolution can be raised " +
        "above native — this is what makes 2x and higher possible. Raising the scale costs GPU " +
        "time, not CPU. At 1x it looks identical to the software rasteriser, so turn it on when " +
        "you want to upscale. On a device with no usable GL context it falls back to a CPU " +
        "rasteriser, which IS slower.",
    "renderer.pgxp.label" to "PGXP geometry precision",
    "renderer.pgxp.description" to
        "Uses the console's full-precision vertex math instead of the truncated coordinates games " +
        "were forced to use — removes the PS1's characteristic wobbling polygons and swimming " +
        "textures, and enables perspective-correct texturing. Hardware rasteriser only. Applies " +
        "live; a fraction of a second of geometry re-transforms on enable.",
    "renderer.maskBit.label" to "GPU mask bit",
    "renderer.maskBit.description" to
        "Honour the PlayStation's draw-mask exactly like real hardware. Games that use it (Silent " +
        "Hill's light and darkness compositing is the classic case) render wrong without it; " +
        "games that don't use it are completely unaffected. Leave on.",
    "renderer.dither.label" to "Authentic dithering",
    "renderer.dither.description" to
        "Dither only when the game asks for it, exactly like real hardware. Off forces dithering " +
        "onto everything, including images the game wanted clean. Leave on.",
    "renderer.widescreenHack.label" to "Widescreen hack",
    "renderer.widescreenHack.description" to
        "Widens the 3D camera so the world FILLS a 16:9 screen instead of being stretched into " +
        "it — you see more to the left and right, and circles stay circles. Pair it with the 16:9 " +
        "display mode below. 2D backgrounds, menus and the HUD are not drawn by the 3D unit, so " +
        "those still stretch; a few games also pop geometry in at the edges they never expected " +
        "you to see.",
    "renderer.textureFilter.label" to "Texture filtering",
    "renderer.textureFilter.description" to
        "Smooths the low-resolution textures on 3D surfaces. Bilinear blends the four nearest " +
        "texels — softest, and the cheapest. xBR-style blends only ALONG detected edges and stays " +
        "sharp across them, which keeps hand-drawn texture art from turning to mush. 2D sprites, " +
        "text and the HUD are left pixel-exact either way. Hardware rasterizer only.",
    "renderer.textureFilter.nearest" to "Nearest",
    "renderer.textureFilter.bilinear" to "Bilinear",
    "renderer.textureFilter.xbr" to "xBR-style",
    "renderer.textureReplacements.label" to "Texture replacements",
    "renderer.textureReplacements.description" to
        "Swaps in your own artwork for the game's textures, from PNG files in the " +
        "replacements folder below. Each file is matched by the hash in its name, so a pack " +
        "works on any build and needs no per-game setup beyond being in the right folder. A " +
        "replacement may be 2x, 4x or 8x the original size; it is drawn at that resolution " +
        "and ignores Texture filtering, which exists to hide the size of the original texels. " +
        "Off costs nothing at all.",
    "renderer.textureDump.label" to "Dump textures",
    "renderer.textureDump.description" to
        "Writes every texture the game samples to the dump folder below, as a PNG named by " +
        "its hash — the starting point for making a pack. Rename or repaint a file, drop it " +
        "in the replacements folder, and it is picked up. An untouched dump put back is " +
        "pixel-for-pixel identical to no replacement at all. Costs storage and some speed " +
        "while it is on, so leave it off unless you are authoring.",
    "renderer.textureFolder.label" to "Texture folder",
    "renderer.textureFolder.description" to
        "Dumps are written to dump/ inside this folder and replacements are read from " +
        "replacements/. Both are created automatically. The folder is per game, keyed on the " +
        "disc serial, so one game's pack can never match another's textures by accident.",
    "renderer.textureDir.label" to "Custom texture folder",
    "renderer.textureDir.description" to
        "Overrides the folder above — use it to keep a pack on shared storage. Leave it " +
        "empty to go back to the automatic per-game folder.",
    "renderer.textureDir.placeholder" to "Automatic (per game)",
    "renderer.downsample.label" to "Downsampling",
    "renderer.downsample.description" to
        "Renders at the internal resolution above, then averages blocks of pixels back down before " +
        "showing them. That is what removes the jagged, sparkly edges upscaling leaves behind — " +
        "you pay the full cost of the high resolution and get a cleaner picture rather than a " +
        "bigger one. The factor has to divide the internal resolution evenly; anything else steps " +
        "down to the nearest one that does.",
    "renderer.downsample.active" to "Active: internal resolution is %sx.",
    "renderer.downsample.inactive" to
        "Needs the hardware rasterizer with internal resolution above 1x — it does nothing as " +
        "things are set now.",
    "renderer.lineDetect.label" to "Line detection",
    "renderer.lineDetect.description" to
        "Games draw thin lines — wires, rails, laser sights, UI rules — as flat polygons, and one " +
        "with no thickness at all draws nothing. This gives those a pixel of thickness so they " +
        "show up. Quads only rescues the ones that vanish completely, which is the safe choice; " +
        "Basic also thickens lines that are already one pixel, which can make them look heavier " +
        "than intended. Hardware rasterizer only.",
    "renderer.lineDetect.quads" to "Quads",
    "renderer.lineDetect.basic" to "Basic",
    "renderer.deinterlace.label" to "Deinterlacing",
    "renderer.deinterlace.description" to
        "Only affects games that switch to the 480-line mode — mostly menus, maps and a handful of " +
        "RPGs. Weave shows both halves of the picture as the game left them, which is sharpest and " +
        "can comb into stripes when things move. Bob shows one half and doubles it: no combing, " +
        "half the vertical detail. Adaptive measures each frame and only bobs when combing is " +
        "actually there.",
    "renderer.deinterlace.weave" to "Weave",
    "renderer.deinterlace.bob" to "Bob",
    "renderer.deinterlace.adaptive" to "Adaptive",
    "renderer.overscan.label" to "Overscan crop",
    "renderer.overscan.description" to
        "A TV hid the outermost few percent of the picture, so games left junk there — a black " +
        "bar, a torn column, a stray line. This trims that off and zooms what is left to fill the " +
        "same space. Small takes about 3% from each edge, Full about 6%. It is a zoom, so it does " +
        "lose a little of the real image.",
    "renderer.overscan.small" to "Small",
    "renderer.overscan.full" to "Full",
    "renderer.rotation.label" to "Display rotation",
    "renderer.rotation.description" to
        "Turns the picture clockwise. Useful for vertical shooters and for handhelds mounted " +
        "sideways; the image is rotated, the controls are not. The OpenGL and software display " +
        "backends do this; the experimental Vulkan one cannot and stays upright.",
    "renderer.internalRes.label" to "Internal resolution",
    "renderer.internalRes.description" to
        "Renders at this multiple of the PlayStation's native resolution. Cost grows with the " +
        "SQUARE of the multiplier, so 4x is roughly sixteen times the rasterising work of 1x — " +
        "raise it a step at a time.",
    "renderer.outputScale.label" to "Display resolution",
    "renderer.outputScale.description" to
        "Renders the output surface smaller and lets the display hardware scale it back up — cuts " +
        "GPU cost, heat and battery without changing the emulated resolution. Steps are multiples " +
        "of the PlayStation's native 240 lines.",
    "renderer.outputScale.screen" to "Screen",
    "renderer.outputScale.native3x" to "3x native",
    "renderer.outputScale.native2x" to "2x native",
    "renderer.outputScale.native1x" to "1x native",
    "renderer.screenRes.label" to "Screen resolution",
    "renderer.screenRes.description" to
        "Forces the output surface to a fixed size instead of the detected panel. Fixes panels " +
        "that mis-report their size — a 1080p screen claiming 1920x1200 squishes the image.",
    "renderer.lowLatency.label" to "Low latency mode",
    "renderer.lowLatency.description" to
        "Stops the emulator running ahead of presentation and asks the panel for a refresh rate " +
        "that is a whole multiple of the game's — 60 picks 120 Hz, PAL 50 picks 100 Hz — so there " +
        "is no uneven cadence.",
    "renderer.sustainedPerf.label" to "Sustained performance",
    "renderer.sustainedPerf.description" to
        "Asks Android to hold a steady, thermally sustainable clock instead of boosting then " +
        "throttling. Better over a long session, but it CAPS peak clock, so a demanding game can " +
        "lose frames.",
    "renderer.adpf.label" to "CPU clock hint (ADPF)",
    "renderer.adpf.description" to
        "EXPERIMENTAL, off by default. Reports each frame's actual CPU work to Android's " +
        "performance-hint service so the governor can clock for the frame deadline instead of " +
        "guessing from a bursty load. Needs Android 13 or newer and does nothing below it. May " +
        "raise power draw and heat, and can be worth nothing on a device whose governor is " +
        "already aggressive — A/B it yourself on a scene that will not hold full speed.",
    "renderer.affinity.label" to "CPU affinity",
    "renderer.affinity.description" to
        "EXPERIMENTAL, off by default. Restricts the emulation thread to a group of CPU cores. " +
        "The performance cores are detected from the device's own clock table, not assumed. " +
        "Android's scheduler usually places the thread well on its own, so Off is still the " +
        "recommendation — this is for the devices where it does not.",
    "renderer.affinity.performanceCores" to "Performance cores",
    "renderer.affinity.allCores" to "All cores",
    "renderer.verticalSync.label" to "Vertical sync",
    "renderer.verticalSync.description" to
        "Present in step with the display refresh. Off can reduce latency at the cost of tearing.",
    "renderer.bilinear.label" to "Bilinear filtering",
    "renderer.bilinear.description" to
        "Smooths the scaled output instead of using nearest-neighbour pixels.",
    "renderer.integerScaling.label" to "Integer scaling",
    "renderer.integerScaling.description" to
        "Scale by a whole number only, so every emulated pixel becomes an exact block instead of " +
        "some being duplicated and others not — which is what makes scrolling 2D shimmer. It " +
        "deliberately shrinks the image to the nearest whole multiple, and has no effect in " +
        "Stretch mode.",
    "renderer.aspect.label" to "Display mode",
    "renderer.aspect.description" to
        "Classic is a hard 4:3 — the display aspect a PlayStation was authored for on a CRT. " +
        "Square shows the raw framebuffer pixels. Stretch fills the screen and ignores the ratio " +
        "entirely. Applies immediately, including to a running game.",
    "renderer.aspect.stretch" to "Stretch",
    "renderer.aspect.classic" to "Classic 4:3",
    "renderer.aspect.square" to "Square 1:1",
    "renderer.aspect.wide" to "Wide 16:9",
    "renderer.customRatio.label" to "Custom ratio",
    "renderer.customRatio.description" to
        "Width divided by height, currently %.3f. Values outside %.1f–%.1f are ignored by the core " +
        "rather than clamped, so an out-of-range entry keeps the previous ratio.",
    "renderer.wideUpscale.label" to "Widescreen output height",
    "renderer.wideUpscale.description" to
        "Desktop window height for the 16:9 path — no effect on Android, and not an " +
        "internal-resolution upscale.",
    "renderer.debugPanel.label" to "Debug panel",
    "renderer.debugPanel.description" to "Show the core's built-in debug overlay over the game.",
    "renderer.reset.label" to "Restore video defaults",
    // Pause-menu Device section + the two PS1 stat toggles whose names are not pure hardware
    // (the R3000A / GTE / SPU / MDEC / CD-ROM / DMA rows stay literal — those are chip names).
    "perf.section.device" to "Device",
    "overlay.toggle.gpuPrimitives" to "GPU (primitives, fill)",
    "overlay.toggle.frameTimes" to "Frame times",
    "overlay.toggle.deviceUsage" to "Device CPU / GPU / RAM",
    // In-game pause menu (Display / Fixes panes), the same PS1 rows in short form.
    "renderer.vsync.label" to "VSync",
    "fixes.dithering.description" to
        "Gate dithering the way the console does. Off is the faster approximation.",
    "fixes.maskBit.label" to "Accurate mask bit",
    "fixes.maskBit.description" to
        "Honour the GPU's mask/semi-transparency bit exactly. Fixes some flicker and shadow " +
        "artefacts.",
    "fixes.integerScaling.description" to "Not implemented in the PlayStation core yet.",

    // --- Emulation tab ---
    "perf.intro" to
        "Core behaviour and speed for the PlayStation CPU and console. The CPU and region options " +
        "are read while the emulator boots; the speed options apply immediately.",
    "perf.cpuMode.label" to "CPU execution mode",
    "perf.cpuMode.description" to
        "Cached is the fast default. Interpreter is slower but the accuracy reference — try it if " +
        "a game misbehaves. Applies on next launch.",
    "perf.cpuMode.cached" to "Cached",
    "perf.cpuMode.interpreter" to "Interpreter",
    "perf.region.label" to "Console region",
    "perf.region.description" to
        "Auto picks the region from the disc. Forcing a region also decides which BIOS the " +
        "auto-selector prefers. Applies on next launch.",
    "perf.skipBios.label" to "Skip BIOS",
    "perf.skipBios.description" to
        "Boot straight into the game instead of sitting through the PlayStation startup logo. A " +
        "few games expect the BIOS to have run and misbehave without it — turn this off if a game " +
        "will not start.",
    "perf.frameLimit.description" to
        "Pace the emulation to the target below. Off runs as fast as the device manages — useful " +
        "for loading screens, but the game runs too fast to play.",
    "perf.emulationSpeed.label" to "Emulation speed",
    "perf.emulationSpeed.description" to
        "Percentage of the game's own frame rate — 50% really does halve a 59.94 Hz game to ~30 " +
        "fps, audio included. Ignored while Frame limit is off.",
    "perf.fpsCap.label" to "Frame rate cap",
    "perf.fpsCap.description" to
        "An extra absolute ceiling in fps, applied on top of the percentage above. It only ever " +
        "lowers the target, so a cap above the game's own rate does nothing. Ignored while Frame " +
        "limit is off.",
    "perf.frameSkipMode.label" to "Frame skip",
    "perf.frameSkipMode.description" to
        "Drops frames from the SCREEN, never from the emulation: the machine is still run to every " +
        "vertical blank, so the game's timing and its audio are identical either way. Auto drops a " +
        "frame only when the device has fallen behind and costs nothing while it keeps up; the " +
        "fixed ratios always drop. This is not a speed control — for that use Emulation speed or " +
        "Frame rate cap above.",
    "perf.fastForwardSpeed.label" to "Fast-forward speed",
    "perf.fastForwardSpeed.description" to
        "How fast the fast-forward hotkey and button run. Unlimited removes the cap entirely; a " +
        "device that cannot reach the chosen speed simply runs as fast as it can. Audio follows " +
        "the speed either way — see the Audio tab.",
    "perf.rewind.label" to "Rewind",
    "perf.rewind.description" to
        "Hold the Rewind button (bind it under Settings › Hotkeys) to run the game backwards. " +
        "Costs memory, not speed: the current setting keeps %s of save states in RAM, and nothing " +
        "at all is allocated while this is off. Applies immediately.",
    "perf.rewindBuffer.label" to "Rewind buffer",
    "perf.rewindBuffer.description" to
        "How far back you can go. This one really is a memory dial — the current combination " +
        "reserves about %s.%s Long buffers on a handheld get the app killed by the system, so " +
        "start short.",
    "perf.rewindBuffer.capped" to "Capped to about %ds by the memory limit.",
    "perf.rewindPrecision.label" to "Rewind precision",
    "perf.rewindPrecision.description" to
        "Snapshots taken per second. Higher lands you closer to the moment you wanted, and " +
        "multiplies the memory above by exactly the same factor.",
    "perf.runahead.label" to "Runahead",
    "perf.runahead.description" to
        "Hides input lag by running the game a few frames ahead of what you see. Be aware of what " +
        "it costs: EVERY frame it saves a state, reloads it, and re-simulates that many extra " +
        "frames. A device already running at 100% will simply drop to a fraction of full speed " +
        "instead of feeling snappier — try 1 first, and leave it Off unless you have headroom. " +
        "Applies immediately.",
    "perf.reset.label" to "Restore emulation defaults",
    "perf.reset.description" to
        "Back to Cached / Auto, frame limit on at 100%, frame skip off, fast-forward 2×, rewind " +
        "and runahead off.",

    // --- Audio tab ---
    "audio.intro" to
        "Output for the PlayStation sound processor. The mixer options apply the next time a game " +
        "is launched.",
    "audio.master.label" to "Volume",
    "audio.master.description" to "Above 100% amplifies the SPU output and can clip loud scenes.",
    "audio.muted.label" to "Mute",
    "audio.muted.description" to
        "Silence the output. The SPU keeps running, so envelopes and timing are unaffected.",
    "audio.swap.label" to "Swap left/right channels",
    "audio.swap.description" to
        "Mirror the stereo image. For headsets or wiring that came out reversed.",
    "audio.bufferMs.label" to "Audio buffer",
    "audio.bufferMs.description" to
        "Bigger is more resistant to crackle and dropouts, at the cost of latency. 13 ms is the " +
        "core's own default. Applies on next launch.",
    "audio.skipReverb.label" to "Lightweight audio (skip reverb)",
    "audio.skipReverb.description" to
        "Bypass the SPU reverb network to save CPU on weak devices. Removes all echo and room " +
        "ambience from games that use it. Applies on next launch.",
    "audio.backend.label" to "Audio backend",
    "audio.backend.description" to
        "OpenSL ES is the compatibility path and the default — it is the only backend that needs " +
        "nothing from SDL's Java layer, which this app does not run. AAudio can be lower latency " +
        "but is experimental here; the core falls back to OpenSL ES on its own if it cannot be " +
        "brought up safely. Applies on next launch.",
    "audio.backend.openSles" to "OpenSL ES",
    "audio.backend.aaudio" to "AAudio",
    "audio.background.label" to "Keep playing in the background",
    "audio.background.description" to
        "Off (recommended): switching the screen off or leaving the app pauses the game and stops " +
        "the audio stream. On: the game keeps running and playing with the screen off, which " +
        "drains the battery and leaves audio coming out of a device that looks switched off.",
    "audio.fastForward.header" to "Fast-forward",
    "audio.fastForward.note" to
        "Audio keeps playing while fast-forwarding: the emulated stream is resampled down to the " +
        "output rate, so nothing is dropped and nothing overruns — but the pitch rises with the " +
        "speed, the same way it does on hardware you can spin faster. Mute it here if that grates.",
    "audio.muteFastForward.label" to "Mute during fast-forward",
    "audio.muteFastForward.description" to
        "Silence while fast-forward is engaged, instead of the pitched-up stream.",
    "audio.ffVolume.label" to "Fast-forward volume",
    "audio.ffVolume.description" to
        "Used instead of Volume while fast-forwarding. Ignored when the mute above is on.",
    "audio.reset.label" to "Restore audio defaults",

    // --- BIOS tab ---
    "bios.intro" to
        "The PlayStation needs a BIOS dump. Pick the console model you want the core to prefer, or " +
        "import one specific file to force it.",
    "bios.model.label" to "Preferred console model",
    "bios.model.description" to
        "Which dump the auto-selector picks when several are found in the BIOS folder. Ignored " +
        "while a BIOS file is forced below.",
    "bios.forced.label" to "Forced BIOS file",
    "bios.forced.description" to
        "settings.toml [bios].override_file — overrides the model preference.",
    "bios.forced.none" to "Not set (auto-select)",
    "bios.forced.missing" to "%s (missing)",
    "bios.forced.size" to "%s · %d KB",
    "bios.import.label" to "Import BIOS file…",
    "bios.import.description" to "Copies the dump into the emulator's own folder and forces it.",
    "bios.import.ok" to "BIOS imported",
    "bios.import.failed" to "Could not read that file",
    "bios.clearForced.label" to "Clear forced BIOS",
    "bios.clearForced.description" to "Go back to auto-selecting from the BIOS folder.",
    "bios.searchFolder.label" to "BIOS search folder",
    "bios.searchFolder.description" to "Relative to the emulator's data folder: %s",
    "bios.bundled.label" to "Bundled BIOS",
    "bios.bundled.description" to
        "bios.bin copied out of the app package on first run, when shipped.",
    "bios.bundled.present" to "Present",
    "bios.bundled.absent" to "Not installed",
    "bios.reset.label" to "Restore BIOS defaults",
    "bios.reset.description" to "Back to SCPH-1001 with no forced file.",

    // --- Advanced tab ---
    "advanced.intro" to
        "Diagnostics for bug reports. Leave logging off for normal play — writing a log costs " +
        "performance.",
    "advanced.logging.label" to "Enable logging",
    "advanced.logging.description" to
        "Write the core's log to the emulator data folder. Same switch as Quiet mode, inverted.",
    "advanced.logLevel.label" to "Log level",
    "advanced.logLevel.description" to
        "Lowest severity that gets logged. Trace is everything; Fatal is almost nothing.",
    "advanced.logLevel.trace" to "Trace",
    "advanced.logLevel.debug" to "Debug",
    "advanced.logLevel.info" to "Info",
    "advanced.logLevel.warn" to "Warn",
    "advanced.logLevel.error" to "Error",
    "advanced.logLevel.fatal" to "Fatal",
    "advanced.quiet.label" to "Quiet mode",
    "advanced.quiet.description" to
        "Silence all log output regardless of the level above. The core treats this as the inverse " +
        "of Enable logging.",
    "advanced.expansionRom.label" to "Expansion ROM",
    "advanced.expansionRom.description" to
        "settings.toml [paths].expansion_rom — an optional EXP1 ROM image.",
    "advanced.defaultExe.label" to "Default PS-X executable",
    "advanced.defaultExe.description" to
        "settings.toml [paths].default_psx_exe — booted when no disc is given.",
    "advanced.settingsFile.label" to "Settings file",
    "advanced.settingsFile.description" to
        "The TOML this whole screen writes. The core re-reads it on every launch.",
    "advanced.reset.label" to "Restore advanced defaults",
    "advanced.reset.description" to "Logging off, level Info, quiet on.",

    // --- per-game override scope (badges, scope note, override panel) ---
    "scope.thisGame" to "this game",
    "scope.badge.perGame" to "Per-game ✕",
    "scope.badge.overridden" to "Overridden here ✕",
    "scope.badge.appWide" to "App-wide",
    "scope.note.perGame" to
        "Changes here apply to %s only. Each row shows whether it is pinned for this game " +
        "(Per-game) or following your global configuration (Global); tap a Per-game badge to drop " +
        "it back to the global value. The \"Restore … defaults\" row at the bottom pins the core's " +
        "own defaults to this game — to go back to following global instead, use Reset this tab in " +
        "the overrides box.",
    "scope.note.global" to
        "Global configuration — the baseline every game starts from. A game can pin its own value " +
        "for any of these from its own settings screen, and that pin wins over anything set here.",
    "scope.overrides.countOne" to "%d per-game override · %s",
    "scope.overrides.countMany" to "%d per-game overrides · %s",
    "scope.overrides.explainPerGame" to
        "These win over your global configuration for this game. Everything else follows global, " +
        "including future changes to it.",
    "scope.overrides.explainGlobal" to
        "%s pins these values, so changing them here will NOT change what that game does until the " +
        "pin is removed.",
    "scope.overrides.globalValue" to "global: %s",
    "scope.overrides.unused" to "not used by this version, safe to clear",
    "scope.overrides.noBadge" to "no badge on any row yet",
    "scope.overrides.resetTab" to "Reset this tab (%d)",
    "scope.overrides.clearAll" to "Clear all overrides",
    "scope.overrides.clearAll.body" to
        "%s will follow your global configuration for every setting.",
    "scope.notice.pinsOne" to "This game pins %d setting",
    "scope.notice.pinsMany" to "This game pins %d settings",
    "scope.notice.body" to
        "%s — these win over anything changed here, which edits your global configuration.",
    "scope.notice.useGlobal" to "Use global for this game",

    // --- PS1 pad device rows (Controls tab) ---
    "pad.analogStart.label" to "Start games in analog mode",
    "pad.analogStart.description" to
        "Report the pad as an analog DualShock from the moment a game boots, so the sticks work " +
        "without pressing ANALOG first. Real hardware starts digital — turn this off for a game " +
        "that misdetects an analog pad. The Analog (toggle) bind still switches modes while " +
        "playing. Applies on the next launch.",
    "pad.multitap4.label" to "Multitap (4 players)",
    "pad.multitap4.description" to
        "Plug a Multitap into controller port 1 so up to four controllers play at once. Extra pads " +
        "are assigned in the order they are first used. ONLY turn this on for a game that supports " +
        "it: a multitap answers with its own ID, so a game that does not know about one sees no " +
        "controller at all and nothing responds. Real hardware behaves the same way — that is why " +
        "a real Multitap has a 1-player switch. Memory cards for players 2-4 are not emulated.",

    // --- Library tab (settings.toml [library]) ---
    "library.intro" to
        "Folders scanned for discs (.cue/.bin/.iso/.img/.chd) and PS-X executables.",
    "library.allFiles.label" to "Grant all-files access",
    "library.allFiles.description" to
        "Required before the emulator can open games from your own folders.",
    "library.addFolder.label" to "Add folder…",
    "library.addFolder.description" to
        "Pick a folder; its filesystem path is stored in settings.toml.",
    "library.empty" to
        "No library folders yet. Add one above and your games appear on the home screen.",
    "library.folder" to "Folder",
    "library.scanSubfolders" to "Scan subfolders",
    "library.remove" to "Remove this folder",
    "library.noPath" to "That location has no filesystem path the emulator can open.",
)
