package com.armsx2.runtime

import android.app.Presentation
import android.content.Context
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.ViewCompositionStrategy
import androidx.compose.ui.unit.LayoutDirection
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

/**
 * Dual-screen play for handhelds with a second built-in panel (AYN Thor) or an attached display.
 *
 * The Activity, the VM and the single [EmulationSurface] never move or multiply. A [Presentation]
 * on the other display hosts either the touch controls (game stays on the Activity's screen) or the
 * game's SurfaceView itself (controls stay with the Activity). Off — the default — and any time no
 * second display is attached, nothing here creates a window, so single-screen play is unchanged.
 */
object DualScreen {
    enum class Mode(val key: String) {
        OFF("off"),
        /** Game on the screen the app is on, touch controls on the other one. */
        GAME_MAIN("game_main"),
        /** Game on the other screen, touch controls with the app. */
        GAME_SECOND("game_second");

        companion object {
            fun fromKey(key: String?): Mode = entries.firstOrNull { it.key == key } ?: OFF
        }
    }

    enum class Placement { NONE, CONTROLS_ON_SECOND, GAME_ON_SECOND, LIBRARY_ON_SECOND, SETTINGS_ON_SECOND }

    private const val KEY_MODE = "ui.dualScreenMode"
    private const val KEY_INFO_PANEL = "ui.dualScreenInfoPanel"
    private const val KEY_LIBRARY_ART = "ui.dualScreenLibraryArt"
    private const val KEY_Y_SETTINGS = "ui.dualScreenYSettings"

    val mode = mutableStateOf(Mode.OFF)

    /** Game info drawn between the touch controls when they are on the second screen. */
    val infoPanel = mutableStateOf(true)

    /** Out of a game: the library's highlighted game's box art on the second screen. */
    val libraryArt = mutableStateOf(true)

    /** Y in the library opens Settings on the second screen instead of the game's menu. */
    val ySettings = mutableStateOf(false)

    /** Settings is being shown on the second screen (asked for with Y); B or Back closes it. */
    val settingsOnSecondary = mutableStateOf(false)

    /** A second display is attached and usable; kept current by the controller. */
    val hasSecondDisplay = mutableStateOf(false)

    /** True when Y should open Settings on the other screen right now. */
    fun ySettingsAvailable(): Boolean =
        ySettings.value && mode.value != Mode.OFF && hasSecondDisplay.value

    /** The main window's copy of the touch controls stands down while this is true. */
    val controlsOnSecondary = mutableStateOf(false)

    /** The main window must not compose the game SurfaceView while this is true. */
    val gameOnSecondary = mutableStateOf(false)

    fun load() {
        mode.value = runCatching { Mode.fromKey(MainActivityRuntime.prefs.getString(KEY_MODE, null)) }
            .getOrDefault(Mode.OFF)
        infoPanel.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_INFO_PANEL, true) }
            .getOrDefault(true)
        libraryArt.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_LIBRARY_ART, true) }
            .getOrDefault(true)
        ySettings.value = runCatching { MainActivityRuntime.prefs.getBoolean(KEY_Y_SETTINGS, false) }
            .getOrDefault(false)
        com.armsx2.core.Ps1GameMedia.load()
    }

    fun setYSettings(enabled: Boolean) {
        ySettings.value = enabled
        if (!enabled) settingsOnSecondary.value = false
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY_Y_SETTINGS, enabled).apply() }
    }

    fun setLibraryArt(show: Boolean) {
        libraryArt.value = show
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY_LIBRARY_ART, show).apply() }
    }

    fun setInfoPanel(show: Boolean) {
        infoPanel.value = show
        runCatching { MainActivityRuntime.prefs.edit().putBoolean(KEY_INFO_PANEL, show).apply() }
    }

    fun setMode(next: Mode) {
        mode.value = next
        runCatching { MainActivityRuntime.prefs.edit().putString(KEY_MODE, next.key).apply() }
    }

    /**
     * What the second display should carry right now. Pure so it can be unit-tested.
     *
     * The CTR race companion owns the second screen whenever it is up; dual-screen yields to it
     * rather than stacking a second Presentation on the same display.
     */
    fun placement(
        mode: Mode,
        hasSecondDisplay: Boolean,
        inGame: Boolean,
        touchEditMode: Boolean,
        companionShowing: Boolean,
        libraryArt: Boolean = false,
        settingsOpen: Boolean = false,
    ): Placement = when {
        mode == Mode.OFF || !hasSecondDisplay || companionShowing -> Placement.NONE
        !inGame && settingsOpen -> Placement.SETTINGS_ON_SECOND
        mode == Mode.GAME_MAIN && (inGame || touchEditMode) -> Placement.CONTROLS_ON_SECOND
        mode == Mode.GAME_SECOND && inGame -> Placement.GAME_ON_SECOND
        !inGame && !touchEditMode && libraryArt -> Placement.LIBRARY_ON_SECOND
        else -> Placement.NONE
    }
}

