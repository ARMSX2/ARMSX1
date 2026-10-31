package com.armsx2.core

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.InputStream
import java.security.MessageDigest

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

    /** Legacy single-file import, retained for migration without deleting the original. */
    fun userBiosFile(context: Context): File = File(context.filesDir, "user_bios.bin")

    /** File choices in setup/the manager update the same model preference as Settings → BIOS. */
    fun modelForBios(file: File): String? = runCatching {
        val info = file.inputStream().use { Ps1BiosProbe.probe(it) } ?: return@runCatching null
        com.armsx2.config.Ps1Settings.MODELS.firstOrNull { Ps1BiosSelection.matches(info, it) }
    }.getOrNull()

    /** Copy a SAF-picked BIOS into app storage and return the imported file (or null on failure). */
    fun importBios(context: Context, uri: Uri): File? = runCatching {
        context.contentResolver.openInputStream(uri)?.use { importBios(context, it) }
    }.getOrNull()

    /** Add a verified dump alongside the other choices; never replace another imported BIOS. */
    private fun importBios(context: Context, source: InputStream): File? {
        val directory = com.armsx2.runtime.MainActivityRuntime.internalBiosDir(context).apply { mkdirs() }
        val staging = File.createTempFile(".bios-import-", ".tmp", directory)
        return try {
            staging.outputStream().use { output ->
                val buffer = ByteArray(8192)
                var bytes = 0L
                while (true) {
                    val count = source.read(buffer)
                    if (count < 0) break
                    bytes += count
                    if (bytes > Ps1BiosProbe.IMAGE_SIZE) return null
                    output.write(buffer, 0, count)
                }
                output.fd.sync()
            }
            if (staging.length() != Ps1BiosProbe.IMAGE_SIZE.toLong()) return null
            staging.inputStream().use { Ps1BiosProbe.probe(it) } ?: return null
            val digest = MessageDigest.getInstance("SHA-256").digest(staging.readBytes())
            val name = digest.joinToString("") { "%02x".format(it.toInt() and 0xff) }
            val target = File(directory, "imported-$name.bin")
            if (!target.exists()) {
                // No REPLACE_EXISTING: an import cannot overwrite a user's existing dump.
                java.nio.file.Files.move(staging.toPath(), target.toPath())
            } else if (target.length() != Ps1BiosProbe.IMAGE_SIZE.toLong() ||
                !MessageDigest.getInstance("SHA-256").digest(target.readBytes()).contentEquals(digest)) {
                return null
            }
            target
        } catch (_: Exception) {
            null
        } finally {
            staging.delete()
        }
    }

    /** Older forced imports become ordinary candidates; the source is never moved or erased. */
    fun preserveLegacyBios(context: Context, path: String) {
        val directory = com.armsx2.runtime.MainActivityRuntime.internalBiosDir(context)
        listOfNotNull(path.takeIf { it.isNotBlank() }?.let(::File), userBiosFile(context), bundledBiosFile(context))
            .distinct().filter { it.isFile && it.parentFile != directory }.forEach { file ->
                if (file.length() == Ps1BiosProbe.IMAGE_SIZE.toLong()) runCatching {
                    file.inputStream().use { importBios(context, it) }
                }
            }
    }

    /** Actual last-mounted SwanStation card. Old slot<N>.mcd files stay untouched. */
    fun memoryCardFile(context: Context, slot: Int): File {
        require(slot in 1..2)
        val directory = File(com.armsx2.runtime.MainActivityRuntime.assetCopyRoot(context), "swanstation/cards")
        val reported = runCatching { kr.co.iefriends.pcsx2.NativeApp.getSwanCardPath(slot) }.getOrNull()
        val remembered = runCatching { File(directory, ".last-card-$slot").readLines().firstOrNull() }.getOrNull()
        val target = (reported?.takeIf { it.isNotBlank() } ?: remembered)?.let(::File)
        if (target != null && runCatching { target.canonicalFile.parentFile == directory.canonicalFile }.getOrDefault(false)) return target
        return File(directory, "duckstation_shared_card_$slot.mcd")
    }
}
