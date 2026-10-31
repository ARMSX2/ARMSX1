package com.armsx2

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Process
import android.os.ParcelFileDescriptor
import android.os.PowerManager
import android.system.Os
import android.system.OsConstants
import android.view.InputDevice
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.BufferedWriter
import java.io.File
import java.io.RandomAccessFile
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.TimeUnit

/**
 * Opt-in local diagnostics with a separate user-selected export destination.
 *
 * Uses the active PSX Lab engine's bounded session journal and process-filtered logcat.
 * Session metadata identifies the game, build, renderer and timing source. Older saved
 * sessions are explicitly recovery/history data. No legacy emulator logs are imported.
 * Startup refreshes a private report; explicit export writes the chosen destination.
 *
 * Prefs are read through the app's canonical "ARMSX2" SharedPreferences file directly (not
 * [com.armsx2.runtime.MainActivityRuntime.prefs]) because the startup refresh runs from
 * [Pasx2Application.onCreate], before the runtime initializes.
 */
object DiagnosticsReport {

    /** Per-section cap so one chatty log cannot make the report unshareable. */
    private const val TAIL_LIMIT = 512 * 1024
    private const val CRASH_FILES = 3
    private const val LOG_LEVEL_INFO = 2

    private const val KEY_ENABLED = "diagnostics.generateLog"
    private const val KEY_URI = "diagnostics.reportUri"
    private val exportGate = DiagnosticExportGate()

