package com.armsx2.ui.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str
import com.armsx2.ui.home.LibraryBackground
import com.armsx2.ui.home.LibraryBackgroundStyle

@Composable
fun LibraryBackgroundStyleSection(showTitle: Boolean = true) {
    val context = LocalContext.current
    LaunchedEffect(Unit) { LibraryBackground.ensureLoaded() }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { LibraryBackground.set(context, it) }
    }
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (showTitle) Text(str("app.bg.style"), style = MaterialTheme.typography.titleMedium)
        if (com.armsx2.ui.theme.InterfacePreferences.psOne) {
            Text(str("app.interface.background"), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text(str("app.bg.style.desc"), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp)) {
            LibraryBackgroundStyle.entries.filter {
                it != LibraryBackgroundStyle.CustomImage || LibraryBackground.uri.value != null
            }.forEach { style ->
                val label = when (style) {
                    LibraryBackgroundStyle.Original -> "app.bg.original"
                    LibraryBackgroundStyle.PsOne -> "app.bg.psone"
                    LibraryBackgroundStyle.CustomImage -> "app.bg.custom"
                }
                val select = { LibraryBackground.setStyle(style) }
                FilterChip(selected = LibraryBackground.style.value == style, onClick = select,
                    label = { Text(str(label)) }, shape = RoundedCornerShape(11.dp),
                    modifier = Modifier.controllerFocusable("app.bg.style.${style.name}",
                        RoundedCornerShape(11.dp), onConfirm = select))
            }
        }
        val choose = { picker.launch(arrayOf("image/*")) }
        OutlinedButton(onClick = choose,
            modifier = Modifier.controllerFocusable("app.bg.choose", onConfirm = choose)) {
            Text(str("games.background.choose"))
        }
    }
}
