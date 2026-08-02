package com.armsx2.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudOff
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.Leaderboard
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.WorkspacePremium
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import kotlinx.coroutines.delay

/**
 * The RetroAchievements notification stack — the "signed in as you", "this game, N of M
 * achievements" and "achievement unlocked" toasts, each with its RA artwork.
 *
 * Fed from native by `NativeApp.onAchievementNotice`, which is called by the notice pump in
 * `frontend/achievements.cpp` (see `armsx_ach_notify_fn` in `frontend/achievements.h` for the
 * contract). Rendered by [RaToastOverlay], mounted once in [WindowImpl] so it draws over live
 * gameplay as well as over the library — the PS1 core runs in-process into the Compose activity's
 * SurfaceView, so a Box-scoped overlay there composites straight onto the game.
 *
 * Distinct from [WelcomeBanner] on purpose. That is one transient slot for a single line of text;
 * a RetroAchievements session produces bursts (a sign-in and a game summary a frame apart, or a
 * combo of unlocks) and each one carries an image, so they need a stack that can hold several at
 * once. Routing RA through the single-slot banner is exactly why a sign-in note used to be
 * suppressed in native code: it would have been overwritten before it could be read.
 */
object RaToasts {
    /** Mirrors the ARMSX_ACH_NOTICE_* enum in `frontend/achievements.h`. */
    const val KIND_INFO = 0
    const val KIND_LOGIN = 1
    const val KIND_GAME = 2
    const val KIND_UNLOCK = 3
    const val KIND_MASTERY = 4
    const val KIND_LEADERBOARD = 5
    const val KIND_ERROR = 6

    /** Beyond this the stack stops being readable and starts being a wall. The native pump already
     *  spaces releases out; this is the backstop for a game that unlocks a whole set at once. */
    const val MAX_VISIBLE = 3

    private const val DEFAULT_DURATION_MS = 5_000L

    data class Toast(
        val id: Long,
        val key: String,
        val kind: Int,
        val title: String,
        val detail: String,
        val imageUrl: String,
        val durationMs: Long,
    )

    private var nextId = 1L

    /** Newest last, so a bottom-anchored column grows away from the corner. Main thread only. */
    val toasts = mutableStateListOf<Toast>()

    /**
     * Post (or replace) a toast. Main thread only — `NativeApp.onAchievementNotice` hops there
     * before calling this.
     *
     * A [key] that matches a toast already on screen replaces it **in place**, keeping its slot and
     * restarting its timer. That is what stops a leaderboard attempt (start, then submitted, then
     * the server's ranking) from stacking three toasts for one run, and it is why the native side
     * keys unlocks per achievement — those must stack.
     */
    @JvmStatic
    fun show(kind: Int, key: String, title: String, detail: String, imageUrl: String, durationMs: Int) {
        if (title.isEmpty()) return

        val toast = Toast(
            id = nextId++,
            key = key.ifEmpty { "ra_notice" },
            kind = kind,
            title = title,
            detail = detail,
            imageUrl = imageUrl,
            durationMs = if (durationMs > 0) durationMs.toLong() else DEFAULT_DURATION_MS,
        )

        val existing = toasts.indexOfFirst { it.key == toast.key }
        if (existing >= 0) {
            toasts[existing] = toast
            return
        }

        // Drop the oldest rather than refuse the newest: the one that has been up longest has
        // already had its time, and a swallowed unlock is the failure that matters here.
        while (toasts.size >= MAX_VISIBLE) toasts.removeAt(0)
        toasts.add(toast)
    }

    fun dismiss(id: Long) {
        toasts.removeAll { it.id == id }
    }
}

/** Caption, accent colour and fallback icon for a notice kind. */
private data class RaToastStyle(val label: String, val accent: Color, val icon: ImageVector)

@Composable
private fun styleFor(kind: Int): RaToastStyle {
    val scheme = MaterialTheme.colorScheme
    // RetroAchievements' own gold, for the two kinds that are an accomplishment. Everything else
    // follows the app theme so the toast does not shout about routine state changes.
    val gold = Color(0xFFCC9900)
    return when (kind) {
        RaToasts.KIND_LOGIN -> RaToastStyle("RetroAchievements", scheme.primary, Icons.Filled.Person)
        RaToasts.KIND_GAME -> RaToastStyle("Achievements", scheme.primary, Icons.Filled.SportsEsports)
        RaToasts.KIND_UNLOCK -> RaToastStyle("Achievement unlocked", gold, Icons.Filled.EmojiEvents)
        RaToasts.KIND_MASTERY -> RaToastStyle("Completed", gold, Icons.Filled.WorkspacePremium)
        RaToasts.KIND_LEADERBOARD -> RaToastStyle("Leaderboard", scheme.tertiary, Icons.Filled.Leaderboard)
        RaToasts.KIND_ERROR -> RaToastStyle("RetroAchievements", scheme.error, Icons.Filled.ErrorOutline)
        else -> RaToastStyle("RetroAchievements", scheme.primary, Icons.Filled.CloudOff)
    }
}

