package kr.co.iefriends.pcsx2;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.ParcelFileDescriptor;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.system.Os;
import android.system.OsConstants;
import android.view.InputDevice;
import android.view.Surface;

import com.armsx2.BiosInfo;
import com.armsx2.EmuState;
import com.armsx2.runtime.MainActivityRuntime;

import java.io.File;
import java.lang.ref.WeakReference;

// ⚠ PS1 SHIM: the PCSX2 (PS2) native methods are stubbed to safe defaults so the lifted
// ARMSX2 UI compiles and runs without a PCSX2 core. PS1-applicable methods get wired to the
// ARMSX SDL runtime over time (see com.armsx2.core.Ps1Native / settings.toml). Original =
// ARMSX2's kr.co.iefriends.pcsx2.NativeApp.
public class NativeApp {
	static {
		String libraryName = selectNativeLibraryName();
		try {
			// libarmsx.so links against libSDL2.so; load it first so the in-process JNI
			// surface below is available even when the SDL activity never ran.
			try {
				System.loadLibrary("SDL2");
			} catch (UnsatisfiedLinkError ignored) {
			}
			System.loadLibrary(libraryName);
			hasNoNativeBinary = false;
			System.out.println("PCSX2_LOAD " + libraryName + " pageSize=" + getRuntimePageSize());
		} catch (UnsatisfiedLinkError e) {
			hasNoNativeBinary = true;
			System.err.println("PCSX2_LOAD_FAILED " + libraryName + ": " + e.getMessage());
		}
	}

	public static boolean hasNoNativeBinary;

	private static long getRuntimePageSize() {
		try {
			long pageSize = Os.sysconf(OsConstants._SC_PAGESIZE);
			return pageSize > 0 ? pageSize : 4096;
		} catch (Throwable ignored) {
			return 4096;
		}
	}

	private static String selectNativeLibraryName() {
		// PS1 port: the core is libarmsx.so (built by build.sh android, staged into
		// jniLibs/<abi>). The PCSX2 emucore_4k/emucore_16k page-size split does not apply.
		return "armsx";
	}


	protected static WeakReference<Context> mContext;
	public static Context getContext() {
		return mContext != null ? mContext.get() : null;
	}

	public static void initializeOnce(Context context) {
		mContext = new WeakReference<>(context);

		// Compute the app's externalFilesDir up front — it's the BIOS
		// folder (always app-owned + writable, where the setup wizard's
		// finishBiosStep deposits the BIOS file) and the fallback for
		// DataRoot when the user hasn't picked one.
		File externalFilesDir = context.getExternalFilesDir(null);
		if (externalFilesDir == null) {
			externalFilesDir = context.getDataDir();
		}

		// DataRoot: prefer the user-chosen system folder only when the SAF tree
		// URI resolves to a POSIX path that native code can actually write.
		// Falls back to externalFilesDir when unset, unresolvable, or blocked
		// by scoped storage.
		String chosen = MainActivityRuntime.Companion.systemDirPosix();
		if (chosen != null && !MainActivityRuntime.Companion.validateSystemDirWritable(chosen)) {
			chosen = null;
		}
		String dataPath = (chosen != null) ? chosen : externalFilesDir.getAbsolutePath();

		// BIOS folder: the directory that actually holds the configured BIOS file.
		// The setup wizard (and the migration in MainActivityRuntime.kickoffEmucoreInit) keep the
		// BIOS in app-private internal storage — NOT under a custom/SD data root —
		// because the native FileSystem APIs can't reliably open a BIOS off a
		// removable/SAF volume on Android 11+ (that made a data-root-on-SD game fail
		// VM init and bounce back to the library). Falls back to externalFilesDir/bios
		// (always app-owned + readable), matching that decoupled-BIOS design.
		String biosFolder = MainActivityRuntime.Companion.biosFolderPosix();
		if (biosFolder == null || biosFolder.isEmpty()) {
			biosFolder = externalFilesDir.getAbsolutePath() + java.io.File.separator + "bios";
		}

		initialize(dataPath, biosFolder, android.os.Build.VERSION.SDK_INT);

		// Replay a host override that arrived via broadcast while the native
		// library was not yet loaded.
		com.armsx2.RetroAchievementsHostOverrideReceiver.applyPending(context);
	}

	public static void initialize(String path, String biosFolder, int apiVer) {  }

	// PGO instrument build only: flush collected profile counters to disk.
	// No-op in normal builds (the native impl is empty without -fprofile-generate).
	public static void dumpPgoProfile() {  }

	/** Turn a RetroArch (.slangp) shader chain on or off, and choose the preset.
	 *
	 *  An empty or null [presetPath] means "no chain" whatever [enabled] says. Cheap and
	 *  non-blocking: the request is recorded and the presenting thread builds the chain on
	 *  its next frame, because compiling shaders must not happen on the caller's thread.
	 *  Safe with no VM and no renderer.
	 *
	 *  Needs the Vulkan present backend — librashader has no GLES runtime. On any other
	 *  backend this is a no-op that says so once in the log rather than silently doing
	 *  nothing. A preset that fails to compile degrades to plain presentation, logged. */
	public static native void setShaderChain(boolean enabled, String presetPath);

	/** The tweakable parameters a .slangp preset declares, as a JSON ARRAY, in declaration
	 *  order. Pure file parsing — no renderer or running game needed, so it is safe to call
	 *  from a settings screen with nothing booted. Blocking IO: call it off the UI thread.
	 *
	 *  Order is load-bearing: a parameter whose "maximum" equals its "minimum" is a CAPTION
	 *  introducing the run that follows it, not a control.
	 *
	 *  NEVER null and never "" — every failure path (no librashader, unreadable preset)
	 *  returns "[]", which JSONArray parses cleanly to an empty list. */
	public static native String shaderPresetParams(String presetPath);

	/** Queues parameter values for the running shader chain; the GS thread applies them on
	 *  its next frame. Safe to call from any thread, and safe with no VM or renderer up —
	 *  the values just sit there until a chain reads them.
	 *
	 *  [names]/[values] are parallel arrays of assignments, NOT the chain's full state:
	 *  a parameter left out keeps what the chain has, so resetting one means sending its
	 *  initial value rather than omitting it. [presetPath] must be the preset the values
	 *  were read off, so a stale set can't land on a chain that has moved on. */
	public static native void setShaderChainParams(String presetPath, String[] names, float[] values);

	// Save a GS dump (.gs of GPU commands) to the snaps folder for diagnosing
	// rendering bugs. frames <= 0 captures a single frame.
	public static void captureGsDump(int frames) {  }
	/** Screenshot of the emulated framebuffer. No-op with no VM.
	 *  Implemented natively (frontend/android_jni.cpp). Queued on the emulation thread;
	 *  the payload is written by SDL_SaveBMP, so the bytes are BMP whatever the extension. */
	public static native void saveScreenshot(String pngPath);

	/** ADPF (PerformanceHintManager): tell the OS how long the emulation thread spent WORKING
	 *  each frame against the frame deadline, so the DVFS governor clocks for that instead of
	 *  inferring demand from a bursty load average. Applies live and is safe with no VM —
	 *  the value is parked and the emulation loop picks it up on its next frame.
	 *
	 *  EXPERIMENTAL and off by default: it is a lever to A/B on a device, not a known win, and
	 *  it can cost power for nothing on a device whose governor is already aggressive.
	 *  Silent no-op below Android 13 (the API is dlsym'd, never linked).
	 *  Implemented natively in frontend/perf_hint.c; persisted app-side (pref "ui.adpf"). */
	public static native void setAdpfEnabled(boolean enabled);

	/**
	 * Push one EmuCore setting into the base settings layer. Mirrors
	 * pcsx2-qt's Settings save flow — Host::SetBase*SettingValue sticks
	 * in s_settings_interface. type ∈ {"bool","int","float","string"};
	 * value is the stringified payload (e.g. "true", "2", "2.5").
	 *
	 * Setting writes here are NOT live until commitSettings() is called.
	 * Batch the writes, then commit once so the VM applies them atomically.
	 */
	public static void setSetting(String section, String key, String type, String value) {  }

	/**
	 * Apply queued settings to a running VM (and the GS thread). Calls
	 * VMManager::ApplySettings + MTGS::ApplySettings. No-op when no VM
	 * is running — settings still take effect on the next runVMThread
	 * because they were pushed to the persistent base settings layer.
	 *
	 * Some settings need a VM restart (recompiler enables, EE cycle rate);
	 * the UI layer should flag those.
	 */
	public static void commitSettings() {  }

	/** Diagnostic: write a line to the native emulog (Console) so it shows in the in-app
	 *  Save Log export. Used by the Joy-Con input diagnostic; no-ops if the console isn't open. */
	public static void emulog(String msg) {  }

	/**
	 * Live GS-only reconfigure for a running VM. Reloads the whole EmuCore/GS
	 * section from the base settings layer and pushes it to the GS thread via
	 * MTGS::ApplySettings WITHOUT the heavier VMManager::ApplySettings (no
	 * CPU/JIT rebuild). Call after pushing EmuCore/GS keys via setSetting() so
	 * renderer / hardware-fix / upscaling-fix changes apply mid-game. No-op
	 * when the GS is closed — the keys still take effect on next launch.
	 */
	public static boolean applyGSSettingsLive() { return false; }

	// The three PS2 patch stubs that used to sit here — reloadPatches(),
	// setEnabledPatches() and purgeGlobalPatchEnableLists() — are GONE along with the PNACH
	// Patch Manager that was their only caller. All three were empty, and the screen driving
	// them let a user tick a patch and watch nothing happen. PS1 cheats are the four
	// cheats* natives near the bottom of this file, and every one of them is real.

