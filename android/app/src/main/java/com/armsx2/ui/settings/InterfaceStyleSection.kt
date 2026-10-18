package com.armsx2.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.theme.InterfacePreferences
import com.armsx2.ui.theme.InterfaceStyle

@Composable
fun InterfaceStyleSection(showTitle: Boolean = true) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (showTitle) Text(str("app.interface.style"), style = MaterialTheme.typography.titleMedium)
        Text(str("app.interface.desc"), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            InterfaceStyle.entries.forEach { style ->
                val select = { InterfacePreferences.set(style) }
                FilterChip(
                    selected = InterfacePreferences.style.value == style, onClick = select,
                    label = { Text(str(if (style == InterfaceStyle.PsOne) "app.bg.psone" else "app.bg.original")) },
                    shape = RoundedCornerShape(12.dp),
                    modifier = Modifier.controllerFocusable("app.interface.${style.name}",
                        RoundedCornerShape(12.dp), onConfirm = select),
                )
            }
        }
    }
}
