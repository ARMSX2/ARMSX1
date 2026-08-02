// SPDX-License-Identifier: GPL-3.0+
package com.armsx2

import android.app.Application
import android.os.Build
import coil.ImageLoader
import coil.ImageLoaderFactory
import coil.decode.GifDecoder
import coil.decode.ImageDecoderDecoder
import coil.disk.DiskCache
import coil.memory.MemoryCache
import coil.request.CachePolicy
import java.io.File

/**
 * Application subclass that owns the Coil [ImageLoader] used app-wide for game
 * cover art. We override the default Coil disk cache for two reasons:
 *
 *  1. The default location is `cacheDir/image_cache`, which Android may wipe
 *     under memory pressure. Cover art is small, never changes, and worth
 *     keeping across cache-clear events — `filesDir/cover_cache` is the
 *     persistent equivalent.
 *  2. The default size is 2% of free space (max 250 MB). 100 MB is plenty
 *     for thousands of covers (typical PS1/PS2 cover JPEG ~30-80 KB) while
 *     not eating the user's storage.
 *
 * Cover requests in the UI use Coil's `SubcomposeAsyncImage`, which picks up
 * this default `ImageLoader` automatically — no changes needed at call sites.
 */
class Pasx2Application : Application(), ImageLoaderFactory {

	override fun onCreate() {
		super.onCreate()
		installCrashLogging()
		StartupTrace.begin()
		warmUpOffMainThread()
	}

	/**
	 * Pull the two unavoidable cold-start costs onto a worker while the boot splash plays.
	 *
	 * Neither of these is optional work — the app needs both — but both used to be
	 * paid on the UI thread, in the window between process start and the library's first frame.
	 * That window is exactly when the user sees the "ARMSX1 isn't responding" flash. Starting them
	 * here buys the whole length of the intro video (~4 s) to absorb them; on the logo-disabled path
	 * the worker still runs ahead of MainActivity.onCreate by however long the splash hand-off takes.
	 *
	 * Nothing here CHANGES behaviour — each item is idempotent and already had to happen. If the main
	 * thread reaches one first it simply blocks the same amount it always did, so the worst case is
	 * the status quo.
	 */
	private fun warmUpOffMainThread() {
		kotlin.concurrent.thread(isDaemon = true, name = "armsx-warmup") {
			// 1. SharedPreferences. getSharedPreferences() only SCHEDULES the XML parse; the first
			//    getX() BLOCKS on it. BootSplashActivity's very first act is prefs.getBoolean(
			//    "ui.bootLogo") — on the UI thread, against a file that also carries config.global,
			//    every per-game settings blob and the controller mappings, so it is not small.
			runCatching { getSharedPreferences("ARMSX2", MODE_PRIVATE).getBoolean("ui.bootLogo", true) }

			// 2. The native libraries. Touching any NativeApp member runs its <clinit>, which
			//    System.loadLibrary's libSDL2 (5.8 MB) and libarmsx (8 MB, pulling libc++_shared at
			//    9 MB) — tens of MB of dlopen + relocation. MainActivityRuntime.onCreate triggers
			//    that on the UI thread at the `NativeApp.sRumbleEnabled = …` line. Class init is
			//    single-flighted by the JVM, so doing it here can only move the cost, never double it.
			runCatching { kr.co.iefriends.pcsx2.NativeApp.hasNoNativeBinary }

			StartupTrace.mark("warmup-done")
		}
	}

