package com.armsx2.ui.memorycards

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.core.Ps1MemoryCards
import com.armsx2.GameInfo
import com.armsx2.data.library.GameLibraryRepository
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * One mounted SwanStation slot: a per-game card in Slot 1 or the shared Slot 2 card.
 */
data class MemoryCardSlot(
    val slot: Int,
    val file: File?,
    val exists: Boolean,
    val sizeBytes: Long,
    val modifiedAt: Long,
) {
    val canManage: Boolean get() = file != null
    /** A card the core will accept: exactly 128 KiB of raw card data. */
    val isValidSize: Boolean get() = exists && sizeBytes == Ps1MemoryCards.CARD_SIZE_BYTES
}

data class MemoryCardUiState(
    val slots: List<MemoryCardSlot> = emptyList(),
    val directory: String = "",
    val loading: Boolean = true,
    val busy: Boolean = false,
    val error: String? = null,
    val message: String? = null,
    val games: List<GameInfo> = emptyList(),
    val targetHint: String? = null,
)

/**
 * Backing state for [MemoryCardScreen].
 *
 * File work runs off the UI thread under the native-session lock. Each action captures its
 * game and exact destination, so changing selection or returning from a picker cannot redirect it.
 */
class MemoryCardViewModel(application: Application) : AndroidViewModel(application) {

    private var gameSource: String? = null

    var state = androidx.compose.runtime.mutableStateOf(MemoryCardUiState())
        private set

    fun refresh(source: String? = gameSource) {
        gameSource = source
        state.value = state.value.copy(loading = true, slots = emptyList(), targetHint = null)
        viewModelScope.launch {
            val slots = withContext(Dispatchers.IO) { readSlots(source) }
            val games = withContext(Dispatchers.IO) {
                runCatching { GameLibraryRepository(getApplication()).loadCached().games }
                    .getOrDefault(emptyList()).sortedBy { it.title.lowercase() }
            }
            if (gameSource != source) return@launch
            state.value = state.value.copy(
                slots = slots,
                directory = Ps1MemoryCards.cardDirectory(getApplication<Application>()).absolutePath,
                loading = false,
                games = games,
                targetHint = if (slots.none { it.slot == 1 })
                    "Start and close the selected game once before importing its Slot 1 card. No other game's card will be replaced."
                    else null,
            )
        }
    }

    /** Produce a blank 128 KiB card for [slot] — byte-identical to what the core makes on boot. */
    fun create(slot: Int, expectedPath: String) = runAction(
        success = { "Created ${Ps1MemoryCards.fileName(slot)} (128 KiB)." },
        failure = { reason -> "Could not create a card for slot $slot. $reason" },
    ) { source ->
        checkPickerTarget(slot, expectedPath, source)
        Ps1MemoryCards.createBlank(getApplication<Application>(), slot, source)
    }

    /**
     * Wipe [slot] by deleting its file; the core writes a fresh blank card the next time a game
     * boots. The core has no in-place format call, so delete-and-regenerate is the honest
     * implementation of "format".
     */
    fun reset(slot: Int, expectedPath: String) = runAction(
        success = { "Slot $slot cleared. A blank card is created the next time a game boots." },
        failure = { reason -> "Could not clear slot $slot. $reason" },
    ) { source ->
        checkPickerTarget(slot, expectedPath, source)
        Ps1MemoryCards.delete(getApplication<Application>(), slot, source)
    }

    /** Copy a picked `.mcd` into [slot], replacing whatever card is there. */
    fun import(slot: Int, uri: Uri, expectedPath: String, source: String? = gameSource) = runAction(
        success = { "Imported a card into slot $slot." },
        failure = { it },
        source = source,
    ) { source ->
        checkPickerTarget(slot, expectedPath, source)
        Ps1MemoryCards.import(getApplication<Application>(), slot, uri, source)
    }

