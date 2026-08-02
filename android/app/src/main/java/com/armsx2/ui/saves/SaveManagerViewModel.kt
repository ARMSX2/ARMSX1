package com.armsx2.ui.saves

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

data class SaveStateItem(
    val slot: Int?,
    val file: File,
    val gameTitle: String,
    val serial: String,
    val preview: Bitmap?,
    val canUseWithActiveGame: Boolean,
)

data class SaveManagerUiState(
    val gameTitle: String? = null,
    val saves: List<SaveStateItem> = emptyList(),
    val loading: Boolean = true,
    val message: String? = null,
    val error: String? = null,
)

class SaveManagerViewModel(application: Application) : AndroidViewModel(application) {
    var state = androidx.compose.runtime.mutableStateOf(SaveManagerUiState())
        private set

    fun refresh() {
        val previous = state.value
        state.value = previous.copy(loading = true)
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { readSaves() }
            state.value = state.value.copy(
                gameTitle = result.gameTitle,
                saves = result.saves,
                loading = false,
            )
        }
    }

    fun save(slot: Int) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching { NativeApp.saveStateToSlot(slot) }.getOrDefault(false)
            }
            state.value = if (ok) {
                state.value.copy(message = "${I18n.get("action.save")} · ${slot + 1}")
            } else {
                state.value.copy(error = "${I18n.get("action.save")} · ${slot + 1}")
            }
            refresh()
        }
    }

    fun load(item: SaveStateItem) {
        val slot = item.slot ?: return
        viewModelScope.launch {
            val hasActiveVm = withContext(Dispatchers.IO) {
                runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
            }
            // With a VM up this is a live load, so it goes through SaveStateGuard and may ask
            // about memory-card divergence first. Cold-booting into the slot does not: that
            // path retries on a timer until the renderer is presenting, and a prompt inside a
            // retry loop would ask the same question sixty times.
            val outcome = if (hasActiveVm) {
                SaveStateGuard.load(slot)
            } else if (MainActivityRuntime.launchCurrentGameFromSaveSlot(slot)) {
                SaveStateGuard.Outcome.Loaded
            } else {
                SaveStateGuard.Outcome.Failed
            }
            state.value = when (outcome) {
                SaveStateGuard.Outcome.Loaded ->
                    state.value.copy(message = "${I18n.get("touch.stateAction.load")} · ${slot + 1}")
                // Declined, not failed. An error banner would misreport the user's own choice.
                SaveStateGuard.Outcome.Cancelled -> state.value
                SaveStateGuard.Outcome.Failed ->
                    state.value.copy(error = "${I18n.get("touch.stateAction.load")} · ${slot + 1}")
            }
        }
    }

    fun delete(item: SaveStateItem) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { runCatching { item.file.delete() }.getOrDefault(false) }
            state.value = if (ok) {
                state.value.copy(message = I18n.get("action.delete"))
            } else {
                state.value.copy(error = I18n.get("action.delete"))
            }
            refresh()
        }
    }

    fun backupAll() {
        val files = state.value.saves.map(SaveStateItem::file)
        if (files.isEmpty()) {
            state.value = state.value.copy(error = I18n.get("savestate.noSavesToBackUp"))
            return
        }
        viewModelScope.launch {
            val count = withContext(Dispatchers.IO) {
                val destination = File(files.first().parentFile, "backups/${System.currentTimeMillis()}").apply { mkdirs() }
                files.count { file ->
                    runCatching {
                        file.copyTo(File(destination, file.name), overwrite = true)
                        true
                    }.getOrDefault(false)
                }
            }
            state.value = state.value.copy(message = "${I18n.get("savestate.backup")} · $count")
        }
    }

    fun dismissMessage() {
        state.value = state.value.copy(message = null, error = null)
    }

    private data class ReadResult(
        val gameTitle: String?,
        val saves: List<SaveStateItem>,
    )

    private fun readSaves(): ReadResult {
        val active = MainActivityRuntime.currentGame.value
        val activeSerial = active?.serial.orEmpty()
        val activePaths = (0 until SLOT_COUNT).mapNotNull { slot ->
            runCatching { NativeApp.getGamePathSlot(slot) }
                .getOrNull()
                ?.takeIf(String::isNotBlank)
                ?.let(::File)
                ?.takeIf(File::exists)
        }

        val roots = listOf("sstates", "savestates")
            .map { File(MainActivityRuntime.assetCopyRoot(getApplication()), it) }
        val discovered = roots.flatMap { root ->
            if (!root.isDirectory) emptyList()
            else root.walkTopDown().filter { it.isFile && it.extension.equals("pss", true) }.toList()
        }
        val allFiles = (activePaths + discovered)
            .distinctBy { it.absolutePath.lowercase() }
            .filter { file -> active == null || activeSerial.isBlank() || serialFrom(file).equals(activeSerial, true) }

        val saves = allFiles.map { file ->
            val serial = serialFrom(file)
            val belongsToActiveGame = active != null && (
                activeSerial.isBlank() || serial.equals(activeSerial, true) || activePaths.any { it.absolutePath == file.absolutePath }
            )
            SaveStateItem(
                slot = slotFrom(file),
                file = file,
                gameTitle = if (belongsToActiveGame) active?.title.orEmpty().ifBlank { serial } else serial,
                serial = serial,
                preview = decodePreview(file),
                canUseWithActiveGame = belongsToActiveGame,
            )
        }.sortedWith(compareBy<SaveStateItem> { it.slot == null }.thenBy { it.slot ?: Int.MAX_VALUE }.thenByDescending { it.file.lastModified() })

        return ReadResult(
            gameTitle = active?.title,
            saves = saves,
        )
    }

    private fun decodePreview(file: File): Bitmap? {
        val bytes = runCatching { NativeApp.getSaveStateImage(file.absolutePath) }.getOrNull() ?: return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        var sample = 1
        while (bounds.outWidth / sample > 640 || bounds.outHeight / sample > 360) sample *= 2
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply {
            inSampleSize = sample
            inPreferredConfig = Bitmap.Config.RGB_565
        })
    }

    private fun slotFrom(file: File): Int? = SLOT_PATTERN.find(file.name)?.groupValues?.getOrNull(1)?.toIntOrNull()

    private fun serialFrom(file: File): String = file.name.substringBefore(" (").ifBlank {
        // psx/state.c:1268 writes "<sanitised title>-<crc32>.slot<N>.pss". nameWithoutExtension
        // strips ".pss", substringBeforeLast('.') strips ".slotN", and CRC_SUFFIX strips the
        // content hash — leaving the title, which is what the card shows.
        file.nameWithoutExtension.substringBeforeLast('.').replace(CRC_SUFFIX, "")
    }

    private companion object {
        const val SLOT_COUNT = 10

        /* This core writes PS1 states as "<title>-<crc32>.slotN.pss" (psx/state.c:1268). The
           inherited PS2 pattern looked for a two-digit slot in a ".p2s" — wrong extension AND
           wrong slot form, so the Save Manager listed nothing at all. Slot is 1+ digits and
           carries the "slot" prefix. */
        val SLOT_PATTERN = Regex("\\.slot([0-9]+)\\.pss$", RegexOption.IGNORE_CASE)
        val CRC_SUFFIX = Regex("-[0-9a-f]{8}$", RegexOption.IGNORE_CASE)
    }
}
