package com.armsx2.core

import android.content.Context
import android.content.Intent
import com.nanodata.armsx.EmulatorActivity
import java.io.File
import java.util.Locale

/**
 * The single seam between the ported Compose front-end and the ARMSX (PS1) native core.
 *
 * ARMSX2 talked to its PS2 core through ~134 in-process JNI methods (`NativeApp.java`) because the
 * game ran inside the same activity. ARMSX's launcher-shell model needs far less: the core runs in
 * a separate SDL2 activity, so this object only has to do two things —
 *
 *  1. **Launch** a game by starting [EmulatorActivity] with the native `argv` it already parses
 *     (`getArguments()` reads [EXTRA_NATIVE_ARGS]). The core accepts a bare positional path as the
 *     CDROM image, `--exe` for a PS-X executable, and `--cdrom` explicitly; disc images may also be
 *     handed over as an `armsx://` VIEW intent, but the extras array keeps us in-process and simple.
 *
 *  2. **Persist settings** by writing the same `settings.toml` the native `frontend/config.c`
 *     reads. That file lives under `SDL_GetPrefPath("nanodata","armsx")`, which on Android resolves
 *     to the app's internal files dir — i.e. [Context.getFilesDir]. Plain app-private POSIX, so
 *     Kotlin file I/O reaches it directly (see [Ps1Config]).
 *
 * There is no save-state / patch / achievement / texture-pack surface here: the PS1 core implements
 * none of those, so the corresponding ARMSX2 screens are intentionally absent from this port.
 */
object Ps1Native {

    /** Must match `EmulatorActivity.EXTRA_NATIVE_ARGS` (package-private there). */
    const val EXTRA_NATIVE_ARGS = "com.nanodata.armsx.EXTRA_NATIVE_ARGS"

    /** Disc-image extensions the core opens (`IsDiscPath`). */
    // "m3u" is a multi-disc PLAYLIST, not an image. It is bootable because the launch path
    // resolves it to its first existing disc (Ps1Playlist.resolveForLaunch) before the core
    // ever sees it — the core is never handed a .m3u.
    private val DISC_EXTS = setOf("cue", "bin", "iso", "img", "chd", "m3u")

    /** PS-X executable extensions (`IsExePath`). */
    private val EXE_EXTS = setOf("exe", "ps-exe", "psexe")

    /**
     * Hand [absolutePath] to the SDL2 runtime for play.
     *
     * @param extraArgs optional per-launch overrides appended before the path (e.g.
     *   `["--region", "ntsc"]`). Persistent settings should go through [Ps1Config] / `settings.toml`
     *   instead; use this only for one-shot overrides.
     */
    fun launchGame(context: Context, absolutePath: String, extraArgs: List<String> = emptyList()) {
        val args = ArrayList<String>(extraArgs)
        args += argForPath(absolutePath)

        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EXTRA_NATIVE_ARGS, args.toTypedArray())
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }
        context.startActivity(intent)
    }

    /** Launch the SDL runtime with no disc — boots to the BIOS (used by the drawer "Boot BIOS"). */
    fun launchBios(context: Context) {
        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EXTRA_NATIVE_ARGS, arrayOf("--bios-menu"))
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
        }
        context.startActivity(intent)
    }

    /** Map a path to the flag the core expects: `--exe` for executables, positional CDROM otherwise. */
    private fun argForPath(path: String): Array<String> {
        val ext = path.substringAfterLast('.', "").lowercase(Locale.US)
        return when (ext) {
            in EXE_EXTS -> arrayOf("--exe", path)
            else -> arrayOf(path) // bare positional → --cdrom (config.c). Handles disc + zip.
        }
    }

    /** True if [file] looks like something the core can boot (disc image, zip, or PS-X exe). */
    fun isBootable(file: File): Boolean {
        if (!file.isFile) return false
        val ext = file.name.substringAfterLast('.', "").lowercase(Locale.US)
        return ext in DISC_EXTS || ext in EXE_EXTS || ext == "zip"
    }
}