    /** Copy [slot]'s card out to a SAF destination the user picked. */
    fun export(slot: Int, uri: Uri, expectedPath: String, source: String? = gameSource) = runAction(
        success = { "Exported ${Ps1MemoryCards.fileName(slot)}." },
        failure = { it },
        source = source,
    ) { source ->
        checkPickerTarget(slot, expectedPath, source)
        Ps1MemoryCards.export(getApplication<Application>(), slot, uri, source)
    }

    fun pickerFailed() {
        state.value = state.value.copy(error = "The selected card slot was lost. Please tap Import or Export again.")
    }

    fun importAll(uri: Uri, folder: Boolean) {
        var result = ""
        runAction(success = { result }, failure = { it }) {
            val games = GameLibraryRepository(getApplication()).loadCached().games
            result = com.armsx2.core.Ps1BulkCards.import(getApplication(), uri, folder, games).toString()
        }
    }

    private fun checkPickerTarget(slot: Int, expectedPath: String, source: String?) {
        com.armsx2.core.Ps1CardTransferTarget.check(slot, expectedPath,
            Ps1MemoryCards.cardFile(getApplication<Application>(), slot, source))
    }

    fun dismissMessage() {
        state.value = state.value.copy(error = null, message = null)
    }

    /**
     * Run [work] off the main thread, then always re-read the slots so the UI reflects the disk.
     *
     * [failure] receives the thrown message so callers can surface the specific reason (wrong file
     * size on import, unreadable destination on export) or ignore it for a fixed line.
     */
    private fun runAction(
        success: () -> String,
        failure: (String) -> String,
        source: String? = gameSource,
        work: (String?) -> Unit,
    ) {
        if (state.value.busy) return
        if (com.armsx2.runtime.MainActivityRuntime.eState.value != com.armsx2.EmuState.STOPPED ||
            runCatching { kr.co.iefriends.pcsx2.NativeApp.hasActiveVM() }.getOrDefault(false)) {
            state.value = state.value.copy(error = "Close the game before managing memory cards.")
            return
        }
        state.value = state.value.copy(busy = true, error = null, message = null)
        viewModelScope.launch {
            val outcome = withContext(Dispatchers.IO) {
                runCatching {
                    val lock = Ps1MemoryCards.sessionLock
                    check(lock.tryLock()) { "Close the game before managing memory cards." }
                    try {
                        check(com.armsx2.runtime.MainActivityRuntime.eState.value == com.armsx2.EmuState.STOPPED &&
                            !kr.co.iefriends.pcsx2.NativeApp.hasActiveVM()) {
                            "Close the game before managing memory cards."
                        }
                        work(source)
                    } finally {
                        lock.unlock()
                    }
                }
            }
            val slots = withContext(Dispatchers.IO) { readSlots() }
            state.value = state.value.copy(
                slots = slots,
                directory = Ps1MemoryCards.cardDirectory(getApplication<Application>()).absolutePath,
                loading = false,
                busy = false,
                message = if (outcome.isSuccess) success() else null,
                error = if (outcome.isSuccess) null else {
                    failure(outcome.exceptionOrNull()?.message ?: "The operation failed.")
                },
            )
        }
    }

    private fun readSlots(source: String? = gameSource): List<MemoryCardSlot> = memoryCardSlotSnapshots { slot ->
        Ps1MemoryCards.cardFile(getApplication<Application>(), slot, source)
    }
}

/** An unresolved card path is not an absent physical slot. Never silently omit either row. */
internal fun memoryCardSlotSnapshots(resolve: (Int) -> File): List<MemoryCardSlot> =
    Ps1MemoryCards.SLOTS.map { slot ->
        val file = runCatching { resolve(slot) }.getOrNull()
        val exists = file?.isFile == true
        MemoryCardSlot(
            slot = slot,
            file = file,
            exists = exists,
            sizeBytes = if (exists) file!!.length() else 0L,
            modifiedAt = if (exists) file!!.lastModified() else 0L,
        )
    }