	// ---- USB lightgun (GunCon 2) ----------------------------------------------
	/** GunCon2 binding ids, from pcsx2/USB/usb-lightgun/guncon2.cpp. */
	public static final int GUNCON_C = 1;
	public static final int GUNCON_B = 2;
	public static final int GUNCON_A = 3;
	public static final int GUNCON_DPAD_UP = 4;
	public static final int GUNCON_DPAD_RIGHT = 5;
	public static final int GUNCON_DPAD_DOWN = 6;
	public static final int GUNCON_DPAD_LEFT = 7;
	public static final int GUNCON_TRIGGER = 13;
	public static final int GUNCON_SELECT = 14;
	public static final int GUNCON_START = 15;
	/** Fires a deliberately off-screen shot — how these games are reloaded. */
	public static final int GUNCON_SHOOT_OFFSCREEN = 16;
	public static final int GUNCON_RECALIBRATE = 17;

	/**
	 * Set the emulated device in a USB port. {@code type} is a core type name
	 * ("guncon2", "None", ...); port is 0 or 1. Restart recommended — swapping a USB
	 * device on a running VM is the emulated equivalent of unplugging it.
	 */
	public static void usbSetDeviceType(int port, String type) {  }

	/**
	 * Every USB device the core can emulate. Records are separated by U+001E, and each record is
	 * {@code typeName} U+001F {@code displayName} then one U+001F-separated entry per subtype.
	 * Enumerated from the core's own registry, so the list cannot drift from what it supports.
	 */
	public static String usbDeviceTypes() { return ""; }

	/** Pick a subtype for whatever device is in {@code port}; devices without subtypes ignore it. */
	public static void usbSetDeviceSubtype(int port, int subtype) {  }

	/** Aim, in WINDOW PIXELS (our SurfaceView is the whole window, so raw touch x/y). */
	public static void usbLightgunAim(float x, float y) {  }

	/** Press/release one GUNCON_* binding on a port. */
	public static void usbLightgunButton(int port, int bind, boolean pressed) {  }
	public static String getGameTitle(String path) {
		// PS1: derive the title from the filename and the serial by scanning the disc image.
		if (path == null || path.isEmpty()) return null;
		try {
			java.io.File f = new java.io.File(path);
			String name = f.getName();
			int dot = name.lastIndexOf('.');
			String title = dot > 0 ? name.substring(0, dot) : name;
			String serial = com.armsx2.core.Ps1DiscId.INSTANCE.serialOf(f);
			if (serial == null) serial = "";
			return title + "|" + serial + "|" + serial;
		} catch (Throwable t) { return null; }
	}
	public static String getGameSerial() { return ""; }
	public static String getGameCRC() { return ""; }
	/** Emulated frames per second, averaged over the core's last half-second window. 0 while no
	 *  game is stepping (stopped or paused). Non-blocking — it reads an atomic the emulation loop
	 *  publishes once per frame, so the Compose OSD can poll it from the UI thread. */
	public static native float getFPS();
	/** Current game's nominal emulated refresh (~59.94 NTSC / 50 PAL), or 0 without a VM. */
	public static native float getNominalFrameRate();

	/** Arm the detailed performance overlay (per-subsystem work counters + host frame-phase
	 *  timing). This is what switches the counters ON INSIDE THE EMULATOR — psx/perf.c — so it
	 *  MUST be cleared when the Statistics block is hidden, or every counter site in the core
	 *  stays live feeding a panel nobody is reading. Off at boot. */
	public static native void setStatisticsEnabled(boolean enabled);

	/** Refill [out] with the latest snapshot and return how many entries were written; 0 until
	 *  the core's first half-second window closes, or while statistics are disabled. Indices are
	 *  com.armsx2.ui.GameOsd.Stat / PSXE_HOST_STAT_* in frontend/host_stats.h. Pass a reused
	 *  array — this deliberately does not allocate. */
	public static native int getStatistics(double[] out);

	/** Build version string from BuildVersion::GitRev — formatted as
	 *  "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT". Used by the
	 *  setup wizard + in-game overlay branding so the displayed version
	 *  tracks the C++ constants without a Kotlin-side hardcoded copy. */
	public static String getBuildVersion() { return "ARMSX1"; }

	public static String getPauseGameTitle() { return ""; }
	public static String getPauseGameSerial() { return ""; }

	// ---- RetroAchievements ----
	//
	// Backed by frontend/achievements.cpp (rcheevos) through frontend/android_jni.cpp.
	// SOFTCORE ONLY: hardcore is refused natively and both hardcore queries always answer
	// false, so no unlock earned here can ever claim hardcore credit on an RA account.
	//
	// None of the String methods below may return null — the achievements screen calls
	// .orEmpty()/optString on the result, and a null used to take the whole app down as soon
	// as the screen opened. The native side returns "" / "{}" instead.

	/** Snapshot the current game's achievements as JSON for the achievements screen and the
	 *  in-game overlay's right-side panel. See armsx_ach_get_json() for the schema — it is the
	 *  same one ARMSX2 emits. Returns the empty-state payload (active=false, items=[]) when no
	 *  game is loaded or nobody is signed in, never null. */
	public static native String getAchievementsJSON();

	/** RetroAchievements hash for a disc image, computed without booting it — the key used to look
	 *  a game up in RA's game list so the library can show progress for games never played. This
	 *  is the PS1 disc hash (RC_CONSOLE_PLAYSTATION), NOT the local save-state fingerprint.
	 *  Empty string if the image is unreadable or has no PS1 executable. Reads the disc: call
	 *  off the UI thread. */
	public static native String getAchievementsHashForPath(String imagePath);

	/** Live RetroAchievements rich-presence string, recomputed on the native side from the
	 *  game's RAM. Empty when no game, no client, or RP not supported by the loaded set. */
	public static native String getRichPresence();

	/** RetroAchievements password login. Returns an empty string on success or a
	 *  human-readable error. Synchronous — runs the HTTP login request to completion, may take
	 *  a few seconds. Callers MUST dispatch off the Main thread (Dispatchers.IO from Compose).
	 *  After success the next getAchievementsJSON poll reflects loggedIn=true, and a running
	 *  game is identified against RA on its next frame; no separate callback wiring needed. */
	public static native String loginAchievements(String username, String password);

	/** RetroAchievements logout. Idempotent. Drops the saved token. */
	public static native void logoutAchievements();

	/** Accepted and ignored. ARMSX RetroAchievements support is softcore only until the
	 *  emulator has had a lot more testing, so there is no hardcore path to enable; passing
	 *  true logs a warning natively and changes nothing. */
	public static native void setHardcoreMode(boolean enabled);

	/** Always false — see {@link #setHardcoreMode}. */
	public static native boolean isHardcoreMode();

	/** Always false — see {@link #setHardcoreMode}. */
	public static native boolean isHardcorePersisted();

	/** Toggle a RetroAchievements presentation option. {@code key} is one of
	 *  "notifications", "leaderboardNotifications", "overlays", "lbOverlays", "soundEffects",
	 *  "encoreMode", "spectatorMode", "unofficialTestMode". Persisted natively and reported
	 *  back in {@link #getAchievementsJSON}. "hardcore" is deliberately not a valid key. */
	public static native void setAchievementsOption(String key, boolean enabled);

	/** Integer-valued options: "notificationsDuration", "leaderboardsDuration",
	 *  "notificationPosition", "overlayPosition". Clamped natively. */
	public static native void setAchievementsOptionInt(String key, int value);

	// Custom achievement-unlock sound. `path` is an app-private absolute file the MediaPlayer
	// can read; an empty string clears it. ARMSX bundles no RA sounds of its own, so unlocks
	// are silent until the user imports one.
	public static native void setAchievementsUnlockSound(String path);

	/** Repoint the RetroAchievements client at a loopback proxy. Persisted and applied live, so
	 *  a running session picks up the new host. */
	public static native void setAchievementsHostOverride(String host);

	/** Drop the host override set by {@link #setAchievementsHostOverride}, restoring
	 *  retroachievements.org. */
	public static native void clearAchievementsHostOverride();

	/** True iff the GS is currently in a HW renderer (OGL/VK), false for
	 *  SW. Mirrors GSIsHardwareRenderer() from the GS thread. Polled by
	 *  the in-game overlay's renderer pill so emucore-driven swaps
	 *  (e.g. SoftwareRendererFMVHack) stay in sync with the UI. */
	public static boolean isHardwareRenderer() { return false; }

	/** Master OSD toggle — flips every OsdShow* bit we enable at first
	 *  init. Backs the in-game overlay's OSD pill. */
	public static void osdShowAll(boolean enabled) {  }

	// Live-only OSD flag apply (no persist) — lets the OSD on/off hotkey hide/restore stats
	// without clobbering the user's saved per-stat selection.
	public static void osdApplyFlags(boolean fps, boolean vps, boolean speed, boolean cpu,
		boolean gpu, boolean res, boolean gsStats, boolean frameTimes, boolean hwInfo,
		boolean version, boolean settings, boolean inputs) {  }

	/** Per-element OSD toggles (Performance Overlay tab). Apply live via
	 *  EmuConfig.GS + MTGS::ApplySettings; persistence to base is done on
	 *  the Kotlin side via setSetting. Disabling GPU also stops the GPU
	 *  timing queries (real perf win), see GS.cpp. */
	public static void osdShowFPS(boolean enabled) {  }
	public static void osdShowVPS(boolean enabled) {  }
	public static void osdShowSpeed(boolean enabled) {  }
	public static void osdShowCPU(boolean enabled) {  }
	public static void osdShowGPU(boolean enabled) {  }
	public static void osdShowResolution(boolean enabled) {  }
	public static void osdShowGSStats(boolean enabled) {  }
	public static void osdShowFrameTimes(boolean enabled) {  }
	public static void osdShowHardwareInfo(boolean enabled) {  }
	public static void osdShowMessages(boolean enabled) {  }
	public static void osdShowGpuStats(boolean enabled) {  }
	public static void osdShowVersion(boolean enabled) {  }
	public static void osdShowSettings(boolean enabled) {  }
	public static void osdShowInputs(boolean enabled) {  }
	/** OSD size (percentage; 25–500, 100 = normal). Applies live via MTGS. */
	public static void osdSetScale(float scale) {  }