	override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
		super.onConfigurationChanged(newConfig)
		com.armsx2.i18n.I18n.refreshSystemLanguage(this)
	}

	/**
	 * Diagnostics for the no-ADB case: some games (e.g. GoW2, LEGO Batman) drop
	 * back to the library on boot on some setups. We can't read logcat on the
	 * user's device, so:
	 *   1. Tee stdout/stderr — which carry the `@@ANDROID_LAUNCH_GAME@@` markers
	 *      and native Console output — into `<externalFilesDir>/logs/session.log`,
	 *      so even a native crash leaves a breadcrumb of the last thing attempted.
	 *   2. Install an uncaught-exception handler that dumps Kotlin/Java crashes to
	 *      `<externalFilesDir>/logs/crash-<time>.txt` with game + build context.
	 * Both are reachable via the in-app "Open data folder" action. Every step is
	 * guarded so the logger itself can never take the app down.
	 */
	private fun installCrashLogging() {
		runCatching {
			val logDir = File(getExternalFilesDir(null) ?: filesDir, "logs").apply { mkdirs() }

			runCatching {
				val sessionLog = File(logDir, "session.log")
				if (sessionLog.length() > 512L * 1024L) sessionLog.delete()
				val sink = java.io.PrintStream(java.io.FileOutputStream(sessionLog, true), true)
				System.setOut(TeePrintStream(System.out, sink))
				System.setErr(TeePrintStream(System.err, sink))
			}

			val previous = Thread.getDefaultUncaughtExceptionHandler()
			Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
				runCatching {
					File(logDir, "crash-${System.currentTimeMillis()}.txt").writeText(
						buildString {
							appendLine("ARMSX1 crash log")
							appendLine("time=${System.currentTimeMillis()}")
							appendLine("thread=${thread.name}")
							appendLine(
								"game=" + runCatching {
									com.armsx2.runtime.MainActivityRuntime.currentGame.value
										?.let { "${it.title} / ${it.serial}" }
								}.getOrNull(),
							)
							appendLine(
								"version=" + runCatching {
									packageManager.getPackageInfo(packageName, 0).versionName
								}.getOrNull(),
							)
							appendLine("device=${Build.MANUFACTURER} ${Build.MODEL} / Android ${Build.VERSION.RELEASE}")
							appendLine("---")
							appendLine(android.util.Log.getStackTraceString(throwable))
						},
					)
				}
				previous?.uncaughtException(thread, throwable)
			}
		}
	}

	override fun newImageLoader(): ImageLoader {
		val coverCacheDir = File(filesDir, "cover_cache").apply { mkdirs() }

		return ImageLoader.Builder(this)
			.components {
				// Animated library background support: decode animated GIF /
				// WebP / APNG. Android's ImageDecoder (API 28+) covers all
				// three; GifDecoder is the fallback for API 26-27. Static
				// images (JPEG/PNG covers) are unaffected.
				if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P)
					add(ImageDecoderDecoder.Factory())
				else
					add(GifDecoder.Factory())
			}
			.diskCache {
				DiskCache.Builder()
					.directory(coverCacheDir)
					// 100 MiB. Covers are tiny; this fits an enormous library.
					.maxSizeBytes(100L * 1024L * 1024L)
					.build()
			}
			.memoryCache {
				MemoryCache.Builder(this)
					// 25% of the app's memory budget (Coil default), but
					// explicit so it's clear what we're doing.
					.maxSizePercent(0.25)
					.build()
			}
			// Cache policy: keep both layers enabled. Read order is
			// memory → disk → network; write order is the inverse.
			.diskCachePolicy(CachePolicy.ENABLED)
			.memoryCachePolicy(CachePolicy.ENABLED)
			.networkCachePolicy(CachePolicy.ENABLED)
			// Game cover JPEGs from xlenore/* don't change once published,
			// so always serve from the disk cache after the first successful
			// fetch even if the server's Cache-Control says otherwise.
			.respectCacheHeaders(false)
			.crossfade(150)
			.build()
	}
}

/**
 * Cold-start instrumentation, on by default and self-limiting.
 *
 * There is no adb on the device this has to be diagnosed on, so the trace writes through
 * `println` — [Pasx2Application.installCrashLogging] tees System.out into
 * `Android/data/com.nanodata.armsx/files/logs/session.log` on shared storage, readable with any
 * file manager. Grep that file for `@@LAUNCH@@`.
 *
 * Two things are recorded:
 *
 *  1. **Phase marks.** [mark] logs the gap since the previous mark, so a slow step in
 *     `MainActivityRuntime.onCreate` names itself instead of having to be guessed at.
 *  2. **A main-thread stall watchdog.** A worker pings the main looper 4×/s for the first
 *     [WATCHDOG_WINDOW_MS]. When a ping is not answered within [STALL_THRESHOLD_MS] it dumps the
 *     main thread's stack — which is the actual answer to "what is blocking the launch", not an
 *     inference from it — and then reports how long the stall ran in total.
 *
 * Cost is one daemon thread and two wake-ups a second for fifteen seconds, then nothing. It stops
 * long before any game boots, so it is never on a frame path.
 */
