package com.armsx2.ui.home

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.i18n.str
import com.armsx2.ui.common.PsOneContactStrip
import com.armsx2.ui.common.psOneBevel

/** The artwork is a label inset into a moulded card; touch and pad actions stay unchanged. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun PsOneGameTile(
    title: String, selected: Boolean, showLabel: Boolean, modifier: Modifier = Modifier,
    onClick: () -> Unit, onDetails: () -> Unit, cover: @Composable (Modifier) -> Unit,
) {
    Surface(
        modifier = modifier.combinedClickable(onClick = onClick, onLongClick = onDetails),
        shape = RoundedCornerShape(topStart = 20.dp, topEnd = 20.dp, bottomEnd = 9.dp, bottomStart = 9.dp),
        color = MaterialTheme.colorScheme.surfaceVariant,
        border = BorderStroke(if (selected) 3.dp else 1.dp,
            if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline),
        shadowElevation = if (selected) 6.dp else 2.dp,
    ) {
        Column(Modifier.psOneBevel(true).padding(9.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Row(Modifier.fillMaxWidth().padding(bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(5.dp).background(
                    if (selected) MaterialTheme.colorScheme.primary else Color(0xFF3B8066), RoundedCornerShape(50)))
                Spacer(Modifier.width(5.dp))
                Text(str("app.interface.memoryCard"), fontSize = 8.sp, lineHeight = 11.sp,
                    letterSpacing = 0.6.sp, fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
            Surface(shape = RoundedCornerShape(9.dp), color = MaterialTheme.colorScheme.surface,
                border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)) {
                cover(Modifier.fillMaxWidth().aspectRatio(1f).padding(3.dp))
            }
            if (showLabel) {
                Text(title, style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.Bold,
                    maxLines = 2, minLines = 2, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 5.dp))
            } else Spacer(Modifier.height(7.dp))
            PsOneContactStrip()
        }
    }
}
