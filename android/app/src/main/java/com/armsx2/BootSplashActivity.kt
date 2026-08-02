package com.armsx2

import android.content.Intent
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.media.MediaPlayer
import android.net.Uri
import android.os.Bundle
import android.view.Surface
import android.view.TextureView
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * Boot splash: plays the bundled ARMSX1 intro (res/raw/boot_intro.mp4) once per process, then
 * hands off to Main. Tapping, Back, a hard timeout, and any playback error all fall through to
 * the app, so a bad codec or a slow decode can never strand the user on a black screen.
 *
 * Opt-out lives in the "ui.bootLogo" preference (App settings, default on). It is read straight
 * off SharedPreferences here because this runs before Compose — [com.armsx2.ui.theme.BootLogoPreferences]
 * only mirrors the same key for the toggle.
 *
 * PLAYBACK: MediaPlayer onto a TextureView, deliberately NOT the obvious VideoView. VideoView is
 * a SurfaceView, and on this window the SurfaceView never got a surface — surfaceCreated never
 * fired, so VideoView.openVideo() kept early-returning and MediaPlayer was never even asked to
 * open the file. The symptom was pure black with NO error callback until the timeout, which
 * reads exactly like a bad video file and is why this is spelled out here. A TextureView draws
 * into the hardware-accelerated view hierarchy and needs no compositor layer of its own.
 *
 * The intro is a 1:1 square video and the splash is full-screen, so [applyCoverTransform] scales
 * it to cover the landscape window at its true aspect — cropping the long axis rather than
 * letterboxing, and never stretching.
 */
