package com.armsx2.core

import android.content.Context
import android.net.Uri
import java.io.File
import java.io.IOException

/**
 * PS1 memory-card files, exactly as the ARMSX1 core expects them.
 *
 * The core does not expose a card manager: `frontend/main.cpp` unconditionally attaches two cards
 * right after creating the pad —
 *
 * ```c
 * const std::string slot1 = std::string(psxe_cfg_get_pref_path()) + "slot1.mcd";
 * const std::string slot2 = std::string(psxe_cfg_get_pref_path()) + "slot2.mcd";
 * psx_pad_attach_mcd(psx_->pad, 0, slot1.c_str());
 * psx_pad_attach_mcd(psx_->pad, 1, slot2.c_str());
 * ```
 *
 * so the whole feature is two fixed files in one fixed directory:
 *
 *  * **Path** — the pref path. `frontend/android_jni.cpp` calls `psxe_cfg_set_pref_path()` with the
 *    app files dir before every run, and SDL's Android `SDL_GetPrefPath` resolves to the same place,
 *    so it is [Context.getFilesDir] either way — the directory `settings.toml` lives in. See
 *    [Ps1Config.memoryCardFile].
 *  * **Name** — `slot1.mcd` and `slot2.mcd`. Nothing else is read; a differently named `.mcd`
 *    sitting next to them is invisible to the core.
 *  * **Size** — always [CARD_SIZE_BYTES] (`MCD_MEMORY_SIZE`, 0x20000 = 128 KiB) raw bytes. PS1 cards
 *    have no size options, no folder variant and no container header.
 *
 * `psx_mcd_init()` (psx/dev/mcd.c) creates a zero-filled 128 KiB file when the path is missing, and
 * `psx_mcd_destroy()` writes the whole 128 KiB buffer back on shutdown. Two consequences this
 * object's callers must respect:
 *
 *  1. "Creating" a card is nothing more than producing a 128 KiB zero-filled file — identical to
 *     what the core would produce on its own. A fresh card reads as unformatted until the BIOS card
 *     manager (or the first game to save) formats it.
 *  2. Because the buffer is flushed on shutdown, edits made **while a game is running** are undone
 *     when that game exits. Card edits belong between sessions.
 *
 * All functions here do blocking file I/O — call them off the main thread.
 */
object Ps1MemoryCards {

    /** `MCD_MEMORY_SIZE` from psx/dev/mcd.h — 0x20000. The only valid PS1 card size. */
    const val CARD_SIZE_BYTES: Long = 128L * 1024L

    /** The two slots `main.cpp` attaches, in core order (slot 1 = pad port 0). */
    val SLOTS: List<Int> = listOf(1, 2)

    /** `slot1.mcd` / `slot2.mcd` — the literal names the core concatenates onto the pref path. */
    fun fileName(slot: Int): String = "slot$slot.mcd"

    /** Absolute location of [slot]'s card: `<filesDir>/slot<N>.mcd`. */
    fun cardFile(context: Context, slot: Int): File = Ps1Config.memoryCardFile(context, slot)

    /** The directory both cards live in (the native pref path). */
    fun cardDirectory(context: Context): File = context.filesDir

    /** True when [slot] currently has a card file on disk. */
    fun exists(context: Context, slot: Int): Boolean = cardFile(context, slot).isFile

    /**
     * Write a blank 128 KiB card for [slot], the same bytes `psx_mcd_init` would write itself.
     * Overwrites nothing: throws if a card is already there.
     */
    @Throws(IOException::class)
    fun createBlank(context: Context, slot: Int): File {
        val target = cardFile(context, slot)
        if (target.exists()) throw IOException("${target.name} already exists.")
        writeBlank(target)
        return target
    }

    /** Zero-fill [target] to exactly [CARD_SIZE_BYTES], creating parent directories as needed. */
    @Throws(IOException::class)
    fun writeBlank(target: File) {
        target.parentFile?.mkdirs()
        val chunk = ByteArray(16 * 1024) // already zero-filled by the JVM
        target.outputStream().use { out ->
            var written = 0L
            while (written < CARD_SIZE_BYTES) {
                val n = minOf(chunk.size.toLong(), CARD_SIZE_BYTES - written).toInt()
                out.write(chunk, 0, n)
                written += n
            }
            out.flush()
        }
        if (target.length() != CARD_SIZE_BYTES) {
            target.delete()
            throw IOException("Could not write a full 128 KiB card.")
        }
    }

    /**
     * Delete [slot]'s card. The core recreates a blank one on the next boot, so this doubles as
     * "format": there is no in-place format entry point in the core to call instead.
     */
    @Throws(IOException::class)
    fun delete(context: Context, slot: Int) {
        val target = cardFile(context, slot)
        if (target.exists() && !target.delete()) throw IOException("Could not delete ${target.name}.")
    }

    /**
     * Copy a user-picked `.mcd` over [slot]'s card.
     *
     * Written to a sibling temp file first and only swapped in once the copy is known to be a valid
     * 128 KiB image, so a bad pick (a save state, a `.gme`/DexDrive dump with its 3904-byte header,
     * a truncated download) can never leave a half-written card where the core expects a good one.
     */
    @Throws(IOException::class)
    fun import(context: Context, slot: Int, uri: Uri): File {
        val target = cardFile(context, slot)
        target.parentFile?.mkdirs()
        val staging = File(target.parentFile, "${target.name}.importing")
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                staging.outputStream().use { output -> input.copyTo(output) }
            } ?: throw IOException("Could not open the selected file.")
            val size = staging.length()
            if (size != CARD_SIZE_BYTES) {
                throw IOException(
                    "That file is ${describeSize(size)}. A PS1 memory card is exactly 128 KiB " +
                        "($CARD_SIZE_BYTES bytes) of raw card data.",
                )
            }
            if (target.exists() && !target.delete()) throw IOException("Could not replace ${target.name}.")
            if (!staging.renameTo(target)) throw IOException("Could not install ${target.name}.")
            return target
        } finally {
            staging.delete()
        }
    }

    /** Copy [slot]'s card out to a user-chosen SAF destination. */
    @Throws(IOException::class)
    fun export(context: Context, slot: Int, uri: Uri) {
        val source = cardFile(context, slot)
        if (!source.isFile) throw IOException("There is no card in slot $slot to export.")
        context.contentResolver.openOutputStream(uri)?.use { output ->
            source.inputStream().use { input -> input.copyTo(output) }
        } ?: throw IOException("Could not open the export destination.")
    }

    /** Human-readable byte count, KiB-oriented because every valid card is 128 KiB. */
    fun describeSize(bytes: Long): String = when {
        bytes <= 0L -> "0 bytes"
        bytes < 1024L -> "$bytes bytes"
        bytes < 1024L * 1024L -> "${bytes / 1024L} KiB"
        else -> "%.1f MiB".format(bytes / (1024f * 1024f))
    }
}
