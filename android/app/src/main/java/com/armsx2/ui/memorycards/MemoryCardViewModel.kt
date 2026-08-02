package com.armsx2.ui.memorycards

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.core.Ps1MemoryCards
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * One of the two memory-card slots the ARMSX1 core attaches (`slot1.mcd` / `slot2.mcd` under the
 * native pref path). There is no card *library* to browse: the core hard-codes both filenames, so
 * the slot IS the card.
 */
data class MemoryCardSlot(
    val slot: Int,
    val file: File,
    val exists: Boolean,
    val sizeBytes: Long,
    val modifiedAt: Long,
) {
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
)

/**
 * Backing state for [MemoryCardScreen].
 *
 * Every action is plain `java.io.File` work on [Dispatchers.IO] against the app-private pref path —
 * no JNI. The old PS2 version routed creation through `NativeApp.createMemoryCard(name, type, size)`
 * which wrote 128 KiB files into `filesDir/memcards/`, a directory the PS1 core never looks at, so
 * nothing the user "created" ever became a card. See [Ps1MemoryCards] for the paths and why they
 * are fixed.
 */
class MemoryCardViewModel(application: Application) : AndroidViewModel(application) {

    var state = androidx.compose.runtime.mutableStateOf(MemoryCardUiState())
        private set

    fun refresh() {
        viewModelScope.launch {
            val slots = withContext(Dispatchers.IO) { readSlots() }
            state.value = state.value.copy(
                slots = slots,
                directory = Ps1MemoryCards.cardDirectory(getApplication<Application>()).absolutePath,
                loading = false,
            )
        }
    }

    /** Produce a blank 128 KiB card for [slot] — byte-identical to what the core makes on boot. */
    fun create(slot: Int) = runAction(
        success = { "Created ${Ps1MemoryCards.fileName(slot)} (128 KiB)." },
        failure = { reason -> "Could not create a card for slot $slot. $reason" },
    ) {
        Ps1MemoryCards.createBlank(getApplication<Application>(), slot)
    }

    /**
     * Wipe [slot] by deleting its file; the core writes a fresh blank card the next time a game
     * boots. The core has no in-place format call, so delete-and-regenerate is the honest
     * implementation of "format".
     */
    fun reset(slot: Int) = runAction(
        success = { "Slot $slot cleared. A blank card is created the next time a game boots." },
        failure = { reason -> "Could not clear slot $slot. $reason" },
    ) {
        Ps1MemoryCards.delete(getApplication<Application>(), slot)
    }

    /** Copy a picked `.mcd` into [slot], replacing whatever card is there. */
    fun import(slot: Int, uri: Uri) = runAction(
        success = { "Imported a card into slot $slot." },
        failure = { it },
    ) {
        Ps1MemoryCards.import(getApplication<Application>(), slot, uri)
    }

    /** Copy [slot]'s card out to a SAF destination the user picked. */
    fun export(slot: Int, uri: Uri) = runAction(
        success = { "Exported ${Ps1MemoryCards.fileName(slot)}." },
        failure = { it },
    ) {
        Ps1MemoryCards.export(getApplication<Application>(), slot, uri)
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
        work: () -> Unit,
    ) {
        if (state.value.busy) return
        state.value = state.value.copy(busy = true, error = null, message = null)
        viewModelScope.launch {
            val outcome = withContext(Dispatchers.IO) { runCatching { work() } }
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

    private fun readSlots(): List<MemoryCardSlot> = Ps1MemoryCards.SLOTS.map { slot ->
        val file = Ps1MemoryCards.cardFile(getApplication<Application>(), slot)
        MemoryCardSlot(
            slot = slot,
            file = file,
            exists = file.isFile,
            sizeBytes = if (file.isFile) file.length() else 0L,
            modifiedAt = if (file.isFile) file.lastModified() else 0L,
        )
    }
}
