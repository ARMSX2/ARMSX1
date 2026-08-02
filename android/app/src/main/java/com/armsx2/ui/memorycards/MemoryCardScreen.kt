package com.armsx2.ui.memorycards

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.armsx2.GameInfo
import com.armsx2.core.Ps1MemoryCards
import com.armsx2.i18n.str
import com.armsx2.ui.common.ArmsBackdrop
import com.armsx2.ui.common.ArmsTopBar
import com.armsx2.ui.common.RoundAction
import com.armsx2.ui.common.StatusChip
import com.armsx2.ui.settings.controllerFocusable
import com.armsx2.ui.theme.Success
import com.armsx2.ui.theme.Warning
import java.text.DateFormat
import java.util.Date

/**
 * Memory-card manager for the PS1 core.
 *
 * The core attaches exactly two cards — `slot1.mcd` and `slot2.mcd` from its pref path (the app
 * files dir) — and nothing else, so this screen manages those two files rather than a library of
 * cards. Every PS2 notion the ARMSX2 original carried (8 MB/16 MB/32 MB/64 MB sizes, folder cards,
 * eight slots, `.ps2` files, per-game card overrides) is gone because the PS1 core has no equivalent:
 * a PS1 card is a fixed 128 KiB raw image, and the two paths are compiled in.
 *
 * [game] is only used for the "restart to apply" hint — cards cannot be scoped per game.
 */
