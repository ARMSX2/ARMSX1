package com.armsx2.ui.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.unit.dp
import com.armsx2.i18n.str

/** The inline chooser owns controller navigation until a choice is confirmed or cancelled. */
internal object ControllerTypePickerNav {
    @Volatile var dismiss: (() -> Unit)? = null
    fun back(): Boolean {
        val close = dismiss ?: return false
        close()
        return true
    }
}

@Composable
internal fun ControllerTypePicker(player: Int, label: String, options: List<String>, selectedIndex: Int,
    description: String, onSelected: (Int) -> Unit) {
    var expanded by remember(player, label) { mutableStateOf(false) }
    val layer = "controller-type:$player:$label"
    val row = "segmented:$label" // Stable settings-search and controller focus identity.
    val toggle = {
        if (expanded) expanded = false else {
            ControllerTypePickerNav.back()
            expanded = true
        }
    }
    BackHandler(enabled = expanded) { expanded = false }
    if (expanded) {
        DisposableEffect(layer) {
            val close = { expanded = false }
            val release = SettingsControllerNav.claimLayer(layer)
            ControllerTypePickerNav.dismiss = close
            onDispose {
                if (ControllerTypePickerNav.dismiss === close) ControllerTypePickerNav.dismiss = null
                release()
            }
        }
        LaunchedEffect(layer) {
            repeat(12) {
                if (SettingsControllerNav.selectById("$layer.option:$selectedIndex")) return@LaunchedEffect
                withFrameNanos { }
            }
        }
    }
    Column(Modifier.fillMaxWidth().padding(vertical = 5.dp)
        .clip(RoundedCornerShape(22.dp))
        .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.72f))
        .border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.46f), RoundedCornerShape(22.dp))) {
        Column(Modifier.fillMaxWidth().controllerFocusable(row, onConfirm = toggle)
            .clickable(onClick = toggle).padding(16.dp)) {
            ChangedSettingMarker(label)
            Row(Modifier.fillMaxWidth()) {
                Text(label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                PerGameMarker(label)
                InfoHint(label, description)
            }
            Text(description, style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 6.dp))
            Text("${options.getOrElse(selectedIndex) { "" }}  ${if (expanded) "▴" else "▾"}",
                color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.titleSmall)
        }
        if (expanded) {
            options.forEachIndexed { index, option ->
                val chosen = index == selectedIndex
                val choose = { onSelected(index); expanded = false }
                Text(option, color = if (chosen) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.fillMaxWidth()
                        .background(if (chosen) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface)
                        .semantics { selected = chosen }
                        .controllerFocusable("$layer.option:$index", onConfirm = choose, layer = layer)
                        .clickable(onClick = choose).padding(horizontal = 18.dp, vertical = 14.dp))
            }
            val close = { expanded = false }
            Text(str("action.cancel"), color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.fillMaxWidth().controllerFocusable("$layer.cancel", onConfirm = close, layer = layer)
                    .clickable(onClick = close).padding(18.dp))
        }
    }
}
