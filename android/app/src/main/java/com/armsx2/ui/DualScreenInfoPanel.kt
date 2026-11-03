package com.armsx2.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.EmuState
import com.armsx2.EnglishTitles
import com.armsx2.PlayTime
import com.armsx2.runtime.MainActivityRuntime
import kotlinx.coroutines.delay

/**
 * Cover, title, play time and achievement progress for the running game, drawn on the second
 * screen UNDER the touch controls. It sits in the top-middle gap the default layout leaves between
 * the shoulder buttons and above the sticks; because it is beneath the controls and takes no input,
 * a custom layout that puts a button over it still gets every press.
 */
@Composable
fun DualScreenInfoPanel() {
    val game = MainActivityRuntime.currentGame.value ?: return
    val state = MainActivityRuntime.eState.value
    if (state != EmuState.RUNNING && state != EmuState.PAUSED) return
    // A game opened from a file link has no library serial; the core still knows it.
    val serial = game.serial?.takeIf { it.isNotBlank() }
        ?: runCatching { kr.co.iefriends.pcsx2.NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotBlank() }

    // Live play time: the stored total only grows when a session ends, so tick it here.
    val played by produceState(PlayTime.totalSecondsIncludingSession(serial), serial) {
        while (true) {
            value = PlayTime.totalSecondsIncludingSession(serial)
            delay(PLAY_TIME_TICK_MS)
        }
    }
    PlayTime.revision.value // re-read achievements when the runtime records new figures
    val achievements = PlayTime.achievements(serial)

    BoxWithConstraints(Modifier.fillMaxSize()) {
        // Keep clear of the default shoulder buttons and D-pad / face clusters at the sides.
        val panelWidth = (maxWidth * 0.46f).coerceAtMost(300.dp)
        val coverHeight = (maxHeight * 0.34f).coerceIn(80.dp, 170.dp)
        Column(
            Modifier
                .align(Alignment.TopCenter)
                .padding(top = 20.dp)
                .widthIn(max = panelWidth)
                .clip(RoundedCornerShape(16.dp))
                .background(Color(0x33FFFFFF))
                .padding(horizontal = 14.dp, vertical = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Top,
        ) {
            Box(Modifier.height(coverHeight).width(coverHeight * COVER_ASPECT)) {
                com.armsx2.ui.common.GameCoverArt(game, Modifier.fillMaxSize())
            }
            Spacer(Modifier.height(10.dp))
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                color = Color.White,
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            val playedText = PlayTime.formatPlayed(played)
            if (playedText.isNotEmpty()) {
                Spacer(Modifier.height(4.dp))
                Text("⏱ $playedText", color = Color(0xCCFFFFFF), fontSize = 13.sp)
            }
            if (achievements != null) {
                Spacer(Modifier.height(2.dp))
                val hardcore = if (achievements.hardcore > 0) "  ·  HC ${achievements.hardcore}" else ""
                Text(
                    "🏆 ${achievements.softcore} / ${achievements.total}$hardcore",
                    color = Color(0xCCFFFFFF),
                    fontSize = 13.sp,
                )
            }
        }
    }
}

private const val PLAY_TIME_TICK_MS = 30_000L

/** Box art is the 3D jewel-case render the library uses, which is square; a narrower box crops it. */
private const val COVER_ASPECT = 1f

/**
 * Out of a game: the library's highlighted game's box art, as large as fits, centred on the second screen. Follows the selector
 * as the D-pad moves it through the grid or the Recently Played row; blank when nothing is
 * highlighted (settings, manager screens, an empty library).
 */
@Composable
fun DualScreenLibraryCover() {
    val game = com.armsx2.ui.home.HomeInputController.highlightedGame() ?: return
    BoxWithConstraints(Modifier.fillMaxSize()) {
        // As big as the screen allows: ~80% of the height, never wider than the screen.
        val coverHeight = minOf(maxHeight * 0.80f, maxWidth * 0.9f).coerceAtLeast(120.dp)
        val titleWidth = maxWidth * 0.9f
        Column(
            Modifier.align(Alignment.Center),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // Keyed on the game so a new cover loads fresh instead of cross-fading the old one.
            androidx.compose.runtime.key(game.uri) {
                Box(Modifier.height(coverHeight).width(coverHeight * COVER_ASPECT)) {
                    com.armsx2.ui.common.GameCoverArt(game, Modifier.fillMaxSize())
                }
            }
            Spacer(Modifier.height(10.dp))
            Text(
                game.displayTitle(EnglishTitles.enabled.value),
                color = Color.White,
                fontSize = 18.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.widthIn(max = titleWidth),
            )
        }
    }
}
