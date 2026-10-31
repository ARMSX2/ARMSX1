package com.armsx2.update

import android.content.Context
import android.content.Intent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.armsx2.BuildConfig
import com.armsx2.i18n.str
import com.armsx2.runtime.MainActivityRuntime
import com.armsx2.ui.common.GlassPanel
import com.armsx2.ui.common.SettingSwitchRow
import com.armsx2.ui.settings.controllerFocusable
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.security.MessageDigest
import java.util.zip.ZipFile
import android.content.pm.PackageManager

/**
 * In-app updater — GitHub sideload flavor ONLY. Renders a "Check for updates" panel at the top of
 * the App (General) settings tab: it reads the GitHub releases feed, compares each release tag
 * against the installed build, and offers to download + install the matching arm64 APK.
 *
 * The play flavor gets the no-op stub in src/play, so this code — and REQUEST_INSTALL_PACKAGES and
 * the FileProvider, which live in src/github/AndroidManifest.xml — never enters a Play artifact.
 * :app:verifyPlayFlavorCleanDebug/Release finalize every play assemble/bundle task and fail the
 * build if either reaches a play artifact; build-play-aab.sh re-checks the finished .aab.
 *
 * ORG vs REPO: the GitHub org is ARMSX2 and this repo is ARMSX1 — `ARMSX2/ARMSX1`. Neither
 * `ARMSX1/ARMSX1` nor `ARMSX2/ARMSX2` (the PS2 sibling) is this app.
 *
 * Stable releases use semantic tags; nightly CI uses dated tags and timestamp version codes.
 * UpdatePolicy keeps these channels separate, and archive identity is checked before installation.
 */

private const val RELEASES_URL = "https://api.github.com/repos/ARMSX2/ARMSX1/releases?per_page=100"

private sealed interface UpdateState {
    data object Idle : UpdateState
    data object Checking : UpdateState
    data object UpToDate : UpdateState
    data class Available(val version: String, val notes: String, val apkUrl: String,
        val bytes: Long = 0, val digest: String = "") : UpdateState
    data class Downloading(val pct: Int) : UpdateState
    data class Error(val msg: String) : UpdateState
}

@Composable
fun UpdaterEntry() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var state by remember { mutableStateOf<UpdateState>(UpdateState.Idle) }
    // str() is @Composable, so resolve the strings the background check / onClick handlers need
    // here and capture them (they run outside composition).
    val checkFailedPrefix = str("update.checkFailed")
    val downloadFailedPrefix = str("update.downloadFailed")

    GlassPanel(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp)) {
        Column(Modifier.padding(4.dp)) {
            Text(str("update.title"), style = MaterialTheme.typography.titleMedium)
            Text(
                "${str("update.currentVersion")}: ${BuildConfig.VERSION_NAME}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            when (val s = state) {
                is UpdateState.Checking -> Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(8.dp))
                    Text(str("update.checking"), style = MaterialTheme.typography.bodySmall)
                }
                is UpdateState.UpToDate -> Text(
                    str("update.upToDate"),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.primary,
                )
                is UpdateState.Error -> Text(
                    s.msg,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
                is UpdateState.Downloading -> Column {
                    Text("${str("update.downloading")} ${s.pct}%", style = MaterialTheme.typography.bodySmall)
                    Spacer(Modifier.height(4.dp))
                    LinearProgressIndicator(
                        progress = { s.pct / 100f },
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
                else -> {}
            }
            Spacer(Modifier.height(8.dp))
            // Extracted so the controller's confirm and the touch onClick share one action, and the
            // button joins the settings nav registry under the id "update.check".
            val runCheck: () -> Unit = {
                scope.launch {
                    state = UpdateState.Checking
                    state = checkForUpdate(
                        MainActivityRuntime.prefs.getBoolean("update.includeNightly", false),
                        checkFailedPrefix,
                    )
                }
            }
            Button(
                enabled = state !is UpdateState.Checking && state !is UpdateState.Downloading,
                onClick = runCheck,
                modifier = Modifier.controllerFocusable("update.check", onConfirm = runCheck),
            ) { Text(str("update.check")) }

            // Opt-in: silently check GitHub for a newer release on every app launch (default off).
            var checkOnLaunch by remember {
                mutableStateOf(MainActivityRuntime.prefs.getBoolean("update.checkOnLaunch", false))
            }
            SettingSwitchRow(
                title = str("update.checkOnLaunch"),
                description = str("update.checkOnLaunch.desc"),
                checked = checkOnLaunch,
                onCheckedChange = {
                    checkOnLaunch = it
                    MainActivityRuntime.prefs.edit().putBoolean("update.checkOnLaunch", it).apply()
                },
            )

            // Opt-in: also consider GitHub pre-releases when checking (default off).
            var includeNightly by remember {
                mutableStateOf(MainActivityRuntime.prefs.getBoolean("update.includeNightly", false))
            }
            SettingSwitchRow(
                title = str("update.includeNightly"),
                description = str("update.includeNightly.desc"),
                checked = includeNightly,
                onCheckedChange = {
                    includeNightly = it
                    MainActivityRuntime.prefs.edit().putBoolean("update.includeNightly", it).apply()
                },
            )
        }
    }

    (state as? UpdateState.Available)?.let { avail ->
        AlertDialog(
            onDismissRequest = { state = UpdateState.Idle },
            title = { Text("${str("update.available")}  ${avail.version}") },
            text = {
                Column(Modifier.heightIn(max = 320.dp).verticalScroll(rememberScrollState())) {
                    Text(
                        avail.notes.ifBlank { str("update.notesUnavailable") },
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    scope.launch {
                        try {
                            state = UpdateState.Downloading(0)
                            downloadAndInstall(context, avail) { pct -> state = UpdateState.Downloading(pct) }
                            state = UpdateState.Idle
                        } catch (e: Exception) {
                            if (e is CancellationException) throw e
                            state = UpdateState.Error("$downloadFailedPrefix: ${e.message}")
                        }
                    }
                }) { Text(str("update.install")) }
            },
            dismissButton = {
                TextButton(onClick = { state = UpdateState.Idle }) { Text(str("action.cancel")) }
            },
        )
    }
}