/** Owns the second-screen [Presentation]; one per Activity, main thread only. */
class DualScreenController(private val activity: ComponentActivity) : DisplayManager.DisplayListener {
    private val displays = activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val main = Handler(Looper.getMainLooper())
    private var started = false
    private var closed = false

    private var presentation: Presentation? = null
    private var placement = DualScreen.Placement.NONE
    private var displayId: Int? = null
    private var surfaceHost: FrameLayout? = null
    private var hostedSurface: EmulationSurface? = null

    private var inGame = false
    private var touchEditMode = false
    private var companionShowing = false

    fun start() {
        if (started || closed) return
        started = true
        displays.registerDisplayListener(this, main)
        refresh()
    }

    fun stop() {
        if (!started) return
        started = false
        runCatching { displays.unregisterDisplayListener(this) }
        teardown()
    }

    fun close() {
        if (closed) return
        stop()
        closed = true
    }

    /** Fed from the Activity's composition whenever one of these inputs changes. */
    fun update(inGame: Boolean, touchEditMode: Boolean, companionShowing: Boolean) {
        this.inGame = inGame
        this.touchEditMode = touchEditMode
        this.companionShowing = companionShowing
        // Settings on the second screen belongs to the library; a game starting closes it.
        if (inGame) DualScreen.settingsOnSecondary.value = false
        refresh()
    }

    /**
     * Called by the composition once it has stopped composing the SurfaceView itself (it does
     * that while [DualScreen.gameOnSecondary] is true), so the view is free to move here.
     */
    fun hostSurface(surface: EmulationSurface) {
        val host = surfaceHost
        if (host == null || placement != DualScreen.Placement.GAME_ON_SECOND) return
        if (surface.parent === host) { hostedSurface = surface; return }
        hostedSurface?.let { old -> if (old.parent === host) host.removeView(old) }
        (surface.parent as? ViewGroup)?.removeView(surface)
        host.addView(surface, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        hostedSurface = surface
    }

    fun releaseSurface(surface: EmulationSurface) {
        val host = surfaceHost
        if (host != null && surface.parent === host) host.removeView(surface)
        if (hostedSurface === surface) hostedSurface = null
    }

    private fun activityDisplayId(): Int? = runCatching {
        activity.window?.decorView?.display?.displayId
            ?: if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) activity.display?.displayId
            else @Suppress("DEPRECATION") activity.windowManager.defaultDisplay?.displayId
    }.getOrNull()

    private fun secondDisplay(): Display? = runCatching {
        val own = activityDisplayId() ?: Display.DEFAULT_DISPLAY
        displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
            .firstOrNull { it.displayId != own && it.isValid && it.state != Display.STATE_OFF }
    }.getOrNull()

    private fun refresh() {
        if (!started || closed || activity.isFinishing || activity.isDestroyed) { teardown(); return }
        val display = secondDisplay()
        DualScreen.hasSecondDisplay.value = display != null
        if (display == null) DualScreen.settingsOnSecondary.value = false
        val wanted = DualScreen.placement(
            DualScreen.mode.value, display != null, inGame, touchEditMode, companionShowing,
            DualScreen.libraryArt.value, DualScreen.settingsOnSecondary.value)
        if (wanted == DualScreen.Placement.NONE || display == null) { teardown(); return }
        if (wanted == placement && display.displayId == displayId && presentation?.isShowing == true) return
        teardown()
        show(display, wanted)
    }

