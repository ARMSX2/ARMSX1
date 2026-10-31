package com.armsx2

import android.app.Activity
import android.app.ActivityManager
import android.app.Application
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import kr.co.iefriends.pcsx2.NativeApp

object DiagnosticCapture {
    private const val LIMIT = 2L * 1024 * 1024
    private val started = AtomicBoolean()

    fun isMainProcess(context: Context): Boolean {
        val name = runCatching {
            if (Build.VERSION.SDK_INT >= 28) Application.getProcessName() else
                (context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager)
                    ?.runningAppProcesses?.firstOrNull { it.pid == Process.myPid() }?.processName
        }.getOrNull()
        return name == context.packageName
    }

    fun start(application: Application) {
        if (!BuildConfig.DIAGNOSTIC_BUILD || !isMainProcess(application) ||
            !started.compareAndSet(false, true)) return
        val logs = File(application.filesDir, "logs").apply { mkdirs() }
        application.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(activity: Activity, state: Bundle?) = event(activity, "created")
            override fun onActivityStarted(activity: Activity) = event(activity, "started")
            override fun onActivityResumed(activity: Activity) = event(activity, "resumed")
            override fun onActivityPaused(activity: Activity) = event(activity, "paused")
            override fun onActivityStopped(activity: Activity) = event(activity, "stopped")
            override fun onActivityDestroyed(activity: Activity) = event(activity, "destroyed")
            override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) = Unit
            private fun event(activity: Activity, state: String) {
                Log.i("ARMSX-DIAG", "activity=${activity.javaClass.simpleName} state=$state")
            }
        })
        thread(isDaemon = true, name = "diagnostic-logcat") {
            var process: java.lang.Process? = null
            runCatching {
                process = ProcessBuilder("logcat", "-v", "threadtime", "--pid=${Process.myPid()}")
                    .redirectErrorStream(true).start()
                val target = File(logs, "device.log")
                val previous = File(logs, "device.previous.log")
                if (target.length() >= LIMIT && !target.renameTo(previous)) target.delete()
                var writer = FileOutputStream(target, true).bufferedWriter(Charsets.UTF_8)
                var size = target.length()
                var flushedAt = SystemClock.elapsedRealtime()
                try {
                    process!!.inputStream.bufferedReader().useLines { lines ->
                        lines.forEach { line ->
                            if (size >= LIMIT) {
                                writer.close()
                                if (!target.renameTo(previous)) target.delete()
                                writer = FileOutputStream(target, true).bufferedWriter(Charsets.UTF_8)
                                size = 0
                            }
                            val bounded = line.take(8192)
                            writer.appendLine(bounded)
                            size += bounded.toByteArray(Charsets.UTF_8).size + 1
                            val now = SystemClock.elapsedRealtime()
                            if (now - flushedAt >= 1000) {
                                writer.flush()
                                flushedAt = now
                            }
                        }
                    }
                } finally {
                    writer.close()
                }
            }.onFailure { Log.w("ARMSX-DIAG", "persistent log capture failed", it) }
            process?.destroy()
        }
        thread(isDaemon = true, name = "diagnostic-heartbeat") {
            runCatching {
                var samples = 0
                while (true) {
                    Thread.sleep(5000)
                    val native = runCatching {
                        "frames=${NativeApp.getPresentedFrameCount()} fps=${NativeApp.getFPS()} " +
                            "nominal=${NativeApp.getNominalFrameRate()}"
                    }.getOrElse { "native=${it.javaClass.simpleName}" }
                    Log.i("ARMSX-DIAG", "uptime=${SystemClock.elapsedRealtime()} $native " +
                        "heap=${Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()} " +
                        "storage=${application.filesDir.usableSpace}")
                    if (++samples % 6 == 0) DiagnosticsReport.refresh(application, export = false)
                }
            }.onFailure { Log.w("ARMSX-DIAG", "diagnostic heartbeat stopped", it) }
        }
    }
}