/**
 * Boot-time auto-check (github flavor only). Mounted once at the app root; when the "check on
 * launch" toggle is on, it runs a single silent GitHub check on start and pops the update prompt
 * ONLY if a newer release exists — no "up to date" popup, no noise on every boot. Reuses the exact
 * check/download/install path as the manual button.
 */
@Composable
fun AutoUpdateGate() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var state by remember { mutableStateOf<UpdateState>(UpdateState.Idle) }
    val checkFailedPrefix = str("update.checkFailed")
    val downloadFailedPrefix = str("update.downloadFailed")

    LaunchedEffect(Unit) {
        if (MainActivityRuntime.prefs.getBoolean("update.checkOnLaunch", false)) {
            val result = checkForUpdate(
                MainActivityRuntime.prefs.getBoolean("update.includeNightly", false),
                checkFailedPrefix,
            )
            if (result is UpdateState.Available) state = result // stay silent on up-to-date / errors
        }
    }

    val s = state
    if (s is UpdateState.Available || s is UpdateState.Downloading) {
        val avail = s as? UpdateState.Available
        AlertDialog(
            onDismissRequest = { if (state !is UpdateState.Downloading) state = UpdateState.Idle },
            title = {
                Text(
                    if (state is UpdateState.Downloading) str("update.downloading")
                    else "${str("update.available")}  ${avail?.version.orEmpty()}",
                )
            },
            text = {
                when (val cur = state) {
                    is UpdateState.Available -> Column(
                        Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState()),
                    ) {
                        Text(
                            cur.notes.ifBlank { str("update.notesUnavailable") },
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    is UpdateState.Downloading -> Column {
                        Text("${cur.pct}%", style = MaterialTheme.typography.bodySmall)
                        Spacer(Modifier.height(6.dp))
                        LinearProgressIndicator(
                            progress = { cur.pct / 100f },
                            modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    else -> {}
                }
            },
            confirmButton = {
                if (avail != null) {
                    TextButton(onClick = {
                        scope.launch {
                            try {
                                state = UpdateState.Downloading(0)
                                downloadAndInstall(context, avail) { pct -> state = UpdateState.Downloading(pct) }
                                state = UpdateState.Idle
                            } catch (e: Exception) {
                                if (e is CancellationException) throw e
                                state = UpdateState.Error("$downloadFailedPrefix: ${e.message}")
                            }
                        }
                    }) { Text(str("update.install")) }
                }
            },
            dismissButton = {
                if (state is UpdateState.Available) {
                    TextButton(onClick = { state = UpdateState.Idle }) { Text(str("update.later")) }
                }
            },
        )
    }
    (state as? UpdateState.Error)?.let { error ->
        AlertDialog(onDismissRequest = { state = UpdateState.Idle },
            title = { Text(str("update.downloadFailed")) }, text = { Text(error.msg) },
            confirmButton = { TextButton(onClick = { state = UpdateState.Idle }) { Text(str("update.later")) } })
    }
}

/**
 * Read the bounded, paginated release history, then select by channel/version, not feed order.
 *
 * Drafts are always skipped. Pre-releases are skipped unless [includePrerelease]. Only the
 * releases list endpoint is used (never /releases/latest): /latest hides pre-releases entirely and
 * can point at a release carrying no APK at all, which would need a second code path to recover
 * from.
 */
private suspend fun checkForUpdate(
    includePrerelease: Boolean,
    checkFailedPrefix: String,
): UpdateState = withContext(Dispatchers.IO) {
    try {
        val releases = ArrayList<UpdatePolicy.Release>()
        for (page in 1..5) {
            val arr = JSONArray(httpGet("$RELEASES_URL&page=$page"))
            for (i in 0 until arr.length()) {
                val rel = arr.optJSONObject(i) ?: continue
                if (rel.optBoolean("draft", false)) continue
                val asset = pickApkAsset(rel) ?: continue
                val published = runCatching { Instant.parse(rel.optString("published_at")).epochSecond }.getOrDefault(0L)
                releases += UpdatePolicy.Release(rel.optString("tag_name").trim(),
                    rel.optBoolean("prerelease", false), published,
                    asset.optString("browser_download_url"), rel.optString("body", ""),
                    asset.optLong("size", 0L), asset.optString("digest", ""))
            }
            if (arr.length() < 100) break
            if (page == 5) throw java.io.IOException("Release history is too large to check completely")
        }
        val selected = UpdatePolicy.select(releases, BuildConfig.VERSION_NAME, BuildConfig.VERSION_CODE.toLong(), includePrerelease)
        if (selected == null) UpdateState.UpToDate else UpdateState.Available(
            selected.tag, selected.notes, selected.url, selected.bytes, selected.digest)
    } catch (e: Exception) {
        if (e is CancellationException) throw e
        UpdateState.Error("$checkFailedPrefix: ${e.message}")
    }
}

/**
 * The APK asset this device can run, or null if the release has none.
 *
 * Only `.apk` names are considered, which drops `.idsig`, `.aab`, checksum files and source
 * archives. Anything naming a non-arm64 ABI is then discarded outright — handing the package
 * installer an armeabi-v7a or x86 APK produces an install failure the user cannot act on. Of what
 * survives, an explicitly arm64-named asset wins; otherwise the first remaining APK is taken, which
 * is the single universal/arm64-only artifact this app actually ships.
 */
private fun pickApkAsset(release: JSONObject): JSONObject? {
    val assets = release.optJSONArray("assets") ?: return null
    val candidates = ArrayList<JSONObject>()
    for (i in 0 until assets.length()) {
        val a = assets.optJSONObject(i) ?: continue
        candidates += a
    }
    if (candidates.isEmpty()) return null
    val selected = UpdatePolicy.pickAsset(candidates.map {
        UpdatePolicy.Asset(it.optString("name"), it.optString("browser_download_url"))
    }) ?: return null
    return candidates.first { it.optString("browser_download_url") == selected.url }
}

/**
 * The response code is checked rather than assumed: `ARMSX2/ARMSX1` is not publicly readable at the
 * time of writing, and an unchecked `inputStream` on a 404 throws a bare FileNotFoundException whose
 * message is the URL — indistinguishable from being offline. Surfacing "HTTP 404" tells the user
 * (and us) which of the two it is.
 */
private fun httpGet(url: String): String {
    val conn = URL(url).openConnection() as HttpURLConnection
    return try {
        conn.connectTimeout = 15_000
        conn.readTimeout = 20_000
        conn.instanceFollowRedirects = true
        conn.useCaches = false
        conn.setRequestProperty("Cache-Control", "no-cache")
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        conn.setRequestProperty("User-Agent", USER_AGENT)
        val code = conn.responseCode
        if (code != HttpURLConnection.HTTP_OK) throw java.io.IOException("HTTP $code")
        conn.inputStream.bufferedReader().use { it.readText() }
    } finally {
        conn.disconnect()
    }
}

private val USER_AGENT: String = "ARMSX1-Updater/" + runCatching { BuildConfig.VERSION_NAME }.getOrDefault("dev")

private suspend fun downloadAndInstall(
    context: Context,
    info: UpdateState.Available,
    onProgress: (Int) -> Unit,
) {
    val apk = withContext(Dispatchers.IO) {
        val dir = File(context.externalCacheDir ?: context.cacheDir, "updates").apply {
            if (!mkdirs() && !isDirectory) throw java.io.IOException("Unable to create update cache")
        }
        val out = File(dir, "armsx1-update.apk")
        val partial = File(dir, "armsx1-update.part.apk")
        val conn = URL(info.apkUrl).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 15_000
            conn.readTimeout = 30_000
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", USER_AGENT)
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw java.io.IOException("HTTP $code")
            val total = conn.contentLengthLong
            var read = 0L
            var lastPct = -1
            conn.inputStream.use { input ->
                partial.outputStream().use { sink ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        currentCoroutineContext().ensureActive()
                        val n = input.read(buf)
                        if (n < 0) break
                        sink.write(buf, 0, n)
                        read += n
                        if (read > 512L * 1024 * 1024) throw java.io.IOException("Update APK exceeds the size limit")
                        if (total > 0) {
                            val pct = ((read * 100) / total).toInt()
                            if (pct != lastPct) {
                                lastPct = pct
                                withContext(Dispatchers.Main) { onProgress(pct) }
                            }
                        }
                    }
                }
            }
            if (read == 0L || (info.bytes > 0 && read != info.bytes) || (total > 0 && read != total))
                throw java.io.IOException("Update download is incomplete; please retry")
            if (info.digest.startsWith("sha256:")) {
                val digest = MessageDigest.getInstance("SHA-256")
                partial.inputStream().use { input ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) { val n = input.read(buffer); if (n < 0) break; digest.update(buffer, 0, n) }
                }
                val actual = digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
                if (!actual.equals(info.digest.removePrefix("sha256:"), true)) throw java.io.IOException("Update checksum does not match GitHub")
            }
            validateUpdateArchive(context, partial)
            if (out.exists() && !out.delete()) throw java.io.IOException("Unable to replace cached update")
            if (!partial.renameTo(out)) throw java.io.IOException("Unable to finish update download")
        } finally {
            conn.disconnect()
            partial.delete()
        }
        out
    }
    // Hand the APK to the system package installer (the user confirms the install there, and the
    // installer is what enforces signature match and versionCode monotonicity).
    val uri = FileProvider.getUriForFile(context, "${context.packageName}.updateprovider", apk)
    val intent = Intent(Intent.ACTION_VIEW).apply {
        setDataAndType(uri, "application/vnd.android.package-archive")
        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
    }
    context.startActivity(intent)
}

