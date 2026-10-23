package com.armsx2.ui.memorycards

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
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
import androidx.compose.runtime.saveable.rememberSaveable
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
 * Manages the actual Slot 1/2 files reported by SwanStation, not retired-core slot<N>.mcd files.
 * [game] fixes the destination to that game's recorded mount; the global screen can choose a game.
 */
@Composable
fun MemoryCardScreen(
    onBack: () -> Unit,
    game: GameInfo? = null,
    viewModel: MemoryCardViewModel = viewModel(),
    embedded: Boolean = false,
) {
    val state = viewModel.state.value
    val sessionActive = com.armsx2.runtime.MainActivityRuntime.eState.value != com.armsx2.EmuState.STOPPED
    var resetTarget by remember { mutableStateOf<MemoryCardSlot?>(null) }
    // Document pickers can recreate the Activity (rotation, low memory). Plain remember
    // loses the destination, making a successful file selection silently do nothing.
    var pendingImportSlot by rememberSaveable { mutableStateOf<Int?>(null) }
    var pendingImportPath by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingImportSource by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingExportSlot by rememberSaveable { mutableStateOf<Int?>(null) }
    var pendingExportPath by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingExportSource by rememberSaveable { mutableStateOf<String?>(null) }
    var selectedSource by rememberSaveable(game?.uri?.toString()) {
        mutableStateOf(game?.uri?.toString())
    }
    var choosingGame by remember { mutableStateOf(false) }
    var bulkUri by rememberSaveable { mutableStateOf<String?>(null) }
    var bulkFolder by rememberSaveable { mutableStateOf(false) }
    val bulkZipPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) { bulkUri = uri.toString(); bulkFolder = false }
    }
    val bulkFolderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) { bulkUri = uri.toString(); bulkFolder = true }
    }

    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val slot = pendingImportSlot
        val path = pendingImportPath
        val source = pendingImportSource
        pendingImportSlot = null
        pendingImportPath = null
        pendingImportSource = null
        if (uri != null) {
            if (slot != null && path != null) viewModel.import(slot, uri, path, source)
            else viewModel.pickerFailed()
        }
    }
    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val slot = pendingExportSlot
        val path = pendingExportPath
        val source = pendingExportSource
        pendingExportSlot = null
        pendingExportPath = null
        pendingExportSource = null
        if (uri != null) {
            if (slot != null && path != null) viewModel.export(slot, uri, path, source)
            else viewModel.pickerFailed()
        }
    }

    LaunchedEffect(selectedSource) { viewModel.refresh(selectedSource) }

    val content: @Composable () -> Unit = {
        Column(
            if (embedded) Modifier.fillMaxWidth()
            else Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (!embedded) {
                ArmsTopBar(
                    title = str("memcard.title"),
                    subtitle = "Slot 1 and Slot 2 · 128 KiB each",
                    leading = { RoundAction("←", str("action.back"), onBack) },
                    actions = {
                        RoundAction("↻", str("games.card.refresh"), { viewModel.refresh(selectedSource) })
                    },
                    horizontalPadding = 0.dp,
                )
            } else {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    val refresh = { viewModel.refresh(selectedSource) }
                    OutlinedButton(onClick = refresh,
                        modifier = Modifier.controllerFocusable("memcard.refresh", onConfirm = refresh)) {
                        Text(str("games.card.refresh"))
                    }
                }
            }

            AboutCardsPanel(state.directory, game, sessionActive)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                val zip = { bulkZipPicker.launch(arrayOf("*/*")) }
                val folder = { bulkFolderPicker.launch(null) }
                OutlinedButton(onClick = zip, enabled = !state.busy && !sessionActive,
                    modifier = Modifier.weight(1f).controllerFocusable("memcard.bulk.zip", onConfirm = zip)) {
                    Text("Import cards ZIP")
                }
                OutlinedButton(onClick = folder, enabled = !state.busy && !sessionActive,
                    modifier = Modifier.weight(1f).controllerFocusable("memcard.bulk.folder", onConfirm = folder)) {
                    Text("Import cards folder")
                }
            }

            if (game == null) {
                Box {
                    OutlinedButton(
                        onClick = { choosingGame = true },
                        enabled = !state.busy && !sessionActive,
                    ) {
                        Text("Card for: " + (state.games.firstOrNull {
                            it.uri.toString() == selectedSource
                        }?.title ?: "Last launched game"))
                    }
                    DropdownMenu(expanded = choosingGame, onDismissRequest = { choosingGame = false }) {
                        DropdownMenuItem(text = { Text("Last launched game") }, onClick = {
                            selectedSource = null
                            choosingGame = false
                        })
                        state.games.forEach { candidate ->
                            DropdownMenuItem(text = { Text(candidate.title) }, onClick = {
                                selectedSource = candidate.uri.toString()
                                choosingGame = false
                            })
                        }
                    }
                }
            }
            state.slots.forEach { slot ->
                MemoryCardSlotRow(
                    item = slot,
                    busy = state.busy || sessionActive || !slot.canManage,
                    onCreate = { slot.file?.let { viewModel.create(slot.slot, it.absolutePath) } },
                    onReset = { if (slot.canManage) resetTarget = slot },
                    onImport = {
                        slot.file?.let { file ->
                        pendingImportSlot = slot.slot
                        pendingImportPath = file.absolutePath
                        pendingImportSource = selectedSource
                        importer.launch(arrayOf("*/*"))
                        }
                    },
                    onExport = {
                        slot.file?.let { file ->
                        pendingExportSlot = slot.slot
                        pendingExportPath = file.absolutePath
                        pendingExportSource = selectedSource
                        exporter.launch(file.name)
                        }
                    },
                )
            }

            Spacer(Modifier.height(12.dp))
        }
    }
    if (embedded) content() else ArmsBackdrop { content() }

    bulkUri?.let { selected ->
        val confirm = { bulkUri = null; viewModel.importAll(android.net.Uri.parse(selected), bulkFolder) }
        AlertDialog(onDismissRequest = { bulkUri = null }, title = { Text("Import all memory cards?") },
            text = { Text("Copies raw 128 KiB cards and automatically assigns unambiguous game ID or exact " +
                "title/filename matches to your scanned library. Matching games will use the imported cards " +
                "on their next launch. Old card files are kept, never overwritten. Unmatched or ambiguous " +
                "cards are kept unassigned. Save states are not included.") },
            confirmButton = { TextButton(onClick = confirm,
                modifier = Modifier.controllerFocusable("memcard.bulk.confirm", onConfirm = confirm)) { Text("Import and assign") } },
            dismissButton = { TextButton(onClick = { bulkUri = null }) { Text(str("action.cancel")) } })
    }

    resetTarget?.let { item ->
        val confirm = {
            item.file?.let { viewModel.reset(item.slot, it.absolutePath) }
            resetTarget = null
        }
        AlertDialog(
            onDismissRequest = { resetTarget = null },
            title = { Text("Erase slot ${item.slot}?") },
            text = {
                Text(
                    "This deletes ${item.file?.name.orEmpty()} and every save on it. A blank card is created " +
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
 * Explain per-game/shared cards and the requirement to close the active native session first.
 */
@Composable
private fun AboutCardsPanel(directory: String, game: GameInfo?, sessionActive: Boolean) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(19.dp),
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = 0.5f)),
    ) {
        Column(Modifier.padding(13.dp)) {
            Text("Memory card storage", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(6.dp))
            Text(
                "Slot 1 defaults to a per-game card; Slot 2 defaults to a shared card. " +
                    "Bulk imports copy raw 128 KiB card images and assign unique game matches " +
                    "without overwriting old card files. Single-slot replacement retains a backup. " +
                    "Memory cards and emulator save states are different formats.",
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
            if (sessionActive) {
                Spacer(Modifier.height(10.dp))
                // The core loads a card into memory on boot and writes the whole buffer back when
                // the game shuts down, so anything changed mid-session is overwritten on exit.
                StatusChip("Close ${game?.title?.ifBlank { "the game" } ?: "the game"} before managing cards", Warning)
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
                        item.file?.name ?: "—",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                when {
                    !item.canManage -> StatusChip("Not loaded", MaterialTheme.colorScheme.onSurfaceVariant)
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
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.create", onConfirm = { if (!busy) onCreate() }),
                    ) { Text("Create formatted card") }
                } else {
                    OutlinedButton(
                        onClick = onExport,
                        enabled = !busy,
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.export", onConfirm = { if (!busy) onExport() }),
                    ) { Text(str("action.export")) }
                }
                Spacer(Modifier.width(7.dp))
                TextButton(
                    onClick = onImport,
                    enabled = !busy,
                    modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.import", onConfirm = { if (!busy) onImport() }),
                ) { Text(str("action.import")) }
                if (item.exists) {
                    Spacer(Modifier.width(7.dp))
                    TextButton(
                        onClick = onReset,
                        enabled = !busy,
                        modifier = Modifier.controllerFocusable("memcard.slot${item.slot}.reset", onConfirm = { if (!busy) onReset() }),
                    ) { Text("Erase", color = MaterialTheme.colorScheme.error) }
                }
            }
        }
    }
}

/** One line of detail under a slot: size and, when the file is odd, why the core will reject it. */
private fun slotDetail(item: MemoryCardSlot): String = when {
    !item.canManage -> "—"
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
