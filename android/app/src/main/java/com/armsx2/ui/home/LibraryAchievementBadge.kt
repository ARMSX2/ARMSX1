package com.armsx2.ui.home

import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.GameInfo
import com.armsx2.PlayTime
import com.armsx2.RaLibrary
import com.armsx2.i18n.str

@Composable
internal fun rememberLibraryAchievementProgress(game: GameInfo): PlayTime.Progress? {
    val revision = PlayTime.revision.value
    val account = RaLibrary.userName
    return remember(game.uri, game.serial, revision, account) { RaLibrary.progressFor(game) }
}

@Composable
internal fun LibraryAchievementBadge(game: GameInfo, modifier: Modifier = Modifier) {
    val progress = rememberLibraryAchievementProgress(game) ?: return
    val status = LibraryAchievementStatus.from(progress.softcore, progress.hardcore, progress.total) ?: return
    val colour = when {
        status.mastered -> Color(0xFFFFC857)
        status.completed -> Color(0xFFCBD3DC)
        else -> Color.White
    }
    val description = str("ra.library.cover.progress").format(status.earned, status.total) +
        if (status.hardcore) " · ${str("ra.mode.hardcore")}" else ""
    Surface(modifier.semantics(mergeDescendants = true) { contentDescription = description },
        shape = RoundedCornerShape(6.dp), color = Color(0xE620252C), contentColor = colour) {
        Text("🏆 ${status.label}", Modifier.padding(horizontal = 5.dp, vertical = 3.dp),
            fontSize = 10.sp, lineHeight = 13.sp, fontWeight = FontWeight.Bold,
            maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}