@Suppress("DEPRECATION")
private fun validateUpdateArchive(context: Context, apk: File) {
    val pm = context.packageManager
    val downloaded = pm.getPackageArchiveInfo(apk.absolutePath, PackageManager.GET_SIGNING_CERTIFICATES)
        ?: throw java.io.IOException("Downloaded file is not a readable APK")
    val current = pm.getPackageInfo(context.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
    fun certificates(info: android.content.pm.PackageInfo) = info.signingInfo?.apkContentsSigners
        ?.map { signature -> MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
            .joinToString("") { "%02x".format(it.toInt() and 255) } }?.toSet().orEmpty()
    // A newer version number must not replace the original engine with an old-core APK.
    val compatibleEngine = !BuildConfig.PSXLAB_BACKEND || ZipFile(apk).use { zip ->
        listOf("psxcore", "psxgpu", "psxdisc", "psxtempo").all { zip.getEntry("lib/arm64-v8a/lib$it.so") != null }
    }
    UpdatePolicy.archiveError(
        UpdatePolicy.Archive(context.packageName, current.longVersionCode, certificates(current)),
        UpdatePolicy.Archive(downloaded.packageName, downloaded.longVersionCode, certificates(downloaded), compatibleEngine),
    )?.let { throw java.io.IOException(it) }
}
