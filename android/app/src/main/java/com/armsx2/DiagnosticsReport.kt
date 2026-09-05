package com.armsx2

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Process
import com.armsx2.config.Ps1SettingsStore
import java.io.BufferedWriter
import java.io.File
import java.io.RandomAccessFile
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * "Generate log file": everything a bug report needs, in one file the USER chose the location of.
 *
 * Every piece already exists, but where users cannot reach it. The core's diagnostics file
 * (`armsx.log` — every emulator/interpreter/renderer line once `[console] logging_enabled` is
 * on) is written under the app-INTERNAL files dir, invisible to file managers; the Kotlin
 * session/crash logs live under the app's external files dir; native render errors only reach
 * logcat. Enabling the toggle asks for a destination through the system save dialog, switches
 * the core's own logging on, and from then on the report at that destination is rewritten on
 * every app start — so after a crash or a bad session, the file the user already knows the
 * location of holds the evidence.
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

    private fun prefs(context: Context) =
        context.getSharedPreferences("ARMSX2", Context.MODE_PRIVATE)

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_ENABLED, false)

    private fun reportUri(context: Context): Uri? =
        prefs(context).getString(KEY_URI, null)?.let { runCatching { Uri.parse(it) }.getOrNull() }

    /**
     * Persist the destination the user picked in the save dialog and switch capture on: the
     * grant is kept across restarts where the provider allows it, and the core's
     * `[console] logging_enabled` goes on globally (paired with `quiet`, which the core derives
     * from it — same contract Ps1AdvancedTab keeps) so the next session writes a full armsx.log.
     */
    fun enable(context: Context, target: Uri) {
        runCatching {
            context.contentResolver.takePersistableUriPermission(
                target,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
        }
        prefs(context).edit()
            .putBoolean(KEY_ENABLED, true)
            .putString(KEY_URI, target.toString())
            .apply()
        runCatching {
            val s = Ps1SettingsStore.load(context)
            // Report capture excludes high-frequency trace and debug events.
            val level = if (s.logLevel < LOG_LEVEL_INFO) LOG_LEVEL_INFO else s.logLevel
            if (!s.loggingEnabled || level != s.logLevel) {
                Ps1SettingsStore.save(
                    context,
                    s.copy(loggingEnabled = true, quiet = false, logLevel = level),
                )
            }
        }
    }

    /** Stop refreshing the report. The core's logging toggle is left as-is — it is the user's
     *  (and Advanced tab's) setting, and turning it off here would surprise both. */
    fun disable(context: Context) {
        prefs(context).edit().putBoolean(KEY_ENABLED, false).apply()
    }

    /**
     * Rewrite the report at the chosen destination. Returns false when the toggle is off, the
     * grant is gone (file deleted, storage detached), or the write failed — never throws.
     * Blocking: call from an IO thread.
     */
    fun refresh(context: Context): Boolean {
        if (!isEnabled(context)) return false

        val uri = reportUri(context) ?: return false

        return runCatching {
            // "wt" truncates: a shorter rewrite must not leave the tail of the previous report.
            val stream = context.contentResolver.openOutputStream(uri, "wt") ?: return false
            stream.bufferedWriter().use { writeReport(context, it) }
            true
        }.getOrDefault(false)
    }

    private fun writeReport(context: Context, w: BufferedWriter) {
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
        val coreLogging = runCatching { Ps1SettingsStore.load(context).loggingEnabled }.getOrDefault(false)
        w.appendLine(
            "core logging: " +
                if (coreLogging) "on" else "off — armsx.log below is stale; re-enable and play again",
        )
        w.appendLine()

        section(w, "settings.toml", File(context.filesDir, "settings.toml"))
        section(w, "armsx.log — core/interpreter/renderer diagnostics", File(context.filesDir, "logs/armsx.log"))
        section(w, "session.log — frontend stdout/stderr", File(externalLogs, "session.log"))

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
            val p = Runtime.getRuntime().exec(
                arrayOf("logcat", "-d", "-t", "2000", "--pid=${Process.myPid()}"),
            )
            p.inputStream.bufferedReader().useLines { lines -> lines.forEach { w.appendLine(it) } }
            p.waitFor()
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
                val start = maxOf(0L, raf.length() - TAIL_LIMIT)
                raf.seek(start)
                val bytes = ByteArray((raf.length() - start).toInt())
                raf.readFully(bytes)
                w.append(String(bytes, Charsets.UTF_8))
            }
        }.onFailure { w.appendLine("unreadable: ${it.javaClass.simpleName}") }
        w.appendLine()
    }
}
