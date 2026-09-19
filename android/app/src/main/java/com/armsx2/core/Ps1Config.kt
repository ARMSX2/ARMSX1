package com.armsx2.core

import android.content.Context
import android.net.Uri
import java.io.File

/**
 * Locates (and, from milestone M3 on, edits) the native `settings.toml` the PS1 core reads.
 *
 * `frontend/config.c` resolves its config via `SDL_GetPrefPath("nanodata","armsx")`, which on
 * Android is the app-internal files dir. Because the launcher runs in the same app (applicationId
 * `com.nanodata.armsx`), [Context.getFilesDir] points at the exact same directory the core will
 * read, so the front-end can author the file directly with plain file I/O — no SAF, no root.
 *
 * For M1 this only exposes the path; the typed settings model + TOML writer land in M3.
 */
object Ps1Config {

    /** The file `frontend/config.c` loads (`<pref>/settings.toml`). May not exist until first run. */
    fun settingsFile(context: Context): File = File(context.filesDir, "settings.toml")

    /** BIOS the [com.nanodata.armsx.EmulatorActivity] copies from assets, if bundled. */
    fun bundledBiosFile(context: Context): File = File(context.filesDir, "bios.bin")

    /**
     * The user-imported BIOS. `EmulatorActivity.copyBundledBios()` only knows how to copy an
     * assets/bios.bin; a sideloaded user BIOS instead lands here and is wired to the core through
     * `[bios].override_file` (an absolute path config.c honours) — see the BIOS screen.
     */
    fun userBiosFile(context: Context): File = File(context.filesDir, "user_bios.bin")

    /** Copy a SAF-picked BIOS into app storage and return the imported file (or null on failure). */
    fun importBios(context: Context, uri: Uri): File? {
        val target = userBiosFile(context)
        val staging = File(target.parentFile, "${target.name}.importing")
        return try {
            val input = context.contentResolver.openInputStream(uri) ?: return null
            input.use { source ->
                staging.outputStream().use { output ->
                    val buffer = ByteArray(8192)
                    var bytes = 0L
                    while (true) {
                        val count = source.read(buffer)
                        if (count < 0) break
                        bytes += count
                        if (bytes > 1024 * 1024) return null
                        output.write(buffer, 0, count)
                    }
                    output.fd.sync()
                }
            }
            if (staging.length() != 512L * 1024 && staging.length() != 1024L * 1024) return null
            android.system.Os.rename(staging.absolutePath, target.absolutePath)
            target
        } catch (_: Exception) {
            null
        } finally {
            staging.delete()
        }
    }

    /** A PS1 memory-card file (slot1.mcd / slot2.mcd) under the native pref path. */
    fun memoryCardFile(context: Context, slot: Int): File =
        File(context.filesDir, "slot$slot.mcd")
}
