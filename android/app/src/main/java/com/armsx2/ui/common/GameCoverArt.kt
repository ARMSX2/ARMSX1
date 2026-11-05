package com.armsx2.ui.common

import androidx.compose.foundation.background
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import coil.compose.SubcomposeAsyncImage
import coil.imageLoader
import com.armsx2.CustomCovers
import com.armsx2.GameInfo
import com.armsx2.CoverCatalogue
import kotlinx.coroutines.delay
import java.io.IOException
import java.io.FileNotFoundException

@Composable
fun GameCoverArt(game: GameInfo, modifier: Modifier = Modifier, size: Int = 500) {
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
        size = size,
    ) { GameCoverPlaceholder(game.title, game.serial) }
}

/** Fall back between styles/editions; retry transient failures twice and honour library refresh. */
@Composable
fun FallbackCoverImage(
    models: List<Any>,
    contentDescription: String,
    modifier: Modifier = Modifier,
    contentScale: ContentScale = ContentScale.Fit,
    crossfade: Boolean = false,
    size: Int = 500,
    placeholder: @Composable () -> Unit,
) {
    val context = LocalContext.current
    val generation = CoverCatalogue.version.intValue
    val customVersion = CustomCovers.version.value
    val orderedModels = remember(models, generation, customVersion) { CoverArtCache.ordered(context, models) }
    val state = remember(orderedModels, generation, customVersion) { mutableStateOf(CoverLoadAttempt()) }
    val attempted = state.value
    LaunchedEffect(attempted, models, generation, customVersion) {
        if (attempted.waiting) {
            delay(attempted.delayMillis)
            if (state.value == attempted) state.value = attempted.resume()
        }
    }
    val model = orderedModels.getOrNull(attempted.index)
    val cached = remember(model, generation, customVersion, size) {
        model?.let { context.imageLoader.memoryCache?.get(CoverArtCache.memoryKey(it, size))?.bitmap }
    }
    if (cached != null) {
        Image(cached.asImageBitmap(), contentDescription, modifier, contentScale = contentScale)
        return
    }
    val preview = remember(model, generation, customVersion, size) {
        if (size > 256) model?.let { context.imageLoader.memoryCache?.get(CoverArtCache.memoryKey(it))?.bitmap }
        else null
    }
    val request = remember(context, model, crossfade, generation, attempted.retry, customVersion, size) {
        CoverArtCache.request(context, model, crossfade, size)
    }
    key(generation, attempted.retry, customVersion) { SubcomposeAsyncImage(
        model = request,
        contentDescription = contentDescription,
        modifier = modifier,
        contentScale = contentScale,
        onSuccess = { if (model != null) CoverArtCache.succeeded(context, models, model) },
        onError = { error ->
            if (state.value == attempted) {
                val cause = error.result.throwable
                CoverArtCache.failed(model, cause)
                val transient = when (cause) {
                    is coil.network.HttpException -> cause.response.code in setOf(408, 429, 500, 502, 503, 504)
                    is IOException -> cause !is FileNotFoundException
                    else -> false
                }
                state.value = attempted.failed(orderedModels.size, transient)
                if (orderedModels.isNotEmpty() && attempted.index == orderedModels.lastIndex) {
                    val source = (model as? String)?.takeIf {
                        it.startsWith("https://raw.githubusercontent.com/xlenore/psx-covers/")
                    } ?: "local/custom"
                    android.util.Log.w("ARMSX-Covers", "Failed source=$source retry=${attempted.retry} cause=${cause.javaClass.simpleName}")
                }
            }
        },
        loading = {
            if (preview != null) Image(preview.asImageBitmap(), contentDescription,
                Modifier.fillMaxSize(), contentScale = contentScale)
            else placeholder()
        },
        error = { placeholder() },
    ) }
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