class BootSplashActivity : ComponentActivity() {
    private var launchedMain = false
    private var rootView: View? = null
    private var player: MediaPlayer? = null
    private var surface: Surface? = null
    private val timeoutRunnable = Runnable {
        android.util.Log.w(LOG_TAG, "intro timed out; continuing to the library")
        launchMainAndFinish()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // The manifest theme (Theme.ARMSX2.Boot) paints the window black. That is right for the
        // logo-disabled path below, which finishes without ever inflating a layout. When the
        // intro DOES play, the layout's own white background takes over for the frame or two
        // before the first decoded frame — see activity_boot_splash.xml.
        super.onCreate(savedInstanceState)
        StartupTrace.mark("splash.onCreate enter")
        applyImmersiveUi()

        // This getBoolean is the first read of the ARMSX2 prefs file, and it BLOCKS until the XML
        // is parsed. Pasx2Application's warm-up worker normally has it loaded by now.
        val prefs = getSharedPreferences("ARMSX2", MODE_PRIVATE)
        val wantsLogo = prefs.getBoolean("ui.bootLogo", true)
        StartupTrace.mark("splash.prefs")
        if (!wantsLogo || playedThisProcess) {
            launchMainAndFinish()
            return
        }
        playedThisProcess = true

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = launchMainAndFinish()
        })

        setContentView(R.layout.activity_boot_splash)
        rootView = findViewById(R.id.boot_splash_root)
        val textureView = findViewById<TextureView?>(R.id.boot_splash_video)
        rootView?.apply {
            setOnClickListener { launchMainAndFinish() }
            postDelayed(timeoutRunnable, HARD_TIMEOUT_MS)
        }
        if (textureView == null) {
            launchMainAndFinish()
            return
        }
        textureView.setOnClickListener { launchMainAndFinish() }
        textureView.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                startPlayback(textureView, texture)
            }

            override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
                player?.let { applyCoverTransform(textureView, it.videoWidth, it.videoHeight) }
            }

            override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                releasePlayer()
                return true
            }

            override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
        }
    }

    private fun startPlayback(textureView: TextureView, texture: SurfaceTexture) {
        if (player != null) return
        val uri = Uri.parse("android.resource://$packageName/${R.raw.boot_intro}")
        surface = Surface(texture)
        player = MediaPlayer().apply {
            setOnPreparedListener { mp ->
                android.util.Log.i(LOG_TAG, "intro prepared ${mp.videoWidth}x${mp.videoHeight}")
                applyCoverTransform(textureView, mp.videoWidth, mp.videoHeight)
                mp.start()
            }
            setOnCompletionListener { launchMainAndFinish() }
            // Log before falling through. Swallowing this silently is how an intro that no longer
            // decodes on some device looks EXACTLY like a working one: black screen, no error,
            // timeout advances to the library. what/extra name the reason — MEDIA_ERROR_UNSUPPORTED
            // (-1010) is the codec refusing the file's profile/level.
            setOnErrorListener { _, what, extra ->
                android.util.Log.e(LOG_TAG, "intro playback failed what=$what extra=$extra")
                launchMainAndFinish()
                true
            }
        }
        val started = runCatching {
            player?.apply {
                setSurface(surface)
                setDataSource(this@BootSplashActivity, uri)
                prepareAsync()
            }
        }
        if (started.isFailure) {
            android.util.Log.e(LOG_TAG, "could not open the intro", started.exceptionOrNull())
            launchMainAndFinish()
        }
    }

    /**
     * Scale the video to COVER the whole window without distorting it — the splash is meant to be
     * full-screen. A TextureView stretches its content to its own bounds by default, which would
     * smear the square intro across a landscape screen; this matrix restores the true aspect and
     * centres it, overflowing (and so cropping) the long axis rather than leaving bars.
     *
     * Cropping is safe for this asset: the logo sits well inside the square, so at 16:9 the crop
     * takes only empty backdrop. A future intro that fills its frame edge-to-edge would lose those
     * edges here.
     */
    private fun applyCoverTransform(textureView: TextureView, videoWidth: Int, videoHeight: Int) {
        val viewWidth = textureView.width.toFloat()
        val viewHeight = textureView.height.toFloat()
        if (viewWidth <= 0f || viewHeight <= 0f || videoWidth <= 0 || videoHeight <= 0) return

        val scale = maxOf(viewWidth / videoWidth, viewHeight / videoHeight)
        val drawnWidth = videoWidth * scale
        val drawnHeight = videoHeight * scale
        textureView.setTransform(
            Matrix().apply {
                setScale(drawnWidth / viewWidth, drawnHeight / viewHeight)
                postTranslate((viewWidth - drawnWidth) / 2f, (viewHeight - drawnHeight) / 2f)
            },
        )
    }

    private fun releasePlayer() {
        player?.runCatching { release() }
        player = null
        surface?.release()
        surface = null
    }

    override fun onDestroy() {
        rootView?.removeCallbacks(timeoutRunnable)
        releasePlayer()
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) applyImmersiveUi()
    }

    private fun applyImmersiveUi() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun launchMainAndFinish() {
        if (launchedMain) return
        launchedMain = true
        StartupTrace.mark("splash.handoff")
        rootView?.removeCallbacks(timeoutRunnable)
        val launch = Intent(this, Main::class.java)
        intent?.let { source ->
            launch.action = source.action
            if (source.data != null || source.type != null) launch.setDataAndType(source.data, source.type)
            source.categories?.forEach(launch::addCategory)
            source.extras?.let(launch::putExtras)
            source.clipData?.let(launch::setClipData)
            launch.addFlags(
                source.flags and (
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
                        Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
                    ),
            )
        }
        launch.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        startActivity(launch)
        finish()
        // overrideActivityTransition is API 34 (Android 14); on 13 and below it
        // throws NoSuchMethodError (crashed the splash on the Retroid). Fall back to
        // the deprecated overridePendingTransition there.
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, 0, 0)
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(0, 0)
        }
    }

    private companion object {
        const val LOG_TAG = "ARMSX-Splash"
        var playedThisProcess = false

        /** The intro is ~4s. This only exists so a device that stalls on decode still gets in. */
        const val HARD_TIMEOUT_MS = 8000L
    }
}
