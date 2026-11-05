package com.armsx2.ui.home

import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.grid.LazyGridState
import androidx.compose.runtime.withFrameNanos

/** Follow selection in either direction without restarting an animation on held input. */
internal suspend fun LazyListState.revealControllerItem(index: Int) {
    withFrameNanos { }
    if (index !in 0 until layoutInfo.totalItemsCount) return
    if (layoutInfo.visibleItemsInfo.none { it.index == index }) scrollToItem(index)
    val info = layoutInfo
    val item = info.visibleItemsInfo.firstOrNull { it.index == index } ?: return
    val delta = HomeScrollPolicy.revealDelta(item.offset, item.size,
        info.viewportStartOffset + info.beforeContentPadding,
        info.viewportEndOffset - info.afterContentPadding)
    if (delta != 0) scrollBy(delta.toFloat())
}

internal suspend fun LazyGridState.revealControllerItem(index: Int) {
    withFrameNanos { }
    if (index !in 0 until layoutInfo.totalItemsCount) return
    if (layoutInfo.visibleItemsInfo.none { it.index == index }) scrollToItem(index)
    val info = layoutInfo
    val item = info.visibleItemsInfo.firstOrNull { it.index == index } ?: return
    val delta = HomeScrollPolicy.revealDelta(item.offset.y, item.size.height,
        info.viewportStartOffset + info.beforeContentPadding,
        info.viewportEndOffset - info.afterContentPadding)
    if (delta != 0) scrollBy(delta.toFloat())
}
