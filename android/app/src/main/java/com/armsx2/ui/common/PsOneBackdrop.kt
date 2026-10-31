package com.armsx2.ui.common

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp

/** Static, decorative chassis detailing, drawn locally without textures, animation or GLES. */
@Composable
fun PsOneBackdrop(modifier: Modifier = Modifier) {
    val darkUi = MaterialTheme.colorScheme.background.luminance() < 0.5f
    Canvas(modifier.fillMaxSize()) {
        drawRect(Brush.verticalGradient(listOf(Color(0xFFF4F3ED), Color(0xFFDEDFD9))))
        val seam = Color(0xFF777A7D).copy(alpha = 0.10f)
        val radius = size.minDimension * 0.57f
        val lid = Offset(size.width * 0.86f, size.height * 0.94f)
        drawCircle(seam, radius, lid, style = Stroke(2.dp.toPx()))
        drawCircle(Color.White.copy(alpha = 0.28f), radius - 4.dp.toPx(), lid,
            style = Stroke(1.dp.toPx()))
        repeat(9) { index ->
            val y = 28.dp.toPx() + index * 7.dp.toPx()
            drawLine(seam, Offset(20.dp.toPx(), y), Offset(66.dp.toPx(), y),
                strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round)
        }
        // Familiar four-button geometry, not a copied logo. Kept faint behind content.
        val r = 9.dp.toPx()
        val start = Offset(26.dp.toPx(), size.height - 26.dp.toPx())
        val step = 30.dp.toPx()
        val stroke = Stroke(2.dp.toPx())
        val triangle = Path().apply {
            moveTo(start.x, start.y - r)
            lineTo(start.x + r, start.y + r * 0.7f)
            lineTo(start.x - r, start.y + r * 0.7f)
            close()
        }
        drawPath(triangle, Color(0xFF348969).copy(alpha = 0.38f), style = stroke)
        val circle = Offset(start.x + step, start.y)
        drawCircle(Color(0xFFC34B58).copy(alpha = 0.38f), r, circle, style = stroke)
        val cross = Offset(start.x + step * 2, start.y)
        val blue = Color(0xFF4168AC).copy(alpha = 0.38f)
        drawLine(blue, cross - Offset(r, r), cross + Offset(r, r), stroke.width)
        drawLine(blue, cross + Offset(-r, r), cross + Offset(r, -r), stroke.width)
        drawRect(Color(0xFFAD5889).copy(alpha = 0.38f),
            Offset(start.x + step * 3 - r, start.y - r), Size(r * 2, r * 2), style = stroke)
        // Preserve the existing UI palette, including readable white labels in dark themes.
        if (darkUi) drawRect(Color.Black.copy(alpha = 0.62f))
    }
}
