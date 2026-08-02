package com.armsx2.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

/**
 * A small transient banner in the TOP-LEFT — the "Welcome Back!" note shown on wake, hotkey
 * notices, and RetroAchievements notifications (game summary at boot, unlocks, sign-in trouble).
 * Android Toasts can't be reliably positioned on API 30+ (setGravity is ignored), so this is a
 * Compose overlay mounted once in [WindowImpl] so it floats above any screen (in-game or library).
 */
object WelcomeBanner {
    /** How long a message stays up when [show] is not given a duration. */
    const val DEFAULT_DURATION_MS = 2600L

    val text = mutableStateOf<String?>(null)
    // Bumped on each show() so the auto-dismiss timer restarts even for an identical message.
    val token = mutableIntStateOf(0)

    /** Dismiss delay for the message currently in [text]. Deliberately a plain var rather than
     *  Compose state: the overlay reads it inside the effect keyed on [token], which restarts
     *  after [show] has already written it, so there is nothing to observe. Written on the main
     *  thread only (see NativeApp.onAchievementNotice, which hops there first). */
    @Volatile
    var durationMs: Long = DEFAULT_DURATION_MS
        private set

    /** Show [message] for [durationMs]. RetroAchievements passes the user's configured
     *  notification duration; everything else takes the default. */
    @JvmStatic
    fun show(message: String, durationMs: Long = DEFAULT_DURATION_MS) {
        this.durationMs = if (durationMs > 0L) durationMs else DEFAULT_DURATION_MS
        text.value = message
        token.intValue += 1
    }
}

@Composable
fun WelcomeBannerOverlay(scope: BoxScope) {
    val msg = WelcomeBanner.text.value ?: return
    LaunchedEffect(WelcomeBanner.token.intValue) {
        delay(WelcomeBanner.durationMs)
        WelcomeBanner.text.value = null
    }
    with(scope) {
        Surface(
            modifier = Modifier
                .align(Alignment.TopStart)
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .padding(start = 14.dp, top = 12.dp),
            shape = RoundedCornerShape(14.dp),
            color = MaterialTheme.colorScheme.surface.copy(alpha = 0.92f),
            border = BorderStroke(1.dp, MaterialTheme.colorScheme.primary.copy(alpha = 0.5f)),
            shadowElevation = 8.dp,
        ) {
            Text(
                text = msg,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                color = MaterialTheme.colorScheme.onSurface,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}
