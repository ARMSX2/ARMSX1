package com.armsx2.core

import android.content.Context
import android.net.Uri
import android.system.Os
import java.io.File
import java.io.IOException

/**
 * SwanStation's raw 128 KiB cards under <dataRoot>/swanstation/cards.
 * Slot 1 is per-game (the actual mounted path is recorded after a completed run); Slot 2 is
 * shared. Old-core slot1.mcd/slot2.mcd files are not implicitly migrated or overwritten.
 *
 * The VM keeps card images in memory, so file edits must happen between sessions under
 * [sessionLock]. Imports replace one explicitly selected destination, retain a backup, and
 * use an atomic rename only after validating the complete input size. All I/O is blocking.
 */
object Ps1MemoryCards {
    val sessionLock = java.util.concurrent.locks.ReentrantLock()

    fun runSession(cancelled: () -> Boolean, run: () -> Boolean): Boolean {
        while (!sessionLock.tryLock(100, java.util.concurrent.TimeUnit.MILLISECONDS)) {
            if (cancelled()) return false
        }
        try {
            return if (cancelled()) false else run()
        } finally {
            sessionLock.unlock()
        }
    }

    /** Standard raw PS1 memory-card size (0x20000). */
    const val CARD_SIZE_BYTES: Long = 128L * 1024L

    /** The two emulated memory-card ports. */
    val SLOTS: List<Int> = listOf(1, 2)

    /** Human-readable slot name; not used to resolve the native card's filename. */
    fun fileName(slot: Int): String = "slot$slot.mcd"

    /** Resolve this game's actual mounted card, or the last mount when no game was selected. */
    fun cardFile(context: Context, slot: Int, gameSource: String? = null): File {
        require(slot in SLOTS)
        if (gameSource == null) return Ps1Config.memoryCardFile(context, slot)
        val root = cardDirectory(context)
        return Ps1CardBindings.resolve(root, gameSource, slot)
            ?: if (slot == 2) File(root, "duckstation_shared_card_2.mcd")
            else throw IOException("Start and close this game once to identify its Slot 1 card. No other game's card was selected.")
    }

    /** Called after a completed run, while the session lock still protects both cards. */
    fun rememberSession(context: Context, gameSource: String) {
        val cards = SLOTS.map { slot ->
            val path = kr.co.iefriends.pcsx2.NativeApp.getSwanCardPath(slot)
            check(path.isNotBlank()) { "The core did not report its memory card." }
            File(path)
        }
        Ps1CardBindings.remember(cardDirectory(context), gameSource, cards)
    }

    /** The native SwanStation save directory, independent of the currently selected game. */
    fun cardDirectory(context: Context): File = File(
        com.armsx2.runtime.MainActivityRuntime.assetCopyRoot(context), "swanstation/cards",
    )

    /** True when [slot] currently has a card file on disk. */
    fun exists(context: Context, slot: Int): Boolean = cardFile(context, slot).isFile

    /**
     * Write a formatted 128 KiB card for [slot], the same bytes `psx_mcd_init` would write itself.
     * Overwrites nothing: throws if a card is already there.
     */
    @Throws(IOException::class)
    fun createBlank(context: Context, slot: Int, gameSource: String? = null): File {
        val target = cardFile(context, slot, gameSource)
        if (target.exists()) throw IOException("${target.name} already exists.")
        writeBlank(target)
        return target
    }

    /** Create a formatted card, creating parent directories as needed. */
    @Throws(IOException::class)
    fun writeBlank(target: File) {
        target.parentFile?.mkdirs()
        target.outputStream().use { out ->
            out.write(Ps1MemoryCardImage.formatted())
            out.flush()
            out.fd.sync()
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
    fun delete(context: Context, slot: Int, gameSource: String? = null) {
        val target = cardFile(context, slot, gameSource)
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
    fun import(context: Context, slot: Int, uri: Uri, gameSource: String? = null): File {
        val target = cardFile(context, slot, gameSource)
        target.parentFile?.mkdirs()
        val staging = File(target.parentFile, "${target.name}.importing")
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                staging.outputStream().use { output ->
                    val buffer = ByteArray(8192)
                    var copied = 0L
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        copied += read
                        if (copied > CARD_SIZE_BYTES) throw IOException("A PS1 memory card must be exactly 128 KiB.")
                        output.write(buffer, 0, read)
                    }
                    output.fd.sync()
                }
            } ?: throw IOException("Could not open the selected file.")
            val size = staging.length()
            if (size != CARD_SIZE_BYTES) {
                throw IOException(
                    "That file is ${describeSize(size)}. A PS1 memory card is exactly 128 KiB " +
                        "($CARD_SIZE_BYTES bytes) of raw card data.",
                )
            }
            // Keep the old card recoverable before replacing it with the explicit import.
            if (target.isFile) {
                val backups = File(target.parentFile, "import-backups").apply { mkdirs() }
                val backup = File(backups, "${target.name}.${System.currentTimeMillis()}-${System.nanoTime()}.bak")
                target.copyTo(backup, overwrite = false)
            }
            Os.rename(staging.absolutePath, target.absolutePath)
            return target
        } finally {
            staging.delete()
        }
    }

    /** Copy [slot]'s card out to a user-chosen SAF destination. */
    @Throws(IOException::class)
    fun export(context: Context, slot: Int, uri: Uri, gameSource: String? = null) {
        val source = cardFile(context, slot, gameSource)
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