object StartupTrace {
	private const val TAG = "ARMSX-Launch"
	private const val WATCHDOG_WINDOW_MS = 15_000L
	private const val PING_INTERVAL_MS = 250L

	/** A main-thread message this slow is already visible as a dropped-frame burst. */
	private const val STALL_THRESHOLD_MS = 400L

	/** Enough to identify every distinct blocker in one launch without flooding session.log. */
	private const val MAX_STALL_REPORTS = 8
	private const val MAX_STACK_FRAMES = 16

	@Volatile private var startedAtMs = 0L
	@Volatile private var lastMarkMs = 0L
	private val begun = java.util.concurrent.atomic.AtomicBoolean(false)

	fun begin() {
		if (!begun.compareAndSet(false, true)) return
		startedAtMs = android.os.SystemClock.uptimeMillis()
		lastMarkMs = startedAtMs
		emit("process start")
		startWatchdog()
	}

	/** Record a launch phase. Cheap enough to sprinkle freely; no-op before [begin]. */
	fun mark(phase: String) {
		val start = startedAtMs
		if (start == 0L) return
		val now = android.os.SystemClock.uptimeMillis()
		val delta = now - lastMarkMs
		lastMarkMs = now
		emit("$phase +${delta}ms t=${now - start}ms")
	}

	private fun emit(msg: String) {
		runCatching { println("@@LAUNCH@@ $msg") }
		android.util.Log.i(TAG, msg)
	}

	private fun startWatchdog() {
		val main = android.os.Handler(android.os.Looper.getMainLooper())
		val mainThread = android.os.Looper.getMainLooper().thread
		kotlin.concurrent.thread(isDaemon = true, name = "armsx-launch-watchdog") {
			val deadline = android.os.SystemClock.uptimeMillis() + WATCHDOG_WINDOW_MS
			var reports = 0
			while (android.os.SystemClock.uptimeMillis() < deadline) {
				val pong = java.util.concurrent.CountDownLatch(1)
				val sentAt = android.os.SystemClock.uptimeMillis()
				if (!main.post { pong.countDown() }) return@thread

				var dumped = false
				while (!pong.await(STALL_THRESHOLD_MS, java.util.concurrent.TimeUnit.MILLISECONDS)) {
					// Dump once per stall, on the first threshold crossing — the stack captured
					// while it is STILL stuck is the one that names the blocker.
					if (!dumped && reports < MAX_STALL_REPORTS) {
						dumped = true
						reports++
						emitStack(mainThread, android.os.SystemClock.uptimeMillis() - sentAt)
					}
					if (android.os.SystemClock.uptimeMillis() > deadline + WATCHDOG_WINDOW_MS) break
				}
				if (dumped) {
					emit("main thread unblocked after ${android.os.SystemClock.uptimeMillis() - sentAt}ms")
				}
				runCatching { Thread.sleep(PING_INTERVAL_MS) }.onFailure { return@thread }
			}
			emit("watchdog done (${reports} stall(s) over ${WATCHDOG_WINDOW_MS}ms)")
		}
	}

	private fun emitStack(mainThread: Thread, blockedMs: Long) {
		val frames = runCatching { mainThread.stackTrace }.getOrDefault(emptyArray())
		val where = frames.take(MAX_STACK_FRAMES).joinToString(" <- ") {
			"${it.className.substringAfterLast('.')}.${it.methodName}:${it.lineNumber}"
		}
		emit("main thread BLOCKED >${blockedMs}ms at $where")
	}
}

/** PrintStream that mirrors everything to a second sink (the on-disk session log). */
private class TeePrintStream(
	private val primary: java.io.PrintStream,
	private val mirror: java.io.PrintStream,
) : java.io.PrintStream(primary) {
	override fun write(b: Int) {
		primary.write(b)
		runCatching { mirror.write(b) }
	}

	override fun write(buf: ByteArray, off: Int, len: Int) {
		primary.write(buf, off, len)
		runCatching { mirror.write(buf, off, len) }
	}

	override fun flush() {
		primary.flush()
		runCatching { mirror.flush() }
	}
}
