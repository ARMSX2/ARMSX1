package com.armsx2.ui.cheats

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import com.armsx2.cheats.Ps1CheatDownloader
import com.armsx2.cheats.Ps1CheatDatabase
import com.armsx2.i18n.str
import com.armsx2.ui.settings.Ps1ActionRow
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Shared by the global browser and per-game Cheats; no automatic network activity. */
@Composable
internal fun CheatDownloadButton(
    blocked: Boolean = false,
    onBusyChanged: (Boolean) -> Unit = {},
    onDownloaded: () -> Unit,
) {
    val context = LocalContext.current.applicationContext
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    var messageKey by remember { mutableStateOf<String?>(null) }
    var revision by remember { mutableStateOf<String?>(null) }
    var notice by remember { mutableStateOf("") }
    var showNotice by remember { mutableStateOf(false) }
    var refresh by remember { mutableIntStateOf(0) }
    LaunchedEffect(refresh) {
        val details = withContext(Dispatchers.IO) {
            runCatching { Ps1CheatDatabase.downloadedRevision(context) to Ps1CheatDatabase.notice(context) }.getOrNull()
        }
        revision = details?.first
        notice = details?.second.orEmpty()
    }
    val busyCallback by rememberUpdatedState(onBusyChanged)
    val downloadedCallback by rememberUpdatedState(onDownloaded)
    Ps1ActionRow(
        label = str(if (busy) "cheats.download.busy" else "cheats.download.button"),
        description = str("cheats.download.description"),
        controllerId = "ps1.cheats.download",
    ) {
        if (!busy && !blocked) {
            busy = true
            messageKey = null
            busyCallback(true)
            scope.launch {
                try {
                    val changed = Ps1CheatDownloader.download(context)
                    messageKey = if (changed) "cheats.download.success" else "cheats.download.current"
                    refresh++
                    downloadedCallback()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    messageKey = "cheats.download.failed"
                } finally {
                    busy = false
                    busyCallback(false)
                }
            }
        }
    }
    messageKey?.let { Text(str(it), color = if (it == "cheats.download.failed")
        MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant) }
    revision?.let {
        Text(str("cheats.download.revision").format(it), style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    if (notice.isNotBlank()) {
        Ps1ActionRow(label = str(if (showNotice) "cheats.download.notice.close" else "cheats.download.notice"),
            controllerId = "ps1.cheats.download.notice") { showNotice = !showNotice }
        if (showNotice) Text(notice, style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
