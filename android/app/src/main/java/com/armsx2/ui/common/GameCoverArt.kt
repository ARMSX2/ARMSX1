package com.armsx2.ui.common

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import coil.size.Precision
import com.armsx2.CustomCovers
import com.armsx2.GameInfo

@Composable
fun GameCoverArt(game: GameInfo, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val customCover = remember(game.uri, CustomCovers.version.value) {
        CustomCovers.fileFor(context, game)
    }
    FallbackCoverImage(
        models = listOfNotNull(customCover) + game.coverUrls,
        contentDescription = game.title,
        modifier = modifier.clip(RoundedCornerShape(14.dp)),
        contentScale = ContentScale.Crop,
        crossfade = true,
    ) { GameCoverPlaceholder(game.title, game.serial) }
}

/** Try each candidate once; reset when the game, custom cover or style changes. */
@Composable
fun FallbackCoverImage(
    models: List<Any>,
    contentDescription: String,
    modifier: Modifier = Modifier,
    contentScale: ContentScale = ContentScale.Fit,
    crossfade: Boolean = false,
    placeholder: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val index = remember(models) { mutableIntStateOf(0) }
    val attempted = index.intValue
    val model = models.getOrNull(attempted)
    val request = remember(context, model, crossfade) {
        ImageRequest.Builder(context).data(model).size(500, 500)
            .precision(Precision.INEXACT).allowHardware(true).crossfade(crossfade).build()
    }
    SubcomposeAsyncImage(
        model = request,
        contentDescription = contentDescription,
        modifier = modifier,
        contentScale = contentScale,
        onError = {
            if (index.intValue == attempted && attempted < models.lastIndex) {
                index.intValue = attempted + 1
            }
        },
        loading = { placeholder() },
        error = { placeholder() },
    )
}

@Composable
fun GameCoverPlaceholder(title: String, serial: String? = null, modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxSize()
            .background(
                Brush.linearGradient(
                    listOf(
                        MaterialTheme.colorScheme.primaryContainer,
                        MaterialTheme.colorScheme.surfaceVariant,
                    ),
                ),
            ),
        contentAlignment = Alignment.Center,
    ) {
        Column(Modifier.padding(10.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Text(
                text = title,
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
                textAlign = TextAlign.Center,
                maxLines = 4,
                overflow = TextOverflow.Ellipsis,
            )
            if (!serial.isNullOrBlank()) {
                Text(
                    text = serial,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.68f),
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}
