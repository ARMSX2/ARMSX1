package com.armsx2

import com.armsx2.core.Ps1Covers
import java.io.File

/**
 * Serial for a disc image, resolved WITHOUT booting it.
 *
 * **PS1 rewrite.** This used to call `NativeApp.getGameTitle(path)`, which ran PCSX2's
 * `GameList::PopulateEntryFromPath` to get a serial *and* the boot-ELF CRC. Neither exists here:
 * the PCSX2 JNI is a stub, and the ARMSX core has no CRC concept (CRCs only ever mattered for
 * `<SERIAL>_<CRC>.pnach` patches, which the PS1 core does not implement). So identification is now
 * pure Kotlin — [com.armsx2.core.Ps1DiscId] reading the disc's `cdrom:\SLUS_005.94` boot line — and
 * [Id.crc] is always null.
 *
 * The API is unchanged so the Info tab, the library's long-press sheet and the emulation menu keep
 * compiling; the CRC rows they render simply come back empty.
 *
 * **Blocking — never call from the main thread.** Reads up to 16 MB off the front of the image.
 * Results are memoised per path for the process lifetime (in [Ps1Covers]); a disc image's identity
 * cannot change without the file changing.
 */
object DiscIdentity {
    data class Id(val serial: String?, val crc: String?)

    /** Cached lookup, or null if the image could not be identified. */
    fun of(path: String): Id? {
        if (path.isBlank()) return null
        val serial = probeSerial(path) ?: return null
        return Id(serial, null)
    }

    /** Always null on PS1 — see the class doc. Kept so PS2-era callers still compile. */
    fun crcOf(path: String): String? = of(path)?.crc

    /** Same, from a [GameInfo]'s uri. Derives the native path exactly as the launcher does
     *  (MainActivityRuntime's launchPath), so identity and boot always agree on the same file. */
    fun of(uri: android.net.Uri): Id? = of(nativePath(uri))

    fun crcOf(uri: android.net.Uri): String? = of(uri)?.crc

    /** Eight hex digits, as CRCs are rendered and as PNACH filenames spell them.
     *  Still referenced by callers that validate a CRC string. */
    val CRC_PATTERN = Regex("[0-9A-Fa-f]{8}")

    /**
     * The CRC for [game]. Always null in the PS1 build — there is no boot ELF to hash and no
     * consumer (patches don't exist), so the UI rows that show it render blank rather than a
     * fabricated value. Left suspend + same-signature so the three call sites are untouched.
     */
    @Suppress("UNUSED_PARAMETER", "RedundantSuspendModifier")
    suspend fun resolve(uri: android.net.Uri, serial: String?): String? = null

    /** Internal-use path the native side expects for [uri]. Also used by [com.armsx2.RaLibrary]
     *  to hash a disc for identification. */
    fun nativePath(uri: android.net.Uri): String =
        if (uri.scheme == "file") uri.path ?: uri.toString() else uri.toString()

    /** Disc serial via [Ps1Covers]' shared memo, so the library scan and this agree (and only one
     *  of them ever pays the read). Null for .chd/.zip/.exe and for anything not on disk. */
    private fun probeSerial(path: String): String? = runCatching {
        if (!File(path).isFile) return@runCatching null
        Ps1Covers.serialForPath(path)
    }.getOrNull()
}
