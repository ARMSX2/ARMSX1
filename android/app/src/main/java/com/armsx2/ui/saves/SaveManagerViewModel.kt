package com.armsx2.ui.saves

import android.app.Application
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.armsx2.i18n.I18n
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.core.PsxStateFiles
import dev.psxlab.BackendStateStore
import com.armsx2.data.library.GameLibraryRepository
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
    val gameSource: String? = null,
    val canLoad: Boolean = canUseWithActiveGame,
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
        if (!item.canLoad) return
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
            } else if (item.gameSource?.let { source ->
                    val game = withContext(Dispatchers.IO) { MainActivityRuntime.resolveSavedGame(source) }
                    game != null && MainActivityRuntime.launchGameFromSaveSlot(game, slot)
                } == true) {
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
                PsxStateFiles.companions(item.file.name).forEach { name ->
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
                        PsxStateFiles.companions(file.name).forEach { name ->
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

        val root = File(MainActivityRuntime.assetCopyRoot(getApplication()), "psxlab/states")
        val library = GameLibraryRepository(getApplication()).loadCached().games
        val sources = (library.map { it.uri.toString() } + listOfNotNull(active?.uri?.toString())).distinct()
        val files = (activePaths + root.listFiles().orEmpty().filter { it.isFile })
            .distinctBy { it.absolutePath }
        val hasActiveVm = runCatching { NativeApp.hasActiveVM() }.getOrDefault(false)
        val saves = files.mapNotNull { file ->
            val metadata = runCatching { BackendStateStore.metadata(file) }.getOrNull() ?: return@mapNotNull null
            val belongsToActiveGame = active != null && activePaths.any { it.absolutePath == file.absolutePath }
            if (active != null && !belongsToActiveGame) return@mapNotNull null
            val source = PsxStateFiles.source(metadata.game(), sources)
            val game = library.firstOrNull { BackendStateStore.normalize(it.uri.toString()) == source?.let(BackendStateStore::normalize) }
            val name = source?.let { android.net.Uri.decode(it).substringAfterLast('/').substringAfterLast(':').substringBeforeLast('.') }
            SaveStateItem(
                slot = metadata.slot(),
                file = file,
                gameTitle = if (belongsToActiveGame) active?.title.orEmpty() else game?.title ?: name ?: "Existing save (select its game to load)",
                serial = game?.serial.orEmpty(),
                preview = decodePreview(file),
                canUseWithActiveGame = belongsToActiveGame && hasActiveVm,
                gameSource = source,
                canLoad = belongsToActiveGame || (!hasActiveVm && source != null),
            )
        }.sortedWith(compareByDescending<SaveStateItem> { item ->
                item.gameSource?.let { source -> BackendStateStore.file(root.parentFile, source, item.slot!!) == item.file } ?: false
            }.thenByDescending { it.file.lastModified() })
            .distinctBy { (it.gameSource ?: it.file.absolutePath) to it.slot }
            .sortedWith(compareBy<SaveStateItem> { it.gameTitle }.thenBy { it.slot })

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

    private companion object {
        const val SLOT_COUNT = 10

    }
}