    private fun show(display: Display, wanted: DualScreen.Placement) {
        val host = if (wanted == DualScreen.Placement.GAME_ON_SECOND)
            FrameLayout(activity).apply { setBackgroundColor(Color.BLACK) } else null
        val next = object : Presentation(activity, display) {
            override fun onCreate(savedInstanceState: Bundle?) {
                super.onCreate(savedInstanceState)
                // Keys stay with the Activity; this window only takes touches.
                window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
                @Suppress("DEPRECATION")
                window?.decorView?.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                window?.decorView?.let { attachOwners(it) }
                setContentView(host ?: if (wanted == DualScreen.Placement.SETTINGS_ON_SECOND)
                    composeView(context) {
                        // The pad drives this Settings through the Activity's key routing, but the
                        // window itself is never focused, so the selection outline (which hides in
                        // unfocused windows) must be told it is the one being driven.
                        val base = androidx.compose.ui.platform.LocalWindowInfo.current
                        val driven = androidx.compose.runtime.remember(base) {
                            object : androidx.compose.ui.platform.WindowInfo by base {
                                override val isWindowFocused: Boolean get() = true
                            }
                        }
                        CompositionLocalProvider(androidx.compose.ui.platform.LocalWindowInfo provides driven) {
                        com.armsx2.ui.settingshub.SettingsScreen(
                            initialCategory = com.armsx2.navigation.SettingsCategory.General,
                            game = null,
                            onBack = { DualScreen.settingsOnSecondary.value = false },
                        )
                        }
                    }
                else if (wanted == DualScreen.Placement.LIBRARY_ON_SECOND)
                    composeView(context) { com.armsx2.ui.DualScreenLibraryCover() }
                else composeView(context) {
                    // Under the controls, and out of the way while the layout is being edited.
                    if (DualScreen.infoPanel.value && !com.armsx2.ui.touch.TouchControls.editMode.value) {
                        com.armsx2.ui.DualScreenInfoPanel()
                    }
                    com.armsx2.ui.touch.TouchControlsOverlay()
                })
            }
        }
        next.setOnDismissListener { if (presentation === next) onLost() }
        try {
            next.show()
        } catch (_: RuntimeException) {
            // InvalidDisplayException / BadTokenException: the display left mid-show, or the
            // Activity is going away. Single-screen play simply continues.
            runCatching { next.dismiss() }
            return
        }
        presentation = next
        placement = wanted
        displayId = display.displayId
        surfaceHost = host
        // Flip these only once the window exists: the composition reacts by dropping its own copy.
        when (wanted) {
            DualScreen.Placement.CONTROLS_ON_SECOND -> DualScreen.controlsOnSecondary.value = true
            DualScreen.Placement.LIBRARY_ON_SECOND, DualScreen.Placement.SETTINGS_ON_SECOND -> Unit
            DualScreen.Placement.GAME_ON_SECOND -> {
                DualScreen.gameOnSecondary.value = true
                // A surface with no parent was freed by an earlier teardown that the composition
                // never saw (the flag went false and back to true between frames), so nothing
                // will ask for it again. One still inside AndroidView is handed over by the
                // composition instead.
                MainActivityRuntime.surface.value?.let { if (it.parent == null) hostSurface(it) }
            }
            DualScreen.Placement.NONE -> Unit
        }
    }

    private fun attachOwners(view: View) {
        view.setViewTreeLifecycleOwner(activity)
        view.setViewTreeViewModelStoreOwner(activity)
        view.setViewTreeSavedStateRegistryOwner(activity)
        view.setViewTreeOnBackPressedDispatcherOwner(activity)
    }

    private fun composeView(context: Context, content: @androidx.compose.runtime.Composable () -> Unit): View =
        ComposeView(context).apply {
            attachOwners(this)
            setViewCompositionStrategy(ViewCompositionStrategy.DisposeOnDetachedFromWindow)
            setContent {
                com.armsx2.ui.theme.Armsx2Theme(forceDark = true) {
                    CompositionLocalProvider(
                    LocalLayoutDirection provides LayoutDirection.Ltr,
                    // This window's context is a display context, not the Activity: file pickers and
                    // back handlers in Settings must still reach the Activity's registries.
                    androidx.activity.compose.LocalActivityResultRegistryOwner provides activity,
                    androidx.activity.compose.LocalOnBackPressedDispatcherOwner provides activity,
                ) {
                        Box(Modifier.fillMaxSize().background(androidx.compose.ui.graphics.Color.Black)) {
                            content()
                        }
                    }
                }
            }
        }

    /** The display went away (or Android dismissed the window): hand everything back, then retry. */
    private fun onLost() {
        teardown()
        main.post { refresh() }
    }

    private fun teardown() {
        val p = presentation
        presentation = null
        // Free the SurfaceView BEFORE the composition is told it may compose it again: a view that
        // still has a parent would make AndroidView throw when it re-adds it.
        surfaceHost?.removeAllViews()
        surfaceHost = null
        hostedSurface = null
        placement = DualScreen.Placement.NONE
        displayId = null
        if (p != null) runCatching { p.dismiss() }
        DualScreen.controlsOnSecondary.value = false
        DualScreen.gameOnSecondary.value = false
    }

    override fun onDisplayAdded(displayId: Int) = refresh()

    override fun onDisplayRemoved(displayId: Int) {
        if (displayId == this.displayId) teardown()
        refresh()
    }

    override fun onDisplayChanged(displayId: Int) {
        if (displayId == this.displayId || displayId == activityDisplayId()) refresh()
    }
}
