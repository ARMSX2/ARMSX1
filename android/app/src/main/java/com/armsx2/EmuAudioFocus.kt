package com.armsx2

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.runtime.MainActivityRuntime

/**
 * Audio focus for the EMULATOR's own output stream.
 *
 * ★ The core's stream is created by SDL, deep in native code (an OpenSL ES buffer-queue player),
 * and it never went through Android's focus protocol at all — `dumpsys audio` showed an empty
 * focus stack with our player sitting at `state:started`. So nothing told the system we were
 * playing, and nothing yielded when something else needed the output.
 *
 * [LibraryMusic] and [PauseMusic] own focus for the FRONT-END's music; this owns it for the game.
 * They never overlap: the library track only plays with no VM running, and the pause track only
 * while the VM is parked behind the overlay.
 *
 * Deliberately NOT requested when the core's audio is muted — there is no reason to stop whatever
 * the user is listening to for a stream that is going to be silent anyway.
 */
object EmuAudioFocus {

    private var focusRequest: AudioFocusRequest? = null
    private var held = false

    /** Set only when THIS object paused the VM. A user pause must never be auto-resumed. */
    private var pausedForFocus = false

    /** False between onPause and onResume. A GAIN that lands while we are off-screen must not
     *  restart the game behind the lock screen — which is the whole bug this fixes. */
    @Volatile
    private var foreground = true

    fun setForeground(value: Boolean) {
        foreground = value
        if (!value) pausedForFocus = false
    }

    /** Take focus for a game that is starting or coming back to the foreground. */
    fun acquire(context: Context) {
        if (held) return
        // active(): the running game's merged view, so a game that mutes itself doesn't take focus.
        val muted = runCatching { Ps1SettingsStore.active(context).audioMuted }.getOrDefault(false)
        if (muted) return
        val am = audioManager(context) ?: return
        held = requestFocus(am)
    }

    /** Give focus back — backgrounding, or the game stopping. Safe to call when we hold none. */
    fun release(context: Context) {
        pausedForFocus = false
        if (!held) return
        held = false
        audioManager(context)?.let { abandonFocus(it) }
    }

    private fun audioManager(context: Context): AudioManager? =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            // Permanent — a media app took the output for good. Park the game and drop the
            // request; it is not coming back on its own.
            AudioManager.AUDIOFOCUS_LOSS -> {
                held = false
                focusRequest = null
                pauseForFocus()
                pausedForFocus = false
            }
            // Transient (a call, a voice assistant). Park the game and arm the self-resume.
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> pauseForFocus()
            // ...but NOT _CAN_DUCK. That is a notification blip; the system lowers our volume by
            // itself and pausing the game for it would be worse than the duck.
            AudioManager.AUDIOFOCUS_GAIN -> {
                if (pausedForFocus) {
                    pausedForFocus = false
                    if (foreground && MainActivityRuntime.eState.value == EmuState.PAUSED) {
                        MainActivityRuntime.resume()
                    }
                }
            }
        }
    }

    private fun pauseForFocus() {
        if (MainActivityRuntime.eState.value != EmuState.RUNNING) return
        pausedForFocus = true
        MainActivityRuntime.pause()
    }

    private fun requestFocus(am: AudioManager): Boolean {
        val granted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setOnAudioFocusChangeListener(focusListener)
                .build()
            focusRequest = req
            am.requestAudioFocus(req)
        } else {
            @Suppress("DEPRECATION")
            am.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
        }
        return granted == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun abandonFocus(am: AudioManager) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { runCatching { am.abandonAudioFocusRequest(it) } }
            focusRequest = null
        } else {
            @Suppress("DEPRECATION")
            runCatching { am.abandonAudioFocus(focusListener) }
        }
    }
}
