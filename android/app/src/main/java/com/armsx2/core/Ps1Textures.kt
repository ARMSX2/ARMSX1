package com.armsx2.core

import android.content.Context
import com.armsx2.config.Ps1Settings
import com.armsx2.config.Ps1SettingsStore
import com.armsx2.runtime.MainActivityRuntime
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

/**
 * Pushes PS1 texture dumping / replacement (`psx/texrep.h`) into a RUNNING session.
 *
 * Mirrors [Ps1Display] and [Ps1Emulation] deliberately — same shape, same call sites — so
 * none of the three is forgotten when a settings screen is added. The failure this shape
 * exists to prevent is the one that has hit pads, save states, fast-forward and the FPS cap
 * in this port: a complete native implementation with **zero callers**, which looks exactly
 * like a dead setting.
 *
 * ## Where the files go
 *
 * The native side is handed ONE base directory and derives two from it:
 *
 * ```
 *   <root>/textures/<serial>/dump/           written by "Dump textures"
 *   <root>/textures/<serial>/replacements/   read by "Texture replacements"
 * ```
 *
 * The serial is resolved here, not in the core: the core has never been told which disc is
 * running, and a shared folder would make one game's pack match another game's textures by
 * accident whenever two discs happen to hold the same texel bytes. With no serial available
 * (no game booted, or a disc the core could not identify) the folder is `_common`, which is
 * still per-user and still valid — a pack dropped there simply applies to everything.
 *
 * A user-set [Ps1Settings.textureDir] wins outright and is passed through unchanged, so a
 * pack can live on shared storage.
 */
object Ps1Textures {

    /** Folder used when the core cannot name the disc. Never empty — an empty base directory
     *  makes the core fall back to its own preferences path, which on Android is not
     *  somewhere a user can put files. */
    private const val FALLBACK_SERIAL = "_common"

    /** Read the persisted settings and push them. Cheap; safe with no VM running.
     *  Resolved for the ACTIVE scope, so a per-game override keeps its own pack. */
    fun push(context: Context) {
        val s = runCatching { Ps1SettingsStore.active(context) }.getOrNull() ?: return
        push(context, s)
    }

    /** Push an in-memory snapshot, for callers that already hold one. */
    fun push(context: Context, s: Ps1Settings) {
        val dir = resolveDir(context, s)
        runCatching { NativeApp.setPs1TextureOptions(s.textureDump, s.textureReplacements, dir) }
    }

    /**
     * The base directory the two flags operate on. Public because the Video tab shows it —
     * "dumping is on" is useless without "and the files are HERE".
     */
    fun resolveDir(context: Context, s: Ps1Settings): String {
        if (s.textureDir.isNotBlank()) return s.textureDir

        val root = runCatching { MainActivityRuntime.assetCopyRoot(context) }.getOrNull()
            ?: context.filesDir.absolutePath
        val serial = runCatching { NativeApp.getGameSerial() }.getOrNull()
            ?.takeIf { it.isNotBlank() }
            ?: FALLBACK_SERIAL

        return File(File(root, "textures"), serial).absolutePath
    }

    /** `<base>/dump`, for the row that tells the user where dumps land. */
    fun dumpDir(context: Context, s: Ps1Settings): String =
        File(resolveDir(context, s), "dump").absolutePath

    /** `<base>/replacements`, for the row that tells the user where packs are read from. */
    fun packDir(context: Context, s: Ps1Settings): String =
        File(resolveDir(context, s), "replacements").absolutePath
}
