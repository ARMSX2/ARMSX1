package com.armsx2.ui.common

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/** Subtle moulded-plastic highlight, without assets, animation or another graphics surface. */
fun Modifier.psOneBevel(enabled: Boolean): Modifier = if (!enabled) this else drawWithContent {
    drawContent()
    val inset = 2.dp.toPx()
    val radius = androidx.compose.ui.geometry.CornerRadius(22.dp.toPx())
    drawRoundRect(Color.White.copy(alpha = 0.55f), Offset(inset, inset),
        Size((size.width - inset * 2).coerceAtLeast(0f), (size.height - inset * 2).coerceAtLeast(0f)),
        cornerRadius = radius, style = androidx.compose.ui.graphics.drawscope.Stroke(1.dp.toPx()))
}

@Composable
fun PsOneContactStrip(modifier: Modifier = Modifier) {
    Canvas(modifier.width(44.dp).height(5.dp)) {
        val gap = 2.dp.toPx()
        val w = (size.width - gap * 7) / 8
        repeat(8) { i ->
            drawRect(Color(0xFFA79A6E), Offset(i * (w + gap), 0f), Size(w, size.height))
        }
    }
}
