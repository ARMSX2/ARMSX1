package com.armsx2.ui.textures

import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import com.armsx2.cheats.Ps1CheatDatabaseIndex
import com.armsx2.cheats.Ps1CheatIdentity
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.core.Ps1Textures
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.textures.TexturePack
import com.armsx2.textures.TexturePackRepository
import com.armsx2.textures.TexturePackSelection
import com.armsx2.ui.cheats.CheatSearchBar
import com.armsx2.ui.home.LibraryKeyboard
import com.armsx2.ui.settings.LocalPs1SettingsScope
import com.armsx2.ui.settings.Ps1ActionRow
import com.armsx2.ui.settings.Ps1SettingsEditor
import com.armsx2.ui.settings.Ps1SettingsScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

@Composable
internal fun TexturePacksSection(editor: Ps1SettingsEditor) {
    val scope = LocalPs1SettingsScope.current
    key(scope?.gameKey, scope?.gameUri) { TexturePacksBody(editor, scope) }
}

@Composable
private fun TexturePacksBody(editor: Ps1SettingsEditor, settingsScope: Ps1SettingsScope?) {
    val context = LocalContext.current.applicationContext
    val coroutineScope = rememberCoroutineScope()
    val currentEditor by rememberUpdatedState(editor)
    var expanded by remember { mutableStateOf(false) }
    var packs by remember { mutableStateOf<List<TexturePack>>(emptyList()) }
    var installed by remember { mutableStateOf<Set<String>>(emptySet()) }
    var serial by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf<String?>(null) }
    var failure by remember { mutableStateOf(false) }
    var progress by remember { mutableIntStateOf(-1) }
    var refreshTick by remember { mutableIntStateOf(0) }
    var query by remember { mutableStateOf("") }
    var allGames by remember { mutableStateOf(false) }
    var limit by remember(query, allGames) { mutableIntStateOf(20) }
    val searchHint = str("textures.search")

    LaunchedEffect(expanded, refreshTick) {
        if (!expanded) return@LaunchedEffect
        busy = true
        try {
            val result = withContext(Dispatchers.IO) {
                val coroutine = currentCoroutineContext()
                val serial = if (settingsScope?.gameKey != null &&
                    MainActivityRuntime.currentGame.value?.settingsKey == settingsScope.gameKey)
                    Ps1CheatDatabaseIndex.normaliseSerial(NativeApp.getGameSerial())
                else if (settingsScope?.gameUri != null)
                    Ps1CheatIdentity.resolve(context, settingsScope.gameUri) { !coroutine.isActive }
                else Ps1CheatDatabaseIndex.normaliseSerial(settingsScope?.gameKey)
                val entries = TexturePackRepository.load(context)
                Triple(entries, entries.filter { TexturePackRepository.installed(context, it) }
                    .mapTo(hashSetOf()) { it.storageKey }, serial)
            }
            packs = result.first; installed = result.second; serial = result.third
        } catch (cancelled: CancellationException) { throw cancelled }
        catch (_: Exception) { status = "textures.loadFailed"; failure = true }
        finally { busy = false }
    }

    fun usePack(pack: TexturePack) {
        check(settingsScope?.gameKey != null && pack.matches(serial))
        val directory = TexturePackRepository.directory(context, pack).absolutePath
        Ps1SettingsStore.update(context, settingsScope.gameKey) {
            TexturePackSelection.updated(it, settingsScope.gameKey, serial, pack, directory)
        }
        currentEditor.reload()
        // Resolve the active game, not necessarily the game whose settings are being edited.
        Ps1Textures.push(context)
    }

    Column {
        Ps1ActionRow(label = str(if (expanded) "textures.browser.close" else "textures.browser"),
            description = str("textures.browser.description"), controllerId = "ps1.textures.browser") {
            if (!busy) expanded = !expanded
        }
        if (expanded) {
            Ps1ActionRow(str(if (busy) "textures.busy" else "textures.refresh"), controllerId = "ps1.textures.refresh") {
                if (!busy) {
                    busy = true; failure = false; status = null; progress = -1
                    coroutineScope.launch {
                        try {
                            TexturePackRepository.refresh(context)
                            status = "textures.refreshed"; refreshTick++
                        } catch (cancelled: CancellationException) { throw cancelled }
                        catch (_: Exception) { status = "textures.refreshFailed"; failure = true }
                        finally { busy = false }
                    }
                }
            }
            if (settingsScope?.gameKey == null) Text(str("textures.chooseGame"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            else if (serial == null && !busy) Text(str("textures.unknownGame"), color = MaterialTheme.colorScheme.error)
            else serial?.let { Text(str("textures.gameId").format(it), style = MaterialTheme.typography.bodySmall) }
            if (editor.value.gpuBackend == Ps1Settings.GPU_SOFTWARE) Text(str("textures.hardwareRequired"),
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            status?.let { Text(str(it), color = if (failure) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant) }
            if (progress >= 0) Text(str("textures.progress").format(progress), style = MaterialTheme.typography.bodySmall)
            if (packs.isEmpty() && !busy) Text(str("textures.empty"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (packs.isNotEmpty()) {
                if (serial != null) Ps1ActionRow(str(if (allGames) "textures.matches" else "textures.allGames"),
                    controllerId = "ps1.textures.scope") { allGames = !allGames }
                CheatSearchBar(query, searchHint, "ps1.textures.search", onClear = { query = "" }) {
                    LibraryKeyboard.open(initial = query, onChange = { query = it }, placeholder = searchHint)
                }
                val results = packs.filter {
                    (serial == null || allGames || it.matches(serial)) &&
                        (it.title + " " + it.serials.joinToString(" ") + " " + it.author).contains(query.trim(), ignoreCase = true)
                }
                if (results.isEmpty()) Text(str("textures.noMatches"), color = MaterialTheme.colorScheme.onSurfaceVariant)
                results.take(limit).forEach { pack ->
                    val local = pack.storageKey in installed
                    val canUse = settingsScope?.gameKey != null && pack.matches(serial)
                    val selected = local && editor.value.textureReplacements &&
                        editor.value.textureDir == TexturePackRepository.directory(context, pack).absolutePath
                    Text("${pack.title} · ${pack.version}", style = MaterialTheme.typography.titleSmall)
                    Text("${pack.author} · ${pack.license}\n${pack.sourceUrl}", style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Ps1ActionRow(
                        label = str(when { selected -> "textures.selected"; local && canUse -> "textures.use";
                            local -> "textures.installed"; canUse -> "textures.downloadUse"; else -> "textures.download" }),
                        controllerId = "ps1.textures.${pack.id}.${pack.sha256}",
                        description = str("textures.size").format(pack.downloadBytes / (1024.0 * 1024.0), pack.expandedBytes / (1024.0 * 1024.0)) +
                            if (serial != null && !pack.matches(serial)) " · " + str("textures.otherRegion") else "",
                    ) {
                        if (!busy && !selected && (!local || canUse)) {
                            busy = true; failure = false; status = null; progress = -1
                            coroutineScope.launch {
                                try {
                                    TexturePackRepository.install(context, pack) { done, total ->
                                        withContext(Dispatchers.Main) { progress = (done * 100 / total).toInt() }
                                    }
                                    installed = installed + pack.storageKey
                                    if (canUse) usePack(pack)
                                    status = if (canUse) "textures.applied" else "textures.downloaded"
                                    refreshTick++
                                } catch (cancelled: CancellationException) { throw cancelled }
                                catch (_: Exception) { status = "textures.installFailed"; failure = true }
                                finally { busy = false; progress = -1 }
                            }
                        }
                    }
                }
                if (results.size > limit) Ps1ActionRow(str("textures.more"), controllerId = "ps1.textures.more") { limit += 20 }
            }
        }
    }
}