	/** OSD text colour as 0xRRGGBB; 0 = default white. */
	public static void osdSetColor(int rgb) {  }

	/** Per-game settings export — writes only the keys that differ from global
	 *  into gamesettings/<serial>_<CRC>.ini for the running game (sparse, like
	 *  PCSX2's desktop UI). Stream: gameIniBeginWrite() once, gameIniPut() per
	 *  override key, gameIniCommitWrite() to save (or delete when empty). */
	public static boolean gameIniBeginWrite() { return false; }
	/** VM-less variant of {@link #gameIniBeginWrite()}: targets a game's INI by serial (globbing
	 *  gamesettings/&lt;serial&gt;_*.ini) when nothing is running, so a per-game Reset from the
	 *  library can still clear a stale, in-game-written override file. Returns false when no such
	 *  file exists — there is then nothing to rewrite and the caller should skip the put/commit. */
	public static boolean gameIniBeginWriteForSerial(String serial) { return false; }
	public static void gameIniPut(String section, String key, String value) {  }
	public static boolean gameIniCommitWrite() { return false; }

	/** Pin a custom Vulkan driver (e.g. Mesa Turnip) for the next VM
	 *  start. Must be called BEFORE MainActivityRuntime.start() — the first MTGS::Open
	 *  triggers Vulkan::LoadVulkanLibrary which reads these paths. Pass
	 *  empty strings to revert to the system loader.
	 *
	 *  driverDir:    /data/.../files/drivers/&lt;id&gt;/ (trailing slash required)
	 *  driverName:   e.g. "libvulkan_freedreno.so"
	 *  redirectDir:  /data/.../files/drivers/&lt;id&gt;/cache/ — Turnip shader cache target
	 *  hookLibDir:   ApplicationInfo.nativeLibraryDir — where the adrenotools
	 *                hook .so's (main_hook etc.) were extracted. */
	public static native void setCustomVulkanDriver(
		String driverDir, String driverName,
		String redirectDir, String hookLibDir);

	/** Which GLES implementation the OpenGL presentation backend binds to:
	 *  {@code "system"} or {@code "angle"} (ANGLE = Google's GLES-on-Vulkan
	 *  translator, bundled as libEGL_angle.so + libGLESv2_angle.so).
	 *
	 *  Wins over settings.toml's {@code [video] gl_driver}, which is only the
	 *  persisted default — call this before starting the VM. Takes effect on
	 *  the next renderer init.
	 *
	 *  ANGLE is an implementation of the OpenGL backend, NOT a peer of Vulkan;
	 *  selecting it does not change {@code gpu_backend}. */
	public static native void setGlDriver(String driver);

	/** The GLES implementation currently selected ("system" / "angle"). This is
	 *  the REQUEST; use {@link #getActiveRenderer()} to see what actually loaded. */
	public static native String getGlDriver();

	/** Human-readable name of the presentation backend that is genuinely running —
	 *  the one that survived the fallback ladder, not the one that was asked for.
	 *  Returns "" (never null) when no renderer is up.
	 *
	 *  Possible shapes:
	 *  <ul>
	 *    <li>{@code "Vulkan (Adreno (TM) 740)"}
	 *    <li>{@code "Vulkan (Adreno (TM) 740, custom driver)"} — and the
	 *        ", custom driver" suffix appears ONLY when a user-supplied driver
	 *        really loaded, so a driver that silently failed is distinguishable
	 *    <li>{@code "OpenGL ES (ANGLE)"}
	 *    <li>{@code "OpenGL ES (system)"}
	 *    <li>{@code "SDL accelerated (opengles2)"}
	 *    <li>{@code "Software"}
	 *  </ul> */
	public static native String getActiveRenderer();

	/** Live display aspect: 0 = classic 4:3, 1 = square 1:1, 2 = wide 16:9.
	 *  Anything negative reverts to settings.toml's {@code [video] display_aspect}.
	 *
	 *  Applies on the NEXT PRESENTED FRAME — no game restart. Aspect is a pure
	 *  presentation property, so the in-game menu can change it live; persist it
	 *  through Ps1SettingsStore as well so it survives a relaunch. */
	public static native void setDisplayAspect(int mode);

	/** Live custom width/height ratio, applied while the aspect mode is CUSTOM. <= 0 clears
	 *  the override and falls back to settings.toml. Next presented frame; no VM restart. */
	public static native void setDisplayAspectCustom(float ratio);

	/** Snap the presented image to a whole multiple of the emulated resolution, so every source
	 *  pixel becomes an exact NxN block. Applies on the next presented frame. */
	public static native void setIntegerScaling(boolean enabled);

	/** Live stretch-to-window: true fills the surface and ignores the aspect,
	 *  false letterboxes/pillarboxes to it. Same immediacy as
	 *  {@link #setDisplayAspect(int)}. */
	public static native void setStretchMode(boolean enabled);

	/** PGXP: sub-pixel polygon precision captured from the GTE (psx/pgxp.c). Real
	 *  native, safe to flip while a game runs; new geometry picks it up within a
	 *  frame. Boot-time value comes from settings.toml ({@code [video] pgxp});
	 *  persist UI changes there so they survive a relaunch. Only the hardware
	 *  rasterizer consumes the precision — the software path stays integer. */
	public static native void setPgxpEnabled(boolean enabled);

	/** Widescreen hack: scales the GTE's X projection so 3D geometry fills a 16:9
	 *  display instead of being stretched into it (psx/cpu.c). Real native, safe to
	 *  flip while a game runs. 2D and UI are not projected through the GTE, so they
	 *  still stretch. Boot-time value comes from settings.toml
	 *  ({@code [video] widescreen_hack}); persist UI changes there too. */
	public static native void setWidescreenHack(boolean enabled);

	/** Deinterlacing for 480-line modes: 0 = weave (default, what always shipped),
	 *  1 = bob, 2 = adaptive. Negative clears the override back to settings.toml
	 *  ({@code [video] deinterlace}). Presentation only — next presented frame. */
	public static native void setDeinterlaceMode(int mode);

	/** Overscan crop: 0 = none (default), 1 = small, 2 = full. Trims the blanking
	 *  border and zooms the kept region to the same rect. Negative clears back to
	 *  settings.toml ({@code [video] overscan_crop}). Next presented frame. */
	public static native void setOverscanCrop(int mode);

	/** Display rotation in DEGREES clockwise: 0 / 90 / 180 / 270. Anything else clears
	 *  the override back to settings.toml ({@code [video] display_rotation}). Applied by
	 *  the present backend; the Vulkan backend cannot rotate and stays upright. */
	public static native void setDisplayRotation(int degrees);

	/** The three GLES-rasterizer video options, pushed together because the backend
	 *  stores them together (frontend/gpu_hw_gl.c).
	 *  textureFilter 0 = nearest (default), 1 = bilinear, 2 = xBR-style — textured
	 *  polygons only. downsample 0 = off, 2..8 = box factor; the effective factor is the
	 *  largest divisor of the internal scale that is <= it, so it is off at 1x.
	 *  lineDetect 0 = disabled, 1 = quads, 2 = basic.
	 *  Boot-time values come from settings.toml ({@code [video] texture_filter},
	 *  {@code downsample}, {@code line_detect}). */
	public static native void setGlVideoOptions(int textureFilter, int downsample, int lineDetect);
	/** PS1 texture dumping and replacement (psx/texrep.h). REAL native
	 *  (Java_kr_co_iefriends_pcsx2_NativeApp_setPs1TextureOptions ->
	 *  psxe_host_set_texture_options), parked and applied on the emulation thread.
	 *  [dir] is the BASE folder: dumps go to {@code <dir>/dump}, replacement packs are read
	 *  from {@code <dir>/replacements}, and both are created on demand. Pass a per-game path
	 *  ({@code <root>/textures/<serial>}) — the core cannot derive one because it does not
	 *  know the disc serial. An empty string makes it fall back to {@code <prefs>/textures}.
	 *  Both flags false tears the subsystem down; nothing is allocated in that state.
	 *  Boot-time values come from settings.toml ({@code [video] texture_dump},
	 *  {@code texture_replacements}, {@code texture_dir}). */
	public static native void setPs1TextureOptions(boolean dump, boolean replace, String dir);

	public static void setPadVibration(boolean isonoff) {  }
	/** Implemented natively. [index] is this front-end's pad code space (Android KEYCODE_*
	 *  for the pad, 200 = analog-mode button, 110-113 / 120-123 = left/right stick
	 *  directions); [range] is the 0..32767 magnitude for a stick direction and is ignored
	 *  (full press) for digital buttons. Thread-safe: parked and applied on the next frame. */
	public static native void setPadButton(int index, int range, boolean iskeypressed);
	/** Implemented natively. Absolute stick position: [stick] 0 = left, 1 = right; [x]/[y]
	 *  are 0x00..0xFF centred on 0x80, matching the PS1 pad's ADC. */
	public static native void setPadAnalog(int stick, int x, int y);
	/** {@link #setPadButton} addressed to one of the four players behind a port-1 Multitap
	 *  ([player] 0..3 = slots A..D). Player 0 is the port itself. Real native
	 *  (psxe_host_pad_button_player -> psx_pad_button_press_player); with no tap attached
	 *  the core drops players 1..3. */
	public static native void setPadButtonForPlayer(int player, int index, int range, boolean iskeypressed);
	/** {@link #setPadAnalog} addressed to one of the four Multitap players. */
	public static native void setPadAnalogForPlayer(int player, int stick, int x, int y);

