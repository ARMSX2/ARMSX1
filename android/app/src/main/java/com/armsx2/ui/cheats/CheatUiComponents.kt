package com.armsx2.ui.cheats

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.cheats.Ps1CheatDatabaseItem
import com.armsx2.i18n.str
import com.armsx2.ui.common.SearchField
import com.armsx2.ui.settings.controllerFocusable

/** Same touch/gamepad keyboard as the library, without a full settings card per action. */
@Composable
internal fun CheatSearchBar(
    query: String,
    placeholder: String,
    controllerId: String,
    onClear: () -> Unit,
    onSearch: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        SearchField(
            value = query,
            placeholder = placeholder,
            onClick = onSearch,
            modifier = Modifier.weight(1f).controllerFocusable(controllerId, onConfirm = onSearch),
        )
        if (query.isNotBlank()) TextButton(
            onClick = onClear,
            modifier = Modifier.controllerFocusable("$controllerId.clear", onConfirm = onClear),
        ) { Text(str("cheats.database.clearSearch")) }
    }
}

@Composable
internal fun catalogueCount(item: Ps1CheatDatabaseItem): String =
    if (item.omittedCount == 0) str("cheats.ui.available").format(item.cheatCount)
    else str("cheats.database.count").format(item.cheatCount, item.omittedCount)

@Composable
internal fun CheatDatabaseAttribution() {
    Text(
        "Libretro contributors · CC BY-SA 4.0",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 12.dp, bottom = 8.dp),
    )
}