    private fun prefs(context: Context) =
        context.getSharedPreferences("ARMSX2", Context.MODE_PRIVATE)

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_ENABLED, false)

    private fun reportUri(context: Context): Uri? =
        prefs(context).getString(KEY_URI, null)?.let { runCatching { Uri.parse(it) }.getOrNull() }

    /** Capture locally without opening a document picker or enabling high-frequency trace. */
    fun enable(context: Context) {
        prefs(context).edit().putBoolean(KEY_ENABLED, true).apply()
        runCatching { NativeApp.setDiagnosticCapture(true) }
    }
    fun setExportDestination(context: Context, target: Uri) {
        runCatching { context.contentResolver.takePersistableUriPermission(target,
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
        prefs(context).edit().putString(KEY_URI, target.toString()).apply()
    }
    /** Advanced engine logging remains a separate user preference. */
    fun disable(context: Context) {
        prefs(context).edit().putBoolean(KEY_ENABLED, false).apply()
        runCatching { NativeApp.setDiagnosticCapture(false) }
    }

    /**
     * Rewrite the report at the chosen destination. Concurrent exports return BUSY.
     * Blocking: call from an IO thread.
     */
    fun refresh(context: Context, export: Boolean = true): DiagnosticExportGate.Result {
        if (!export && !isEnabled(context) && !BuildConfig.DIAGNOSTIC_BUILD) return DiagnosticExportGate.Result.FAILED
        if (!DiagnosticCapture.isMainProcess(context)) return DiagnosticExportGate.Result.FAILED
        val local = File(context.filesDir, "logs/diagnostic-report.txt")
        val exportRequested = export
        val uri = if (exportRequested) reportUri(context) else null
        if (exportRequested && uri == null) return DiagnosticExportGate.Result.FAILED
        if (uri == null) {
            return if (capture(context, local)) DiagnosticExportGate.Result.SUCCESS else DiagnosticExportGate.Result.FAILED
        }
        return exportGate.run {
            if (!capture(context, local)) return@run false
            // A stable provider reference lets Android kill this process when the provider
            // dies. Keep this optional export on an unstable client: provider death becomes
            // a failed export handled by exportGate, with the local report still intact.
            val client = context.contentResolver.acquireUnstableContentProviderClient(uri)
                ?: return@run false
            client.use {
                val descriptor = it.openFile(uri, "wt", null) ?: return@run false
                ParcelFileDescriptor.AutoCloseOutputStream(descriptor).use { output ->
                    local.inputStream().use { input -> input.copyTo(output) }
                }
            }
            true
        }
    }

    private fun capture(context: Context, local: File): Boolean = synchronized(this) {
        runCatching {
            local.parentFile?.mkdirs()
            val pending = File(local.parentFile, "diagnostic-report.pending")
            pending.bufferedWriter().use { writeReport(context, it) }
            check(pending.renameTo(local))
            true
        }.getOrDefault(false)
    }

    private fun writeReport(context: Context, w: BufferedWriter) {
        runCatching { NativeApp.flushEngineLog(1500) }
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        val externalLogs = File(context.getExternalFilesDir(null) ?: context.filesDir, "logs")

        w.appendLine("ARMSX1 diagnostic report $stamp")
        w.appendLine(
            "app: ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE}) " +
                "${BuildConfig.FLAVOR}-${BuildConfig.BUILD_TYPE}",
        )
        w.appendLine(
            "device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}/${Build.BOARD}) " +
                "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            w.appendLine("soc: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        }
        w.appendLine("source commit: ${BuildConfig.SOURCE_COMMIT}")
        w.appendLine("diagnostic build: ${BuildConfig.DIAGNOSTIC_BUILD}")
        if (BuildConfig.PACING_DIAGNOSTICS) {
            w.appendLine("pacing diagnostic patch: pacing-probe-v1 (local changes over source commit)")
            w.appendLine("Pacing capture is active; broad diagnostic logcat/heartbeat capture is not required.")
            val manager = context.getSystemService(android.hardware.display.DisplayManager::class.java)
            runCatching { manager.displays.forEach { display ->
                w.appendLine("display ${display.displayId}: ${display.name} actual_hz=${display.refreshRate} " +
                    "mode=${display.mode.modeId} size=${display.mode.physicalWidth}x${display.mode.physicalHeight}")
                w.appendLine("supported modes: " + display.supportedModes.joinToString { mode ->
                    "${mode.modeId}:${mode.physicalWidth}x${mode.physicalHeight}@${mode.refreshRate}"
                })
            } }.onFailure { w.appendLine("Display mode metadata unavailable: ${it.javaClass.simpleName}") }
        }
        w.appendLine("abis: ${Build.SUPPORTED_ABIS.joinToString()}")
        w.appendLine("page size: ${runCatching { Os.sysconf(OsConstants._SC_PAGESIZE) }.getOrDefault(-1L)}")
        w.appendLine("free app storage: ${context.filesDir.usableSpace}")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val thermal = runCatching {
                context.getSystemService(PowerManager::class.java).currentThermalStatus
            }.getOrDefault(-1)
            w.appendLine("thermal status at export: $thermal (0=none, 1=light, 2=moderate, " +
                "3=severe, 4=critical, 5=emergency, 6=shutdown, -1=unavailable)")
        }
        val coreLogging = BuildConfig.DIAGNOSTIC_BUILD ||
            isEnabled(context) || runCatching { Ps1SettingsStore.active(context).loggingEnabled }.getOrDefault(false)
        w.appendLine("core: PSX Lab (original engine)")
        w.appendLine("core logging preference: " + if (coreLogging) "on" else "off")
        w.appendLine("process id at export: ${Process.myPid()}")
        w.appendLine("Configuration files show requested settings, not proof of the active renderer.")
        w.appendLine()

        w.appendLine("===== current PSX Lab session in this process =====")
        // Read the adapter's cached counters; do not schedule work on the VM thread.
        val current = run {
            runCatching { NativeApp.getEngineDiagnostics() }.getOrElse {
                "Native session snapshot unavailable: ${it.javaClass.simpleName}"
            }
        }
        if (current.isNotBlank()) {
            w.appendLine(current)
            w.appendLine("Session state above identifies whether this is running, paused or ended.")
        } else {
            w.appendLine("No native session snapshot in this process. Historical files below are NOT current measurements.")
        }
        w.appendLine()
        section(w, "saved settings.toml (requested configuration)", File(context.filesDir, "settings.toml"))
        val engineLogs = File(context.filesDir, "logs/psxlab")
        section(w, "PSX Lab session checkpoint (historical, timestamped)", File(engineLogs,"last-session.json"))
        section(w, "PSX Lab structured engine log (current tail)", File(engineLogs,"engine-current.jsonl"))
        section(w, "PSX Lab structured engine log (previous tail)", File(engineLogs,"engine-previous.jsonl"))
        w.appendLine("Timing interpretation: wall_ns includes waiting; cpu_ns is active thread CPU time. " +
            "largest_phase identifies a candidate path, not a proven cause. " +
            "GPU/API completion is not physical display presentation. Logging can affect timing.")
        w.appendLine("===== current engine thread stacks (snapshot at export) =====")
        runCatching { Thread.getAllStackTraces().entries.filter { (thread, _) ->
            thread.name.startsWith("PSX-") || thread.name.contains("VM", ignoreCase = true)
        }.take(24).forEach { (thread, stack) ->
            w.appendLine("${thread.name} java_id=${thread.id} state=${thread.state}")
            stack.take(24).forEach { w.appendLine("  at $it") }
        } }.onFailure { w.appendLine("unavailable: ${it.javaClass.simpleName}") }
        w.appendLine()
        w.appendLine("===== input devices =====")
        w.appendLine("Controller settings at export:")
        w.appendLine(runCatching { com.armsx2.input.ControllerMappings.diagnosticStickSettings() }
            .getOrElse { "unavailable: ${it.javaClass.simpleName}" })
        runCatching {
            InputDevice.getDeviceIds().take(64).forEach { id ->
                val device = InputDevice.getDevice(id) ?: return@forEach
                w.appendLine("id=$id name=${device.name} vendor=${device.vendorId} product=${device.productId} " +
                    "sources=0x${device.sources.toString(16)} descriptor=${device.descriptor}")
                device.motionRanges.forEach { range ->
                    w.appendLine("  axis=${range.axis} source=0x${range.source.toString(16)} " +
                        "min=${range.min} max=${range.max} flat=${range.flat} fuzz=${range.fuzz}")
                }
            }
        }.onFailure { w.appendLine("unavailable: ${it.javaClass.simpleName}") }
        w.appendLine()

        val crashes = externalLogs
            .listFiles { f -> f.name.startsWith("crash-") && f.name.endsWith(".txt") }
            ?.sortedByDescending(File::lastModified)?.take(CRASH_FILES).orEmpty()
        if (crashes.isEmpty()) {
            w.appendLine("===== crashes: none recorded =====")
            w.appendLine()
        }
        crashes.forEach { section(w, "crash ${it.name}", it) }

        // Native aborts (SIGSEGV etc.) never reach the in-app handlers — debuggerd owns them.
        // The OS keeps the record instead: every recent death of this process, with the actual
        // TOMBSTONE (symbolized — the APK ships the core's symbol table) for native crashes and
        // the thread dump for ANRs. This is the comprehensive Android-side crash story.
        w.appendLine("===== process exits (ApplicationExitInfo) =====")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching {
                val am = context.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
                val exits = am.getHistoricalProcessExitReasons(context.packageName, 0, 5)
                if (exits.isEmpty()) w.appendLine("none recorded")
                val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)
                exits.forEach { info ->
                    w.appendLine(
                        "--- ${fmt.format(Date(info.timestamp))} reason=${exitReason(info.reason)} " +
                            "status=${info.status}",
                    )
                    info.description?.takeIf(String::isNotBlank)?.let { w.appendLine("    $it") }
                    runCatching {
                        info.traceInputStream?.bufferedReader()?.use { trace ->
                            w.appendLine("    --- trace (tombstone / ANR dump) ---")
                            var copied = 0
                            for (line in trace.lineSequence()) {
                                if (copied > TAIL_LIMIT) break
                                w.appendLine(line)
                                copied += line.length + 1
                            }
                        }
                    }
                }
            }.onFailure { w.appendLine("unavailable: ${it.javaClass.simpleName}") }
        } else {
            w.appendLine("requires Android 11+")
        }
        w.appendLine()

        // Belt and braces alongside the exit records: the renderer's last lines and the abort
        // message are also still in our own logcat buffer right after a bad session.
        w.appendLine("===== recent logcat (this process) =====")
        runCatching {
            val snapshot = File.createTempFile("report-logcat-", ".txt", context.cacheDir)
            var process: java.lang.Process? = null
            try {
                process = ProcessBuilder("logcat", "-d", "-t", "600", "--pid=${Process.myPid()}")
                    .redirectErrorStream(true).redirectOutput(snapshot).start()
                if (!process.waitFor(3, TimeUnit.SECONDS)) process.destroyForcibly()
                section(w, "logcat snapshot", snapshot)
            } finally {
                process?.destroy()
                snapshot.delete()
            }
        }.onFailure { w.appendLine("logcat unavailable: ${it.javaClass.simpleName}") }
    }

    /** Human name for an [android.app.ApplicationExitInfo] reason code. The constants are
     *  compile-time ints, so referencing them here is safe below API 30. */
    private fun exitReason(reason: Int): String = when (reason) {
        android.app.ApplicationExitInfo.REASON_EXIT_SELF -> "exit_self"
        android.app.ApplicationExitInfo.REASON_SIGNALED -> "signaled"
        android.app.ApplicationExitInfo.REASON_LOW_MEMORY -> "low_memory_kill"
        android.app.ApplicationExitInfo.REASON_CRASH -> "java_crash"
        android.app.ApplicationExitInfo.REASON_CRASH_NATIVE -> "native_crash"
        android.app.ApplicationExitInfo.REASON_ANR -> "anr"
        android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "init_failure"
        android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "permission_change"
        android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive_resource_usage"
        android.app.ApplicationExitInfo.REASON_USER_REQUESTED -> "user_requested"
        android.app.ApplicationExitInfo.REASON_USER_STOPPED -> "user_stopped"
        android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "dependency_died"
        android.app.ApplicationExitInfo.REASON_OTHER -> "other"
        else -> "reason_$reason"
    }

    private fun section(w: BufferedWriter, title: String, file: File) {
        if (!file.isFile || file.length() == 0L) {
            w.appendLine("===== $title: absent =====")
            w.appendLine()
            return
        }
        val size = file.length()
        val note = if (size > TAIL_LIMIT) ", last ${TAIL_LIMIT / 1024} KB" else ""
        w.appendLine("===== $title ($size bytes$note) =====")
        runCatching {
            RandomAccessFile(file, "r").use { raf ->
                val length = raf.length()
                val start = maxOf(0L, length - TAIL_LIMIT)
                raf.seek(start)
                val bytes = ByteArray((length - start).toInt())
                var count = 0
                while (count < bytes.size) {
                    val read = raf.read(bytes, count, bytes.size - count)
                    if (read <= 0) break
                    count += read
                }
                w.append(String(bytes, 0, count, Charsets.UTF_8))
            }
        }.onFailure { w.appendLine("unreadable: ${it.javaClass.simpleName}") }
        w.appendLine()
    }
}