	/** Multitap in controller port 1: four players share the port
	 *  (psx/input/multitap.c). Real native, and LIVE — the core swaps the device in the
	 *  port at the next instruction boundary, so a running game sees it plugged in
	 *  without a restart. Boot value comes from settings.toml ({@code [input] multitap});
	 *  persist UI changes there too or the next launch reverts.
	 *
	 *  Worth knowing before switching it on: a tap answers the pad poll with its OWN id
	 *  (80h), so a game that does not understand multitaps reports NO controller at all.
	 *  Real hardware behaves identically — that is why an SCPH-1070 has a physical
	 *  1-player/multitap switch. */
	public static native void setMultitapEnabled(boolean enabled);

	/** Rewind: keep a ring of save states and step backwards through it
	 *  (psx/rewind.h). [seconds] 1..60, [frequency] 1..8 snapshots per second.
	 *
	 *  seconds x frequency is a MEMORY figure, not just a duration — one snapshot is
	 *  ~3.5 MB, so 10 s at 2/s is ~70 MB. Nothing is allocated while it is off. Use
	 *  {@link #rewindSnapshotBytes()} to show the real cost next to the control. */
	public static native void setRewind(boolean enabled, int seconds, int frequency);

	/** Runahead: re-simulate [frames] frames every frame so the picture reacts sooner,
	 *  0..5, 0 = off (the default). Costs a state save + a state load + [frames] extra
	 *  emulated frames EVERY frame — on a device already at 100% speed this does not hide
	 *  latency, it cuts the frame rate. */
	public static native void setRunahead(int frames);

	/** Hold-to-rewind: true while the bound button is down. Each frame the emulator then
	 *  steps one snapshot BACK instead of advancing; at the end of the buffer it holds
	 *  still rather than resuming forward play. Needs {@link #setRewind} on first. */
	public static native void setRewindActive(boolean active);

	/** One-shot step back, for a menu row or a tap. Additive with the hold above. */
	public static native void rewindStepBack();

	/** Bytes ONE rewind snapshot costs. Measured from the running machine once a state has
	 *  been captured, an estimate before that — so a settings row can show a real figure
	 *  with no game loaded. Multiply by seconds x frequency for the buffer's size. */
	public static native long rewindSnapshotBytes();

	/** Bytes the rewind ring is holding right now. 0 while rewind is off. */
	public static native long rewindBytesHeld();
	/**
	 * Physical-controller entry point (every KeyEvent / MotionEvent write in
	 * MainActivityRuntime funnels here). Was a no-op stub, which is exactly why
	 * physical gamepads did nothing: the touch overlay calls
	 * {@link #setPadButton} directly and worked, the whole pad layer went through
	 * this method and hit the floor.
	 *
	 * PS1: the core drives ONE pad — every psx_pad_* call in frontend/main.cpp is
	 * hard-coded to pad index 0 — so there is no destination for port 1 and it is
	 * dropped. PadRouter caps claims at one slot for the same reason, so in practice
	 * everything arrives here as port 0.
	 *
	 * Stick directions (110-113 left, 120-123 right) are per-direction magnitudes in
	 * 0..32767. They are accumulated here and collapsed into ONE absolute
	 * {@link #setPadAnalog} write per stick (0x00..0xFF centred on 0x80, the PS1
	 * pad's ADC range) rather than being forwarded as four separate directional
	 * codes. Everything else is a digital bit and goes straight through.
	 */
	public static void setPadButtonForPort(int port, int index, int range, boolean iskeypressed) {
		if (port < 0 || port >= MAX_PLAYERS) return;
		int stick, slot;
		switch (index) {
			case 110: stick = 0; slot = STICK_UP;    break;
			case 111: stick = 0; slot = STICK_RIGHT; break;
			case 112: stick = 0; slot = STICK_DOWN;  break;
			case 113: stick = 0; slot = STICK_LEFT;  break;
			case 120: stick = 1; slot = STICK_UP;    break;
			case 121: stick = 1; slot = STICK_RIGHT; break;
			case 122: stick = 1; slot = STICK_DOWN;  break;
			case 123: stick = 1; slot = STICK_LEFT;  break;
			default:
				if (iskeypressed && android.util.Log.isLoggable("ARMSX-PAD", android.util.Log.DEBUG)) {
					android.util.Log.d("ARMSX-PAD", "setPadButton port=" + port
							+ " code=" + index + " range=" + range);
				}
				// Player 0 keeps the original entry point so the touch overlay's direct
				// setPadButton() calls and this path stay on one native.
				if (port == 0) setPadButton(index, range, iskeypressed);
				else setPadButtonForPlayer(port, index, range, iskeypressed);
				return;
		}
		int x, y;
		synchronized (sStickLock) {
			sStickDir[port][stick][slot] = iskeypressed
					? Math.max(0, Math.min(STICK_FULL_RANGE, range))
					: 0;
			x = stickAxisByte(sStickDir[port][stick][STICK_RIGHT], sStickDir[port][stick][STICK_LEFT]);
			y = stickAxisByte(sStickDir[port][stick][STICK_DOWN], sStickDir[port][stick][STICK_UP]);
		}
		if (android.util.Log.isLoggable("ARMSX-PAD", android.util.Log.DEBUG)) {
			android.util.Log.d("ARMSX-PAD", "setPadAnalog port=" + port + " stick=" + stick
					+ " x=0x" + Integer.toHexString(x) + " y=0x" + Integer.toHexString(y));
		}
		if (port == 0) setPadAnalog(stick, x, y);
		else setPadAnalogForPlayer(port, stick, x, y);
	}

	/** Players the core can address: four, because a Multitap in port 1 carries four pads
	 *  (PSXI_MULTITAP_SLOTS). With no tap attached the core DROPS players 1..3 rather than
	 *  folding them onto player 1, so a mis-routed pad reads as a dead player rather than
	 *  two people sharing one controller. */
	public static final int MAX_PLAYERS = 4;

	// Per-direction stick magnitudes, collapsed into the two absolute axes the PS1
	// pad actually has. [player][stick][direction]; stick 0 = left, 1 = right.
	private static final int STICK_UP = 0, STICK_RIGHT = 1, STICK_DOWN = 2, STICK_LEFT = 3;
	private static final int STICK_FULL_RANGE = 32767;
	private static final Object sStickLock = new Object();
	private static final int[][][] sStickDir = new int[MAX_PLAYERS][2][4];

	/** Two opposed 0..32767 magnitudes -> one 0x00..0xFF axis centred on 0x80. */
	private static int stickAxisByte(int positive, int negative) {
		int delta = positive - negative;
		int v = 0x80 + (delta * 127) / STICK_FULL_RANGE;
		return v < 0x00 ? 0x00 : (v > 0xFF ? 0xFF : v);
	}

	// Every digital pad code the core understands (see HostPadMaskForCode in
	// frontend/main.cpp). Used by resetPadState to release the lot explicitly.
	private static final int[] PAD_DIGITAL_CODES = {
		19, 20, 21, 22,            // d-pad up/down/left/right
		96, 97, 99, 100,           // cross / circle / square / triangle
		102, 103, 104, 105,        // L1 / R1 / L2 / R2
		106, 107,                  // L3 / R3
		108, 109,                  // start / select
		200,                       // analog-mode toggle
	};

	/** Drop every held pad bit AND the accumulated stick deflection, then re-centre
	 *  both sticks. Call on focus loss / pause / game exit so nothing sticks down —
	 *  {@link #resetKeyStatus()} alone clears the native side but would leave this
	 *  class's stick accumulator holding the last deflection.
	 *
	 *  ★ The explicit release sweep below is NOT belt-and-braces. resetKeyStatus()
	 *  drops the commands still PARKED in the core's queue; it does not touch the
	 *  bits already handed to the emulated pad, nor the core's held-button mirror
	 *  (ArmsxApp::host_pad_mask_), which only moves on a press/release EDGE. Without
	 *  the sweep, a button held when focus is lost stays down in the guest forever,
	 *  and — worse — the mirror still believes it is down, so the NEXT press of that
	 *  button is skipped as a no-op and the button reads dead until it is pressed
	 *  twice. Releasing a code that isn't held is free (the core no-ops it). */
	public static void resetPadState() {
		synchronized (sStickLock) {
			// [player][stick][direction] since the Multitap landed, so this is two levels deep.
			for (int[][] player : sStickDir) {
				for (int[] stick : player) java.util.Arrays.fill(stick, 0);
			}
		}
		try {
			resetKeyStatus();
			// Every player, not just player 0: with a Multitap attached the guest holds four
			// pads' worth of bits, and leaving players 2-4 pressed is the same "button reads
			// dead until pressed twice" bug this whole sweep exists to prevent.
			for (int player = 0; player < MAX_PLAYERS; player++) {
				for (int code : PAD_DIGITAL_CODES) setPadButtonForPort(player, code, 0, false);
				if (player == 0) {
					setPadAnalog(0, 0x80, 0x80);
					setPadAnalog(1, 0x80, 0x80);
				} else {
					setPadAnalogForPlayer(player, 0, 0x80, 0x80);
					setPadAnalogForPlayer(player, 1, 0x80, 0x80);
				}
			}
		} catch (Throwable ignored) {
		}
	}
	/** Local co-op: hot-plug a 2nd DualShock2 into PS2 port 2 when a second physical
	 *  controller joins. Idempotent; briefly parks the VM to rebuild the pad list. */
	public static void enablePad2() {  }
	/** PS2 Multitap: enable/disable the 3 extra pad slots on one physical port
	 *  (port 0 = PS2 port 1 / unified slots 2,3,4; port 1 = PS2 port 2 / slots 5,6,7).
	 *  Idempotent; briefly parks the VM (up to ~3s) to rebuild the pad list, so call
	 *  it OFF the UI thread. */
	public static void setMultitap(int port, boolean enabled) {  }
	/** Implemented natively: drops every pad bit the host is holding down. */
	public static native void resetKeyStatus();