@Composable
fun MemoryCardScreen(onBack: () -> Unit, game: GameInfo? = null, viewModel: MemoryCardViewModel = viewModel()) {
    val state = viewModel.state.value
    var resetTarget by remember { mutableStateOf<MemoryCardSlot?>(null) }
    var pendingImportSlot by remember { mutableStateOf<Int?>(null) }
    var pendingExportSlot by remember { mutableStateOf<Int?>(null) }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val slot = pendingImportSlot
        pendingImportSlot = null
        if (uri != null && slot != null) viewModel.import(slot, uri)
    }
    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val slot = pendingExportSlot
        pendingExportSlot = null
        if (uri != null && slot != null) viewModel.export(slot, uri)
    }

    LaunchedEffect(Unit) { viewModel.refresh() }

    ArmsBackdrop {
        LazyColumn(
            Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            contentPadding = PaddingValues(horizontal = 8.dp),
        ) {
            item {
                ArmsTopBar(
                    title = str("memcard.title"),
                    subtitle = "Slot 1 and Slot 2 · 128 KiB each",
                    leading = { RoundAction("←", str("action.back"), onBack) },
                    actions = {
                        RoundAction("↻", str("games.card.refresh"), viewModel::refresh)
                    },
                    horizontalPadding = 0.dp,
                )
            }

            item { AboutCardsPanel(state.directory, game) }

            items(state.slots, key = { it.slot }) { slot ->
                MemoryCardSlotRow(
                    item = slot,
                    busy = state.busy,
                    onCreate = { viewModel.create(slot.slot) },
                    onReset = { resetTarget = slot },
                    onImport = {
                        pendingImportSlot = slot.slot
                        importer.launch(arrayOf("application/octet-stream", "*/*"))
                    },
                    onExport = {
                        pendingExportSlot = slot.slot
                        exporter.launch(slot.file.name)
                    },
                )
            }

            item { Spacer(Modifier.height(12.dp)) }
        }
    }

    resetTarget?.let { item ->
        val confirm = {
            viewModel.reset(item.slot)
            resetTarget = null
        }
        AlertDialog(
            onDismissRequest = { resetTarget = null },
            title = { Text("Erase slot ${item.slot}?") },
            text = {
                Text(
                    "This deletes ${item.file.name} and every save on it. A blank card is created " +
                        "automatically the next time a game boots. Export it first if you want a backup.",
                )
            },
            confirmButton = {
                TextButton(
                    onClick = confirm,
                    modifier = Modifier.controllerFocusable("memcard.reset.confirm", onConfirm = confirm),
                ) { Text("Erase", color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = {
                TextButton(
                    onClick = { resetTarget = null },
                    modifier = Modifier.controllerFocusable("memcard.reset.cancel", onConfirm = { resetTarget = null }),
                ) { Text(str("action.cancel")) }
            },
        )
    }

    (state.error ?: state.message)?.let { message ->
        AlertDialog(
            onDismissRequest = viewModel::dismissMessage,
            title = { Text(if (state.error != null) str("memcard.title") else str("action.ok")) },
            text = { Text(message) },
            confirmButton = {
                TextButton(
                    onClick = viewModel::dismissMessage,
                    modifier = Modifier.controllerFocusable("memcard.message.ok", onConfirm = viewModel::dismissMessage),
                ) { Text(str("action.ok")) }
            },
        )
    }
}

/**
 * The standing explanation of what this screen can and cannot do. Says out loud that cards are
 * shared by every game, because the core offers no per-game card path to hang a control off.
 */
@Composable
private fun AboutCardsPanel(directory: String, game: GameInfo?) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(19.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Text("How memory cards work here", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(6.dp))
            Text(
                "The emulator always plugs a card into Slot 1 and Slot 2. Each card is a single " +
                    "128 KiB file and is shared by every game — there are no per-game cards, no card " +
                    "sizes and no folder cards on PlayStation. A new card shows as unformatted until " +
                    "the console's memory-card manager formats it.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (directory.isNotBlank()) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Stored in $directory",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            if (game != null) {
                Spacer(Modifier.height(10.dp))
                // The core loads a card into memory on boot and writes the whole buffer back when
                // the game shuts down, so anything changed mid-session is overwritten on exit.
                StatusChip("Close ${game.title.ifBlank { "the game" }} before changing cards", Warning)
            }
        }
    }
}

@Composable
private fun MemoryCardSlotRow(
    item: MemoryCardSlot,
    busy: Boolean,
    onCreate: () -> Unit,
    onReset: () -> Unit,
    onImport: () -> Unit,
    onExport: () -> Unit,
) {
    val slotLabel = if (item.slot == 1) str("memcard.slot1") else str("memcard.slot2")
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(19.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("▤", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.headlineMedium)
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Text(slotLabel, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(
                        item.file.name,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                when {
                    item.isValidSize -> StatusChip("Card inserted", Success)
                    item.exists -> StatusChip("Wrong size", Warning)
                    else -> StatusChip("Empty", MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Spacer(Modifier.height(6.dp))
            Text(
                slotDetail(item),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (!item.exists) {
                    Button(
                        onClick = onCreate,
                        enabled = !busy,
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.create", onConfirm = onCreate),
                    ) { Text(str("memcard.create")) }
                } else {
                    OutlinedButton(
                        onClick = onExport,
                        enabled = !busy,
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.export", onConfirm = onExport),
                    ) { Text(str("action.export")) }
                }
                Spacer(Modifier.width(7.dp))
                TextButton(
                    onClick = onImport,
                    enabled = !busy,
                    modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.import", onConfirm = onImport),
                ) { Text(str("action.import")) }
                if (item.exists) {
                    Spacer(Modifier.width(7.dp))
                    TextButton(
                        onClick = onReset,
                        enabled = !busy,
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.reset", onConfirm = onReset),
                    ) { Text("Erase", color = MaterialTheme.colorScheme.error) }
                }
            }
        }
    }
}

/** One line of detail under a slot: size and, when the file is odd, why the core will reject it. */
private fun slotDetail(item: MemoryCardSlot): String = when {
    item.isValidSize ->
        "${Ps1MemoryCards.describeSize(item.sizeBytes)} · last written ${formatTimestamp(item.modifiedAt)}"
    item.exists ->
        "${Ps1MemoryCards.describeSize(item.sizeBytes)} — a memory card must be exactly 128 KiB. " +
            "Erase it and let the emulator rebuild it, or import a valid card."
    else ->
        "No card yet. Create one, import a .mcd, or just start a game — the emulator makes a blank " +
            "128 KiB card on boot."
}

private fun formatTimestamp(millis: Long): String =
    if (millis <= 0L) "never" else DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(millis))
