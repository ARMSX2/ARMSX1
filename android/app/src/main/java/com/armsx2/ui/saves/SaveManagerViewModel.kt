package com.armsx2.ui.saves

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.core.SwanStateFiles
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
        if (!item.canUseWithActiveGame) return
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
            val ok = withContext(Dispatchers.IO) { runCatching {
                check(item.file.delete()) { "Unable to delete state" }
                SwanStateFiles.companions(item.file.name).forEach { name ->
                    val sidecar = File(item.file.parentFile, name)
                    if (sidecar.exists()) check(sidecar.delete()) { "Unable to delete $name" }
                }
                true
            }.getOrDefault(false) }
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
                        SwanStateFiles.companions(file.name).forEach { name ->
                            val sidecar = File(file.parentFile, name)
                            if (sidecar.isFile) sidecar.copyTo(File(destination, name), overwrite = false)
                        }
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
        val activePaths = (-1 until SLOT_COUNT).mapNotNull { slot ->
            runCatching { NativeApp.getSwanStatePath(slot) }
                .getOrNull()
                ?.takeIf(String::isNotBlank)
                ?.let(::File)
                ?.takeIf(File::exists)
        }

        val roots = listOf("swanstation/states")
            .map { File(MainActivityRuntime.assetCopyRoot(getApplication()), it) }
        val discovered = roots.flatMap { root ->
            if (!root.isDirectory) emptyList()
            else root.listFiles()?.filter { it.isFile && SwanStateFiles.slot(it.name) != null }.orEmpty()
        }
        val allFiles = (activePaths + discovered)
            .distinctBy { it.absolutePath.lowercase() }
            .filter { file -> active == null || activePaths.any { it.absolutePath == file.absolutePath } }

        val saves = allFiles.map { file ->
            val serial = serialFrom(file)
            val belongsToActiveGame = active != null && activePaths.any { it.absolutePath == file.absolutePath }
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

    private fun slotFrom(file: File): Int? = SwanStateFiles.slot(file.name)

    private fun serialFrom(file: File): String = SwanStateFiles.identity(file.name)

    private companion object {
        const val SLOT_COUNT = 10

    }
}