	// ---- USB keyboard (#254: EQOA / Konami-keyboard games) ----
	/** Attach ({@code true}) or detach ({@code false}) an emulated USB HID
	 *  keyboard on USB port {@code port} (0 = USB1, 1 = USB2). Persists
	 *  [USB{port+1}] Type = hidkbd/None and, when a VM is running, recreates the
	 *  device live so the game sees the (dis)connect. Call off the UI thread — a
	 *  live change briefly parks the emulation pipeline. */
	public static void usbSetKeyboardEnabled(int port, boolean enabled) {  }
	/** Feed one Android hardware {@link android.view.KeyEvent} to the emulated USB
	 *  keyboard on {@code port}. {@code androidKeyCode} is {@code KeyEvent.keyCode};
	 *  {@code pressed} is the down/up state. Returns {@code true} iff a USB keyboard
	 *  is attached to that port AND the key mapped to a HID usage — i.e. the event
	 *  was consumed by the emulated keyboard and should NOT also drive the pad /
	 *  frontend. No-op (returns {@code false}) otherwise. */
	public static boolean usbKeyboardKey(int port, int androidKeyCode, boolean pressed) { return false; }

	// ---- Controller rumble (BT/USB gamepads via Android InputDevice) ----
	// Device id of the most-recently-used gamepad, set from MainActivityRuntime.dispatchKeyEvent.
	public static volatile int sRumbleDeviceId = -1;
	// Master enable (default on).
	public static volatile boolean sRumbleEnabled = true;
	// One-shot length; re-issued when the game changes intensity, cancelled on
	// zero. Long enough to cover sustained rumble between intensity changes.
	private static final int RUMBLE_MS = 3000;

	/** Called from native (IOP thread) when PS2 pad motor intensity changes for
	 *  [pad] (unified slot: 0 = Player 1, 1 = Player 2). largeMotor/smallMotor are
	 *  0..255. Local co-op: routes the rumble to THAT player's controller. Falls
	 *  back to the last-used gamepad when the port isn't claimed yet (single-player,
	 *  or before first input) — solo play is unchanged. No-op with no vibrator. */
	public static void onPadRumble(int pad, int largeMotor, int smallMotor) {
		if (!sRumbleEnabled) return;
		int devId = com.armsx2.input.PadRouter.INSTANCE.deviceIdForPort(pad);
		if (devId < 0) devId = sRumbleDeviceId;
		// devId may stay -1 for touch-only Player 1 (no gamepad); vibrateDevice still
		// drives the device's own haptic for P1 (issue #241). P2 with no pad has no target.
		if (devId < 0 && pad != 0) return;
		float low = Math.max(0f, Math.min(1f, largeMotor / 255f));   // low-frequency / large
		float high = Math.max(0f, Math.min(1f, smallMotor / 255f));  // high-frequency / small
		vibrateDevice(devId, low, high, RUMBLE_MS, pad == 0);
	}

	// ---- Achievement / notification sound playback ----
	// Called from native Common::PlaySoundAsync (RetroAchievements unlock/info/
	// leaderboard-submit .wav). Fire-and-forget; must never throw back to JNI.
	// Uses MediaPlayer, not SoundPool: SoundPool decoded/resampled the 44.1 kHz
	// stereo PCM oddly and it came out "weird". MediaPlayer plays the .wav straight,
	// matching desktop PCSX2. One short-lived player per shot, released on complete.
	// Strong references to the currently-playing sound players. Without this the
	// MediaPlayer below is a pure local; once start() returns and the worker thread
	// exits, nothing roots it, so the GC (very active under a running emulator) could
	// finalize and release it MID-PLAYBACK — that's why unlock sounds dropped at random
	// with no error logged. Held from before start() until the completion/error callback.
	private static final java.util.Set<android.media.MediaPlayer> sActiveSounds =
			java.util.Collections.synchronizedSet(new java.util.HashSet<>());

	/** Volume (0..1) for RA unlock / info / leaderboard-submit sounds. Set from Kotlin
	 *  (AchievementsViewModel.setSoundVolume) so a slider tames the effect without editing the
	 *  .wav. 1.0 = the sound as authored. */
	public static volatile float sSoundVolume = 1.0f;