/**
 * Draws the [RaToasts] stack in the TOP-LEFT, growing downward.
 *
 * Top-left is where PCSX2 and the RetroAchievements clients put these, so it is where a user
 * coming from those looks. It shares the corner with [WelcomeBanner], which is why the stack drops
 * below it whenever that banner is up rather than drawing on top of it. The other corners are
 * taken: [GameOsd] owns top-right and top-centre, and the bottom two are under the touch controls.
 *
 * Like [GameOsd] this is a plain [Box]/[Column] with no gesture modifiers, so it is invisible to
 * the touch dispatcher and presses fall straight through to whatever is underneath.
 */
@Composable
fun RaToastOverlay(scope: BoxScope) {
    if (RaToasts.toasts.isEmpty()) return

    // Clear the single-slot banner when both want the corner at once.
    val bannerUp = WelcomeBanner.text.value != null
    val topPadding = if (bannerUp) 68.dp else 12.dp

    with(scope) {
        Column(
            modifier = Modifier
                .align(Alignment.TopStart)
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .padding(start = 14.dp, top = topPadding, end = 14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // Snapshot the list so a native post mid-composition cannot mutate what is being
            // iterated. toasts is a SnapshotStateList; toList() is the cheap, safe read.
            RaToasts.toasts.toList().forEach { toast ->
                key(toast.id) { RaToastCard(toast) }
            }
        }
    }
}

@Composable
private fun RaToastCard(toast: RaToasts.Toast) {
    val style = styleFor(toast.kind)
    var visible by remember { mutableStateOf(false) }

    LaunchedEffect(toast.id) {
        visible = true
        delay(toast.durationMs)
        visible = false
        // Let the fade-out play before the entry leaves the list.
        delay(220)
        RaToasts.dismiss(toast.id)
    }

    AnimatedVisibility(
        visible = visible,
        enter = fadeIn() + slideInHorizontally { -it / 3 },
        exit = fadeOut() + shrinkVertically(),
    ) {
        Row(
            modifier = Modifier
                .widthIn(max = 380.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(MaterialTheme.colorScheme.surface.copy(alpha = 0.93f))
                .border(1.dp, style.accent.copy(alpha = 0.55f), RoundedCornerShape(14.dp))
                .padding(horizontal = 12.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            RaToastBadge(toast.imageUrl, style)

            Column(verticalArrangement = Arrangement.spacedBy(1.dp)) {
                Text(
                    text = style.label,
                    color = style.accent,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                )
                Text(
                    text = toast.title,
                    color = MaterialTheme.colorScheme.onSurface,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (toast.detail.isNotEmpty()) {
                    Text(
                        text = toast.detail,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.78f),
                        fontSize = 12.sp,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
        }
    }
}

/**
 * The badge / avatar / box art. The URL comes straight from rc_client and is fetched by Coil,
 * which already has the app-wide `files/cover_cache` disk cache behind it (see
 * `Pasx2Application.newImageLoader`) — so RA art shares one downloader and one cache with the
 * library covers instead of introducing a second pipeline.
 *
 * The image is never waited on: the toast shows immediately with the kind's icon and the artwork
 * swaps in when it arrives, which for a cached badge is the same frame.
 */
@Composable
private fun RaToastBadge(imageUrl: String, style: RaToastStyle) {
    Box(
        modifier = Modifier
            .size(44.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(style.accent.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            imageVector = style.icon,
            contentDescription = null,
            tint = style.accent,
            modifier = Modifier.size(22.dp),
        )
        if (imageUrl.isNotEmpty()) {
            AsyncImage(
                model = imageUrl,
                contentDescription = null,
                modifier = Modifier.size(44.dp).clip(RoundedCornerShape(10.dp)),
                contentScale = ContentScale.Crop,
            )
        }
    }
}