	/** A RetroAchievements toast: signing in (with the user's avatar and score), the game summary
	 *  when a disc is identified at boot (with its box art), an achievement unlock (with its
	 *  badge), a leaderboard attempt, a completed set, a saved sign-in that expired.
	 *
	 *  Called from native (frontend/android_jni.cpp AchievementsNotify) on the emulation thread,
	 *  by the notice pump in frontend/achievements.cpp — never straight out of an rc_client
	 *  callback, which can land on any thread. Hops to the main looper here because
	 *  {@link com.armsx2.ui.RaToasts} is Compose state, and the overlay that renders it is hosted
	 *  once in WindowImpl so it shows over the game and over the library alike.
	 *
	 *  [kind] is an ARMSX_ACH_NOTICE_* value (frontend/achievements.h) and only picks the caption,
	 *  accent and fallback icon — an unknown one must still render. [key] collapses repeats: a
	 *  notice whose key is already on screen replaces it instead of stacking. [imageUrl] is an
	 *  https RA image, fetched by Coil off the shared cover cache; "" means draw the fallback icon.
	 *  [durationMs] is the user's configured duration for this category (achievement and
	 *  leaderboard notifications are timed separately), already clamped to 3..30 s natively.
	 *
	 *  Every String is non-null by construction on the native side. Must never throw back to JNI. */
	public static void onAchievementNotice(int kind, String key, String title, String detail,
	                                       String imageUrl, int durationMs) {
		if (title == null || title.isEmpty()) return;
		final String safeKey = key != null ? key : "";
		final String safeDetail = detail != null ? detail : "";
		final String safeImage = imageUrl != null ? imageUrl : "";
		try {
			new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
				try {
					com.armsx2.ui.RaToasts.show(kind, safeKey, title, safeDetail, safeImage, durationMs);
				} catch (Throwable t) {
					android.util.Log.e("ARMSX2", "achievement notice failed", t);
				}
			});
		} catch (Throwable t) {
			android.util.Log.e("ARMSX2", "achievement notice post failed", t);
		}
	}

	public static void playSound(String path) {
		if (path == null || path.isEmpty()) return;
		// An RA sound means the set just changed state, so re-read the counts now rather than waiting
		// for the slow poll — this is what makes the library's progress figure move as you play.
		// Off-thread because it builds and parses the set JSON, and this call is on the emu thread.
		new Thread(com.armsx2.AchievementsProgress::snapshotCurrentGame, "ach-progress").start();
		// Cap concurrent players — a burst of simultaneous unlocks (combo/milestone) could
		// otherwise exhaust the device's MediaPlayer/codec pool and make start() no-op.
		if (sActiveSounds.size() >= 4) return;
		new Thread(() -> {
			android.media.MediaPlayer mp = null;
			try {
				mp = new android.media.MediaPlayer();
				mp.setAudioAttributes(new android.media.AudioAttributes.Builder()
						// USAGE_GAME, not ASSISTANCE_SONIFICATION: the unlock jingle is game audio and
						// must play on the media/game path. SONIFICATION is a UI/system-feedback usage
						// that Do Not Disturb silences — which is why cheevo sounds went quiet with DND
						// on. Game/media audio is exempt from DND, so this plays regardless.
						.setUsage(android.media.AudioAttributes.USAGE_GAME)
						.setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION)
						.build());
				mp.setDataSource(path);
				mp.setOnCompletionListener(m -> { sActiveSounds.remove(m); try { m.release(); } catch (Throwable ignore) {} });
				mp.setOnErrorListener((m, what, extra) -> { sActiveSounds.remove(m); try { m.release(); } catch (Throwable ignore) {} return true; });
				sActiveSounds.add(mp);
				mp.prepare();
				mp.setVolume(sSoundVolume, sSoundVolume);
				mp.start();
			} catch (Throwable t) {
				if (mp != null) { sActiveSounds.remove(mp); try { mp.release(); } catch (Throwable ignore) {} }
				android.util.Log.e("ARMSX2", "playSound failed: " + path, t);
			}
		}, "armsx2-ra-sound").start();
	}

	/** Drive [devId]'s vibrator(s) with the PS2 large/high motor intensities for [ms].
	 *  When the controller exposes no usable vibrator and [allowSystemFallback] is set,
	 *  drive the device's own haptic motor instead (issue #241 — handhelds like the
	 *  Odin 3 whose built-in gamepad has no rumble actuator, only system haptics). */
	private static void vibrateDevice(int devId, float low, float high, int ms, boolean allowSystemFallback) {
		try {
			// Single combined motor can't reproduce both PS2 actuators, so blend
			// them the way AetherSX2/NetherSX2 do (org.libsdl.app
			// SDLControllerManager): 0.6*large + 0.4*small. The PS2 small motor is
			// BINARY (full-scale 0xff whenever it pulses), so the old Math.max()
			// slammed the lone motor to FULL on every small-motor buzz — it felt
			// like the large motor was firing for small-motor events. The weighted
			// mix keeps a small-only pulse light and distinct from a large pulse.
			float combined = Math.min(1f, low * 0.6f + high * 0.4f);
			boolean drove = false;
			InputDevice dev = (devId >= 0) ? InputDevice.getDevice(devId) : null;
			if (dev != null) {
				if (Build.VERSION.SDK_INT >= 31) {
					VibratorManager vm = dev.getVibratorManager();
					int[] ids = vm.getVibratorIds();
					if (ids.length >= 2) {
						drove = rumbleOne(vm.getVibrator(ids[0]), low, ms);
						drove |= rumbleOne(vm.getVibrator(ids[1]), high, ms);
					} else if (ids.length == 1) {
						drove = rumbleOne(vm.getVibrator(ids[0]), combined, ms);
					} else {
						// Some pads (e.g. certain DualShock/DualSense BT modes) expose 0
						// vibrators to VibratorManager but still drive via the legacy API.
						drove = rumbleOne(dev.getVibrator(), combined, ms);
					}
				} else {
					drove = rumbleOne(dev.getVibrator(), combined, ms);
				}
			}
			// No controller actuator handled it → fall back to the device's built-in
			// haptic (issue #241), when permitted (Player 1 / explicit test) so a
			// vibrator-less P2 pad never buzzes the handheld that P1 is holding.
			if (!drove && allowSystemFallback) {
				rumbleOne(systemVibrator(), combined, ms);
			}
		} catch (Throwable ignored) {
		}
	}

	/** User-set haptic strength multiplier (0..2, default 1.0 = as authored). Scales EVERY
	 *  vibration — controller rumble AND touch ticks both funnel through rumbleOne — so one
	 *  "Vibration Strength" slider tames or boosts all of it. Set from Kotlin
	 *  (ControllerMappings.setHapticIntensity) live and at app start. */
	public static volatile float sHapticScale = 1.0f;

	/** @return true if [v] is a real, usable vibrator that was driven (or cancelled). */
	private static boolean rumbleOne(Vibrator v, float intensity, int ms) {
		if (v == null || !v.hasVibrator()) return false;
		intensity *= sHapticScale;
		if (intensity <= 0f) {
			try { v.cancel(); } catch (Throwable ignored) {}
			return true;
		}
		int amp = Math.round(intensity * 255f);
		if (amp < 1) amp = 1;
		if (amp > 255) amp = 255;
		try {
			v.vibrate(VibrationEffect.createOneShot(ms, amp));
		} catch (Throwable t) {
			try { v.vibrate(ms); } catch (Throwable ignored) {}
		}
		return true;
	}

	// The device's own haptic motor (system vibrator), resolved once. On handhelds
	// like the Odin 3 the built-in gamepad exposes no rumble actuator — only this —
	// so it's the fallback target when a controller has no usable vibrator (issue #241).
	private static volatile Vibrator sSystemVibrator;
	private static Vibrator systemVibrator() {
		Vibrator v = sSystemVibrator;
		if (v != null) return v;
		try {
			Context ctx = getContext();
			if (ctx != null) {
				if (Build.VERSION.SDK_INT >= 31) {
					VibratorManager vm = (VibratorManager) ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
					v = (vm != null) ? vm.getDefaultVibrator() : null;
				} else {
					v = (Vibrator) ctx.getSystemService(Context.VIBRATOR_SERVICE);
				}
				if (v != null) sSystemVibrator = v;
			}
		} catch (Throwable ignored) {
		}
		return v;
	}

	// Short crisp haptic "tick" for on-screen touch button presses (issue #247),
	// PPSSPP/Azahar-style. Driven by the device's own vibrator and INDEPENDENT of
	// game rumble. The UI gates it via the Touch Haptics setting, so this is only
	// invoked when enabled. Coalesced: simultaneous multi-touch presses (d-pad +
	// face land in the same frame) collapse to ONE tick, and fast mashing is rate-
	// limited, so the vibrator queue can't be saturated on low-end devices.
	private static volatile long sLastTouchHapticMs = 0L;
	public static void touchHaptic() {
		long now = android.os.SystemClock.uptimeMillis();
		if (now - sLastTouchHapticMs < 24L) return;
		sLastTouchHapticMs = now;
		try { rumbleOne(systemVibrator(), 0.6f, 12); } catch (Throwable ignored) {}
	}

	/** Index (0-based) of the [index]th connected physical gamepad, or -1. Used as a
	 *  fallback so the rumble test works even before a port has been claimed in-game. */
	private static int nthGamepadDeviceId(int index) {
		int n = 0;
		for (int id : InputDevice.getDeviceIds()) {
			InputDevice d = InputDevice.getDevice(id);
			if (d == null) continue;
			int src = d.getSources();
			boolean pad = (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
					|| (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
			if (!pad) continue;
			if (n == index) return id;
			n++;
		}
		return -1;
	}

	/** Strongly buzz the controller mapped to [port] (0 = P1, 1 = P2) for ~500ms.
	 *  Falls back to the Nth gamepad when no port is claimed yet (tested outside a game). */
	public static void testRumble(int port) {
		int devId = com.armsx2.input.PadRouter.INSTANCE.deviceIdForPort(port);
		if (devId < 0) devId = nthGamepadDeviceId(port);
		// devId may stay -1 (touch-only / Odin built-in with no rumble); vibrateDevice
		// then falls back to the device's own haptic so the test still buzzes (issue #241).
		vibrateDevice(devId, 0.9f, 0.9f, 500, true);
	}

	/** One-line report of [port]'s controller and whether Android exposes any vibrator
	 *  for it (new VibratorManager + legacy API). Lets the Pad tab tell the user whether
	 *  a missing rumble is a routing issue or the pad just isn't drivable by Android. */
	public static String rumbleStatusForPort(int port) {
		int devId = com.armsx2.input.PadRouter.INSTANCE.deviceIdForPort(port);
		boolean mapped = devId >= 0;
		if (devId < 0) devId = nthGamepadDeviceId(port);
		if (devId < 0) return "Player " + (port + 1) + ": no controller found";
		InputDevice d = InputDevice.getDevice(devId);
		String name = (d != null && d.getName() != null) ? d.getName() : ("device " + devId);
		int vmCount = 0;
		boolean legacy = false;
		try {
			Vibrator lv = (d != null) ? d.getVibrator() : null;
			legacy = lv != null && lv.hasVibrator();
		} catch (Throwable ignored) {}
		if (Build.VERSION.SDK_INT >= 31 && d != null) {
			try { vmCount = d.getVibratorManager().getVibratorIds().length; } catch (Throwable ignored) {}
		}
		boolean hasRumble = vmCount > 0 || legacy;
		int motors = Math.max(vmCount, legacy ? 1 : 0);
		return "Player " + (port + 1) + ": " + name
				+ (mapped ? "" : " (not active in-game yet)")
				+ (hasRumble ? " — rumble OK (" + motors + " motor" + (motors == 1 ? "" : "s") + ")"
						: " — NO rumble exposed by Android");
	}

	public static void setAspectRatio(int type) {  }
	public static void setFmvAspectRatio(int type) {  }
	public static void speedhackLimitermode(int value) {  }
	/** Fast-forward speed multiplier (Turbo scalar, 0.05-10.0). Set before engaging
	 *  Turbo (speedhackLimitermode(1)); the FF-speed slider uses Unlimited (mode 3) at its top. */
	public static void setTurboScalar(float scalar) {  }
	/** Custom speed / FPS cap as a percent of native (100 = full speed).
	 *  Applies live to the running VM's frame pacer. */
	public static void setNominalSpeed(int percent) {  }
	/** Cap presented frames per second (0 = uncapped). Throttles only the
	 *  display swap, so emulation keeps running at 100% speed while the
	 *  on-screen FPS is limited. Applies live. */
	public static void setFpsCap(int fps) {  }
	/** Per-region emulated PS2 vsync rate (NTSC / PAL Hz), applied live without a
	 *  restart — recomputes the vsync pacer + target speed. Parks the VM briefly
	 *  (keeps audio alive), so call it off the UI thread (via LiveGsApplyQueue). */
	public static void applyFramerateLive(float ntsc, float pal) {  }
	/** PCSX2's frame skip. STILL A STUB, and deliberately so — do not wire anything to it.
	 *  ARMSX1 has real frame skip, but it is one field of the pacing policy and goes through
	 *  {@link #setSpeedLimits}'s frameSkip parameter (settings.toml [runtime] frame_skip).
	 *  Calling this instead is how the feature looked implemented for a release and reached
	 *  nothing. */
	public static void setFrameSkip(int skip) {  }

	/** GitHub #375: top-align the render in portrait (true) vs vertical-center (false). */
	public static native void setPortraitRenderTop(boolean top);

	/** Pixels to keep clear at the top of a PORTRAIT render for a punch-hole/notch camera. Taken
	 *  from the window's display cutout; 0 on devices without one. Only affects portrait
	 *  top-aligned output. */
	public static native void setPortraitRenderTopInset(int pixels);
	/** SPU2 output volume, percent (0..200). Applies live + persists. */
	public static void setAudioVolume(int volume) {  }
	/** Mute/unmute SPU2 output. Applies live + persists. */
	public static void setAudioMuted(boolean muted) {  }
	/** Swap final stereo output channels L&lt;-&gt;R (flipped-speaker devices). Applies live + persists. */
	public static void setAudioSwapChannels(boolean swap) {  }
	public static void speedhackEecyclerate(int value) {  }
	public static void speedhackEecycleskip(int value) {  }
	public static void setInstantVU1(boolean enabled) {  }

	public static void renderUpscalemultiplier(float value) {  }
	public static void renderMipmap(int value) {  }
	public static void renderHalfpixeloffset(int value) {  }
	public static void renderTvShader(int value) {  }
	public static void renderShadeBoost(boolean enabled, int brightness, int contrast, int saturation, int gamma) {  }
	public static void renderSoftware() {  }
	public static void renderOpenGL() {  }
	public static void renderVulkan() {  }
	public static void renderAuto() {  }
	// When true, the Auto renderer resolves to Vulkan HW instead of OpenGL (set for Adreno devices).
	public static void setPreferVulkan(boolean enabled) {  }
	/** Affinity Control Mode: which CPU cores the emulation thread is allowed to run on.
	 *  0 off (scheduler decides, the default) · 1 Performance cores · 2 All cores.
	 *  7 is accepted as an alias for 1: the numbering is inherited from the PS2 build, where
	 *  1-6 ordered the EE/VU/GS threads — three threads a PlayStation emulator does not have.
	 *  Anything else is treated as off.
	 *
	 *  The performance cluster is DETECTED at runtime from each core's cpuinfo_max_freq, never
	 *  assumed from core index. Only the emulation thread is pinned; the audio thread and the
	 *  background workers are deliberately left to the scheduler (frontend/perf_hint.h).
	 *
	 *  EXPERIMENTAL. Android's EAS scheduler usually beats hand-pinning, so off stays the
	 *  recommendation. Applies live and also honoured at boot — safe to set before
	 *  runVMThread. Implemented natively in frontend/perf_hint.c. */
	public static native void setAffinityMode(int mode);
	public static void renderPreloading(int value) {  }

	// toggleTextureDumping() lived here and was an empty stub returning false, wired to a
	// hotkey that therefore did nothing. PS1 texture dumping is a real feature now
	// (psx/texrep.h) and its hotkey flips [video] texture_dump through the settings store,
	// so the state survives the session and matches what the Video tab shows —
	// MainActivityRuntime.toggleTextureDump(). There is no live native toggle by design.

	/** Create a memory card in the memcards folder. type: 1=File, 2=Folder.
	 *  fileType (File only): 1=8MB, 2=16MB, 3=32MB, 4=64MB. Returns success. */
	public static boolean createMemoryCard(String name, int type, int fileType) {
		// PS1 memory card: a 128 KiB blank image the core will format on first use.
		try {
			Context ctx = getContext();
			if (ctx == null || name == null || name.isEmpty()) return false;
			java.io.File dir = new java.io.File(ctx.getFilesDir(), "memcards");
			if (!dir.exists() && !dir.mkdirs()) return false;
			java.io.File card = new java.io.File(dir, name.endsWith(".mcd") ? name : name + ".mcd");
			if (card.exists()) return true;
			try (java.io.FileOutputStream fos = new java.io.FileOutputStream(card)) {
				byte[] chunk = new byte[8192];
				for (int i = 0; i < 16; i++) fos.write(chunk);
			}
			return true;
		} catch (Throwable t) { return false; }
	}
	public static boolean isMemoryCard(String name) {
		if (name == null || name.isEmpty()) return false;
		return name.toLowerCase(java.util.Locale.US).endsWith(".mcd");
	}

	// ---- In-process rendering surface (frontend/android_jni.cpp) ----
	// The Compose-hosted SurfaceView forwards its holder's Surface here; the core renders
	// into it from this same process, so the touch overlay / pause overlay / hotkeys /
	// RetroAchievements / Discord presence all work. Call onNativeSurfaceChanged BEFORE
	// runVMThread.
	public static native void onNativeSurfaceCreated();
	public static native void onNativeSurfaceChanged(Surface surface, int w, int h);
	public static native void onNativeSurfaceDestroyed();
	public static void setDisplayRefreshRate(float hz) {  }

	/** Implemented natively. BLOCKING: boots [path] and runs the emulation loop until
	 *  shutdown(). Call on a dedicated thread, after onNativeSurfaceChanged. */
	public static native boolean runVMThread(String path);
	public static native void pause();
	public static native void resume();
	/** Implemented natively: park (or restart) the PLATFORM audio stream because the Activity
	 *  went off-screen / came back.
	 *
	 *  ★ NOT the same thing as {@link #pause()}. pause() freezes the VM, and the core answers by
	 *  calling SDL_PauseAudioDevice() — which only makes SDL's audio thread hand the backend
	 *  SILENCE. The Android OpenSL ES player is left PLAYING, so AudioFlinger keeps our track
	 *  active, the speaker path never reaches standby, and audioserver holds a PARTIAL_WAKE_LOCK
	 *  ('AudioMix', charged to this app) for as long as we are backgrounded — on a sleeping
	 *  Retroid Pocket 6 it was the only wake lock left on the device. That is what let the tail
	 *  of the mix trickle out of a switched-off screen. This CLOSES the device
	 *  (ArmsxSession::setAudioSuspended), and reopens it on the way back.
	 *
	 *  Ignored by the core when settings.toml's [audio] background_playback is true. */
	public static native void setAudioSuspended(boolean suspended);

	/** App off-screen: stop drawing and posting frames. Distinct from pausing the VM —
	 *  the pause menu is on-screen and must keep presenting — and from suspending audio,
	 *  which [audio] background_playback may legitimately keep alive. */
	public static native void setPresentationSuspended(boolean suspended);
	/** Implemented natively: hard reset of the running game. */
	public static native void resetGame();
	/** Implemented natively: absolute fast-forward on/off. The speed it runs at comes from
	 *  {@link #setSpeedLimits}. */
	public static native void setFastForward(boolean enabled);
	/** Implemented natively: the whole frame-pacing policy, applied live.
	 *  frameLimit false runs unthrottled; speedPercent is 10..1000 % of the game's own rate;
	 *  fpsLimit is an extra absolute cap in fps (0 = off); fastForwardSpeed is the multiplier
	 *  used instead of speedPercent while fast-forward is engaged (0 = uncapped).
	 *  <p>frameSkip is NOT a speed control and nothing above reads it — it drops PRESENTS while
	 *  the machine keeps running at full speed: 0 off, 1..5 fixed (present 1 of every N+1),
	 *  -1 adaptive (drop one only when the frame pacer reports the device fell behind). It rides
	 *  this call because the same panel edits it and the whole policy is pushed at once.
	 *  Persisted in settings.toml's [runtime] table, which the core reads at boot. */
	public static native void setSpeedLimits(boolean frameLimit, int speedPercent, int fpsLimit, float fastForwardSpeed, int frameSkip);
	/** Implemented natively: queue a game path/URI for the running loop to boot, so a disc
	 *  swap does not need the process torn down. */
	public static native void enqueueLaunchArgument(String argument);
	// Keep the audio device alive across a menu/overlay pause (no reclaim, no
	// resume rebuild). Set true right before pauseForOverlay's pause(); resume()
	// clears it. See native setOutputPauseSuppressed / SPU2::SetOutputPauseSuppressed.
	public static void setOutputPauseSuppressed(boolean suppressed) {  }
	public static native void shutdown();
	public static native boolean hasActiveVM();

	/** Persist the Vulkan pipeline cache to disk so cold restarts don't have
	 *  to recompile every TFX pipeline. No-op for OpenGL (its cache flushes
	 *  on its own). Called from MainActivityRuntime.onPause so backgrounding the app saves
	 *  the cache before Android can reap the process. Safe to call when no
	 *  Vulkan device is active (becomes a no-op). */
	public static void flushShaderCache() {  }

	/**
	 * Probe a file descriptor for PS2 BIOS metadata. Used by the setup
	 * wizard's directory-based BIOS selector to enumerate candidates and
	 * show region/version per file. The fd MUST be detached (ownership
	 * transferred to native) before the call — emucore wraps it in a FILE*
	 * and closes it on return either way.
	 *
	 * Returns null if the file isn't a valid BIOS image.
	 */
	public static BiosInfo getBiosInfoFromFd(int fd) {
		android.os.ParcelFileDescriptor pfd = null;
		try {
			pfd = android.os.ParcelFileDescriptor.adoptFd(fd);
			long size = pfd.getStatSize();
			if (size < 128 * 1024 || size > 8L * 1024 * 1024) return null;
			java.io.FileInputStream fis = new java.io.FileInputStream(pfd.getFileDescriptor());
			byte[] buf = new byte[(int) Math.min(size, 1024 * 1024)];
			int read = 0, n;
			while (read < buf.length && (n = fis.read(buf, read, buf.length - read)) > 0) read += n;
			int region = 10;
			for (int i = 0; i + 4 <= read; i++) {
				if (buf[i] == 'S' && buf[i+1] == 'C' && buf[i+2] == 'E') {
					char c = (char) buf[i+3];
					if (c == 'I') { region = 0; break; }
					if (c == 'A') { region = 1; break; }
					if (c == 'E') { region = 2; break; }
				}
			}
			String zone = region == 0 ? "Japan" : region == 1 ? "USA" : region == 2 ? "Europe" : "Free";
			return new BiosInfo(0x0200, region, "PlayStation BIOS", zone);
		} catch (Throwable t) {
			return null;
		} finally {
			if (pfd != null) { try { pfd.close(); } catch (Exception ignored) {} }
		}
	}

	/**
	 * Read enough of a PS2 disc image to extract its serial (e.g.
	 * "SLUS-20312"). Walks the ISO9660 directory to find SYSTEM.CNF and
	 * parses the BOOT2 line. Handles flat ISO/raw-sector images and CHDs;
	 * CSO/ZSO/GZ still return null and the caller falls back to filename
	 * parsing. fd is consumed (closed by native).
	 */
	public static String getGameSerialFromFd(int fd) { return ""; }

	/**
	 * PCSX2 game-database compatibility lookup. Returns the raw 0-6
	 * Compatibility enum value:
	 *   0 Unknown, 1 Nothing, 2 Intro, 3 Menu, 4 InGame, 5 Playable, 6 Perfect
	 * Caller maps to the 5-star display.
	 */
	public static int getCompatibilityForSerial(String serial) { return 0; }

	/** GameDB region string for a serial ("NTSC-U", "PAL-E", "PAL-IN", "NTSC-C", "NTSC-K",
	 *  "NTSC-HK", ...), or "" if the serial isn't in the database. Lets the library show
	 *  the real region (India/China/Korea/HK) a serial prefix alone can't distinguish. */
	public static String getRegionForSerial(String serial) { return ""; }

	/** GameDB titles for a serial as "&lt;name&gt;\n&lt;name-sort&gt;\n&lt;name-en&gt;", or "" if the serial
	 *  isn't in the database. name-sort / name-en may be empty; name is set for any entry.
	 *
	 *  One call, one lookup — the library asks for every game it scans. For a Japanese game
	 *  name is the original title, name-sort its kana reading (sort by this, not the kanji),
	 *  and name-en the romanised one. */
	public static String getTitlesForSerial(String serial) { return ""; }

	/** Capture the machine into slot [slot] under &lt;filesDir&gt;/savestates/.
	 *
	 *  BLOCKS: the request is parked and run by the emulation thread from psx_update(), so this
	 *  waits up to two seconds for a frame boundary. Call it off the main thread. Returns false
	 *  (and logs the reason under ARMSX-JNI) if no VM is running or the write failed. */
	public static native boolean saveStateToSlot(int slot);
	/** True while the emulated memory card is mid-write, when a state save is refused to protect
	 *  the card. The counter only ticks down while the VM runs, so it does NOT clear while paused. */
	public static boolean isMemcardBusy() { return false; }
	/** Restore slot [slot]. Same blocking contract as {@link #saveStateToSlot(int)}. False if the
	 *  slot is empty, was written by an incompatible build, or holds a different disc.
	 *
	 *  Does NOT check memory-card divergence — it has no way to ask the user, and a boolean
	 *  cannot carry the difference between "broken" and "are you sure". Use
	 *  {@link #loadStateFromSlotChecked(int, boolean)} through
	 *  {@code com.armsx2.ui.saves.SaveStateGuard} for anything the user drives. */
	public static native boolean loadStateFromSlot(int slot);
	/** Restore slot [slot], returning the raw core result code instead of a boolean.
	 *
	 *  0 is success. {@link #STATE_ERR_CARD_NEWER} and {@link #STATE_ERR_CARD_DIVERGED} are
	 *  ADVISORY: the state is intact and nothing has been applied to the machine, but the
	 *  memory cards no longer hold what the state was taken against. Ask, then call again with
	 *  ignoreCardDivergence = true. Any other negative value is a real failure.
	 *
	 *  Same blocking contract as {@link #saveStateToSlot(int)}. */
	public static native int loadStateFromSlotChecked(int slot, boolean ignoreCardDivergence);

	/** psx/state.h PSX_STATE_OK. */
	public static final int STATE_OK = 0;
	/** psx/state.h PSX_STATE_ERR_CARD_NEWER — the game saved to the card AFTER this state was
	 *  taken, so loading it puts the console behind its own card. The dangerous direction. */
	public static final int STATE_ERR_CARD_NEWER = -15;
	/** psx/state.h PSX_STATE_ERR_CARD_DIVERGED — the card differs, but nothing says which way. */
	public static final int STATE_ERR_CARD_DIVERGED = -16;
	/** Disc path recorded for [slot], or "" when the slot holds nothing for the mounted disc.
	 *  The save/load picker uses non-empty here to mean "occupied", and disables the tile for
	 *  LOADING when it is empty — so while this was a stub returning "", every slot looked empty
	 *  and Load was permanently greyed out even though states were being written. */
	public static native String getGamePathSlot(int slot);
	/** PNG preview for [slot], or an empty array when that state has none (states written
	 *  before previews existed simply have no THUMB section — not an error). Never null. */
	public static native byte[] getImageSlot(int slot);
	/** PNG preview read straight from a state file. Empty array when it has none. Never null. */
	public static native byte[] getSaveStateImage(String path);

	// Hot-swap the CDVD disc on the running VM (keeps the session alive, cycles
	// the tray so the game detects the new disc). Returns false if there's no
	// valid VM or the new image failed to open (in which case the core has
	// already reverted to the previous disc). Used by the in-game Swap Disc
	// picker for CodeBreaker / multi-disc swaps. Call off the main thread — it
	// parks the CPU thread and blocks until the swap completes.
	/** Swap the running game's disc. Accepted asynchronously: the swap is applied on the
	 *  emulation thread at the next instruction boundary, so true means "queued", not "done". */
	public static native boolean changeDisc(String path);

	// Autosave-on-exit slot. Backed by a dedicated `.autosave.p2s` filename
	// in the savestate folder (see VMManager::SAVESTATE_SLOT_AUTOSAVE) so the
	// numbered slots 0-9 stay user-controlled. saveAutosaveState is called
	// from the in-game "Save State And Exit" menu; hasAutosaveState gates
	// the load picker's autosave tile.
	public static boolean saveAutosaveState() { return false; }
	public static boolean loadAutosaveState() { return false; }
	public static boolean hasAutosaveState() { return false; }
	public static byte[] getAutosaveImage() { return new byte[0]; }
	public static String getAutosaveGamePath() { return ""; }
	// Frames the GS has presented since it opened (host-side, not saved in the state). The
	// auto-load-on-boot path waits until this is advancing before restoring, so the load happens
	// once the renderer is actually presenting — otherwise the restored frame never reaches the
	// surface and the screen stays black.
	public static native int getPresentedFrameCount();

	// ---- cheats (PS1) ---------------------------------------------------------
	//
	// GameShark codes out of a `.cht` catalogue, applied to R3000A memory once per emulated
	// frame by psx/cheats.c. NOT the PS2 patch machinery: PNACH is EE-shaped and keyed by
	// <SERIAL>_<CRC>, neither of which exists on this machine. See psx/cheats.h for why.
	//
	// All four are real. The contract callers must keep: only drive these for the game that
	// is actually RUNNING. The catalogue is process-global, so loading another game's file
	// while a session is live would swap the running game's codes out from under it. A cheat
	// selection made for a game that is not running belongs in settings.toml (`[cheats]`,
	// through com.armsx2.config.Ps1SettingsStore) and takes effect at that game's next launch.

	/** Parse `path` as the current cheat catalogue, replacing whatever was loaded.
	 *  Returns the number of entries, or -1 when the file cannot be read. "" clears —
	 *  which is how "this game has no cheat file" is expressed. Arms nothing on its own. */
	public static native int cheatsLoad(String path);

	/** The catalogue as the CORE parsed it, so the app's own reader can be checked against it
	 *  instead of assumed equal. Records separated by U+001E; fields by U+001F:
	 *  name, code-line count, "1"/"0" for "uses a code type this build does not implement",
	 *  description. NEVER null — an empty catalogue is "". */
	public static native String cheatsList();

	/** Arm exactly `enabledNames` from the loaded catalogue and apply from the next frame.
	 *  `master` false disarms everything regardless of the list. Returns the number armed, or
	 *  -1 when RetroAchievements hardcore is active — in which case nothing is armed and the
	 *  caller should say so rather than showing switches that do nothing. */
	public static native int cheatsApply(boolean master, String[] enabledNames);

	/** How many entries the running machine has armed right now. Shown in the UI instead of a
	 *  count of ticked boxes, so "you ticked five" and "five are running" cannot disagree. */
	public static native int cheatsArmedCount();

	// Discord lives in the :discord process now, not in emucore — see
	// com.armsx2.discord.DiscordNative. ARMSX2 is GPL-3.0+ and the Social SDK is proprietary, so
	// the two are kept as separate programs talking over IPC rather than one linked binary.
	// Re-declaring those natives here would not link: emucore does not contain them.

	public static void vmSetPaused(boolean paused) {
		new Handler(Looper.getMainLooper()).post(() -> {
			// Pause/resume callbacks can arrive after the user has already
			// requested Close Game / Reset. Do not let a stale resume flip the
			// Compose state back to RUNNING while the native VM is unwinding.
			if (MainActivityRuntime.isVmStopInProgress())
				return;
			if (!paused && MainActivityRuntime.eState.getValue() == EmuState.STOPPED)
				return;
			if (paused) {
				MainActivityRuntime.eState.setValue(EmuState.PAUSED);
			} else {
				MainActivityRuntime.eState.setValue(EmuState.RUNNING);
				// One-shot auto-load of the autosave state, if the user enabled
				// "Auto-load last state on boot" (no-op otherwise).
				MainActivityRuntime.onVmRunning();
			}
		});
	}

	// Call jni
	public static int openContentUri(String uriString) {
		Context _context = getContext();
		if(_context != null) {
			ContentResolver _contentResolver = _context.getContentResolver();
			try {
				ParcelFileDescriptor filePfd = _contentResolver.openFileDescriptor(Uri.parse(uriString), "r");
				if (filePfd != null) {
					return filePfd.detachFd();  // Take ownership of the fd.
				}
			} catch (Exception ignored) {}
		}
		return -1;
	}

	// Fallback directory creation for native FileSystem::CreateDirectoryPath.
	// On Android 11+ FUSE-emulated external storage a raw libc mkdir() can be
	// denied (EACCES/EPERM) for MANAGE_EXTERNAL_STORAGE apps even though the
	// Java File API succeeds — which is why FOLDER memory cards failed to
	// format ("Format failed!") on a custom data folder while file cards
	// worked. Returns true if the directory exists after the call.
	public static boolean createDirectoryPath(String path) {
		if (path == null || path.isEmpty()) return false;
		try {
			java.io.File dir = new java.io.File(path);
			if (dir.isDirectory()) return true;
			dir.mkdirs();
			return dir.isDirectory();
		} catch (Throwable t) {
			return false;
		}
	}

	// Fallback file creation for native FileSystem::OpenCFile. On Android 11+
	// FUSE-emulated external storage a raw libc fopen(O_CREAT) can be denied
	// (EACCES/EPERM) even though the Java File API succeeds — the same split that
	// forced createDirectoryPath above. Creating the empty file here lets the
	// native truncating write ("w"/"wb") that follows open the now-existing file,
	// which FUSE permits — which is what makes NEW folder-card saves work on a
	// custom data folder instead of crashing. Returns true if the file exists after.
	public static boolean createFilePath(String path) {
		if (path == null || path.isEmpty()) return false;
		try {
			java.io.File file = new java.io.File(path);
			if (file.isFile()) return true;
			java.io.File parent = file.getParentFile();
			if (parent != null && !parent.isDirectory()) parent.mkdirs();
			return file.createNewFile() || file.isFile();
		} catch (Throwable t) {
			return false;
		}
	}
}
