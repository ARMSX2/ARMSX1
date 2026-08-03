package com.armsx2.core

import java.io.File
import java.io.RandomAccessFile
import java.util.Locale

/**
 * PS1 disc-serial extraction.
 *
 * Every PS1 game disc carries a `SYSTEM.CNF` in the ISO9660 ROOT DIRECTORY with a line like
 * `BOOT = cdrom:\SLUS_005.94;1`, naming the boot executable — and that name IS the disc serial.
 *
 * **This used to be a blind byte scan.** It read the first 16 MB of the image and regex'd it for
 * the literal text `cdrom:` followed by a serial. That works for most discs by luck: SYSTEM.CNF
 * usually sits a few dozen sectors in. It is not, however, where SYSTEM.CNF is *defined* to be —
 * ISO9660 puts the file wherever the mastering tool chose and records the location in the root
 * directory — so any disc that lays SYSTEM.CNF outside that arbitrary window, or spells its BOOT
 * line in a way the regex did not anticipate, came back with NO SERIAL AT ALL. A null serial is
 * not a cosmetic loss: cover art, RetroAchievements identification, play-time tracking, Discord
 * presence and the per-game settings key are all keyed on it.
 *
 * So identification now does what the core's own fast-boot does ([psx/fastboot.c]) and what the
 * BIOS does: find the volume descriptor, walk the root directory, read SYSTEM.CNF from the extent
 * the directory record points at, and parse its BOOT line. That is correct for any legal layout,
 * costs a handful of 2 KB reads instead of 16 MB per game, and reports WHY it failed when it does.
 * The old byte scan is kept as a last-ditch fallback for images with no readable filesystem.
 *
 * **Compressed containers go to the core.** A `.chd` cannot be read here at all: CHD v5 Huffman-
 * compresses its own hunk map, so there is no reaching the filesystem without decompressing it,
 * and every CHD in a library therefore used to come back with NO SERIAL — no cover, no per-game
 * settings key, no play-time record, just a placeholder tile. Writing a second CHD decoder in
 * Kotlin would have been the wrong fix twice over: a few hundred lines of bit-level decoding, and
 * a decoder that is a bit wrong returns a plausible WRONG serial rather than failing, which
 * silently attaches one game's art and settings to another. So the disc goes to the reader that
 * already boots it — [kr.co.iefriends.pcsx2.NativeApp.getDiscSerialForPath], over psx/discid.c,
 * through the same vtable the emulated drive reads through. One decoder, one answer.
 *
 * That does mean identification can now touch native. It costs nothing: `Pasx2Application`'s
 * warm-up thread already `System.loadLibrary`s SDL2 + libarmsx at process start (deliberately, to
 * keep the dlopen off the UI thread), and `MainActivity.onCreate` calls `NativeApp.initializeOnce`
 * regardless — so the library is up long before a game tile asks for its cover. Every call is
 * guarded anyway: with no native binary the probe degrades to exactly what it did before.
 *
 * The serial is normalised to psx-covers' filename form: `SLUS_005.94` → `SLUS-00594`. The native
 * reader normalises to the same shape, on purpose — the two are alternative routes to one
 * identity, and a disagreement would split a game's settings in half.
 */
object Ps1DiscId {

    /**
     * One identification attempt and the trace of how it went.
     *
     * [detail] exists because "no serial" used to be indistinguishable from "never looked": it is
     * written to the library's `serial_probe.log` so a disc that fails to identify says where it
     * got to — which layout was detected, where SYSTEM.CNF lives, what the BOOT line actually
     * said — instead of leaving a blank tile and no evidence.
     */
    data class Probe(
        val serial: String?,
        /** "iso9660", "rawscan", "pbp", "native", "cache", or "" when nothing produced a serial. */
        val method: String = "",
        val detail: String = "",
    )

    /** Fallback byte scan only. The ISO walk reads kilobytes, not megabytes. */
    private const val SCAN_BYTES = 32 * 1024 * 1024
    private const val CHUNK = 1 * 1024 * 1024
    private const val OVERLAP = 64

    private const val USER_BYTES = 2048          // ISO9660 logical sector
    private const val PVD_SECTOR = 16            // where the primary volume descriptor lives
    private const val VD_SEARCH_SECTORS = 64     // how far in to look for it
    private const val DR_ROOT = 156              // root directory record, from the PVD's user data
    private const val DR_EXTENT = 2
    private const val DR_SIZE = 10
    private const val DR_NAME_LEN = 32
    private const val DR_NAME = 33
    private const val MAX_ROOT_BYTES = 16 * 1024 * 1024

    /**
     * Sector geometries a PS1 rip is found in, as (bytes per sector, offset of the 2048 user
     * bytes). Order matters only in that the first one whose sector 16 holds a volume descriptor
     * wins; the probe below cannot false-positive across them because the `CD001` magic lands at a
     * different absolute offset in each.
     */
    private val LAYOUTS = listOf(
        2352 to 24,   // raw MODE2/2352 (form 1 or 2) — the usual cue+bin rip
        2352 to 16,   // raw MODE1/2352
        2048 to 0,    // plain ISO, or a cue's MODE1/2048
        2336 to 8,    // MODE2/2336
        2448 to 24,   // raw + 96 bytes of subchannel
        2448 to 16,
    )

    private val IMAGE_EXTS = setOf("bin", "iso", "img")

    /** `cdrom:\SLUS_005.94` in raw bytes. Accepts `cdrom0:`, either slash, and no separator. */
    private val BOOT_REGEX = Regex("""(?i)cdrom0?:\s*[\\/]{0,2}\s*([A-Za-z]{4})[_\-]?(\d{3})\.?(\d{2})""")

    /** A boot-executable name (`SLUS_006.64`, `SLUS_00664`, `slus-006.64`) → `SLUS-00664`. */
    private val EXE_NAME_REGEX = Regex("""^([A-Za-z]{4})[_\-.]?(\d{3})\.?(\d{2})""")


    /**
     * Serial of the FIRST disc inside a PSP EBOOT (.PBP). Layout (see psx/dev/cdrom/pbp.c, which
     * reads the same fields to actually mount it):
     *
     *   PBP  +0x24                     u32 offset of DATA.PSAR
     *   PSAR "PSTITLEIMG000000"        multi-disc: +0x200 holds u32 disc offsets (rel. PSAR)
     *        "PSISOIMG0000"            single-disc: the PSAR is the disc
     *   disc +0x400                    NUL-terminated ASCII serial, "_SCES_02380"
     *
     * Disc 1's serial is the right one to report: it is what the cover, the per-game settings key
     * and the achievements identity all hang off, exactly as for a multi-disc .m3u.
     */
    private fun probePbp(rom: File): Probe? = runCatching {
        java.io.RandomAccessFile(rom, "r").use { f ->
            fun u32(at: Long): Long {
                f.seek(at)
                val b = ByteArray(4)
                if (f.read(b) != 4) return -1
                return (b[0].toLong() and 0xff) or ((b[1].toLong() and 0xff) shl 8) or
                    ((b[2].toLong() and 0xff) shl 16) or ((b[3].toLong() and 0xff) shl 24)
            }

            val magic = ByteArray(4)
            f.seek(0)
            if (f.read(magic) != 4) return@runCatching null
            if (!(magic[0].toInt() == 0 && magic[1].toInt().toChar() == 'P' &&
                    magic[2].toInt().toChar() == 'B' && magic[3].toInt().toChar() == 'P')) {
                return@runCatching null
            }

            val psar = u32(0x24)
            if (psar <= 0) return@runCatching null

            val psarMagic = ByteArray(16)
            f.seek(psar)
            if (f.read(psarMagic) != 16) return@runCatching null
            val tag = String(psarMagic, Charsets.US_ASCII)

            val disc = when {
                tag.startsWith("PSISOIMG0000") -> psar
                tag.startsWith("PSTITLEIMG") -> {
                    val rel = u32(psar + 0x200)
                    if (rel <= 0) return@runCatching null
                    psar + rel
                }
                else -> return@runCatching null
            }

            val raw = ByteArray(15)
            f.seek(disc + 0x400)
            if (f.read(raw) != raw.size) return@runCatching null

            // "_SCES_02380" -> "SCES-02380". Stop at the first NUL.
            val text = String(raw, Charsets.US_ASCII).substringBefore('\u0000')
            val serial = text.trim('_', ' ').replace('_', '-').replace('.', '-').trim()
            val normalised = NORMALISE.find(serial)?.let { m ->
                "${m.groupValues[1].uppercase(Locale.US)}-${m.groupValues[2]}${m.groupValues[3]}"
            }
            if (normalised.isNullOrBlank()) return@runCatching null

            Probe(
                normalised, "pbp",
                "${rom.name}: PBP ${if (tag.startsWith("PSTITLEIMG")) "multi-disc" else "single-disc"}; " +
                    "disc serial @+0x400 = $text -> $normalised",
            )
        }
    }.getOrNull()

    /** `SCES-02380` / `SCES02380` / `SCES-023.80` in any case -> the three capture groups. */
    private val NORMALISE = Regex("""(?i)^([A-Za-z]{4})[-_.]?(\d{3})[-_.]?(\d{2})""")

    fun serialOf(rom: File): String? = probe(rom).serial

    /**
     * Identify [rom], recording how. Never throws: an unreadable disc is a [Probe] with a null
     * serial and a [Probe.detail] saying what happened.
     */
    fun probe(rom: File): Probe {
        // A PBP keeps the serial in clear ASCII at disc+0x400, so it needs no decompression and
        // no ISO9660 walk — which is just as well, since the disc inside is deflate-compressed
        // in 16-sector blocks and the generic reader below cannot see into it at all.
        if (rom.extension.lowercase(Locale.US) == "pbp") {
            probePbp(rom)?.let { return it }
            return withNativeFallback(rom, "${rom.name}: PBP container carried no readable disc serial")
        }

        val candidates = runCatching { dataCandidates(rom) }.getOrDefault(emptyList())
        if (candidates.isEmpty()) {
            // .chd (and .zip): nothing here can see inside a compressed container. This is the
            // common path for a CHD library, not an edge case — the native reader below is what
            // identifies it.
            return withNativeFallback(
                rom,
                "${rom.name}: no readable data track " +
                    "(compressed container, or the cue names a file that is not there)",
            )
        }
        val trace = StringBuilder()
        for (data in candidates) {
            val hit = probeFile(data, trace)
            if (hit != null) return Probe(hit.first, hit.second, trace.toString().trimEnd())
        }
        // The Kotlin walk and its byte scan both came up empty. Before settling for a blank tile,
        // ask the core's reader — it opens layouts this cannot (and this is free: a disc that
        // identified above never reaches here).
        return withNativeFallback(rom, trace.toString().trimEnd())
    }

    // ---- native reader (compressed containers, and last resort) ------------------------------

    /**
     * Hand [rom] to the core's own disc reader and take whatever it says, keeping [kotlinDetail]
     * in the trace so `serial_probe.log` still records how the Kotlin attempt went.
     *
     * Never throws and never blocks on anything but the read. With no native binary loaded — the
     * JVM unit tests, or a build whose `.so` failed to load — this degrades to the [Probe] the
     * caller would have returned anyway, and says so rather than leaving "no cover" and no
     * evidence.
     */
    private fun withNativeFallback(rom: File, kotlinDetail: String): Probe {
        val detail = StringBuilder(kotlinDetail.trimEnd())
        if (detail.isNotEmpty()) detail.append("; ")

        val result = runCatching {
            kr.co.iefriends.pcsx2.NativeApp.getDiscSerialForPath(rom.absolutePath)
        }
        val serial = result.getOrNull()?.trim()?.takeIf { it.isNotBlank() }

        return when {
            serial != null -> {
                detail.append("core disc reader -> ").append(serial)
                Probe(serial, "native", detail.toString())
            }
            result.isFailure -> {
                // UnsatisfiedLinkError, i.e. no libarmsx in this process. Worth naming: it is the
                // difference between "this disc has no serial" and "nothing ever looked".
                detail.append("core disc reader unavailable (")
                    .append(result.exceptionOrNull()?.javaClass?.simpleName ?: "unknown")
                    .append(')')
                Probe(null, "", detail.toString())
            }
            else -> {
                detail.append("core disc reader found no serial")
                Probe(null, "", detail.toString())
            }
        }
    }

    // ---- data-track resolution ------------------------------------------------------------

    /**
     * The file(s) that could hold the data track, best first.
     *
     * A `.cue` is text pointing at the real image, so it must be followed — scanning the cue
     * itself finds nothing but its own filenames. The old code followed only the FIRST `FILE`
     * line and, when that file was missing, silently fell back to scanning the cue, which
     * guarantees a null serial and leaves no trace of why.
     */
    private fun dataCandidates(rom: File): List<File> {
        val ext = rom.extension.lowercase(Locale.US)
        return when {
            ext == "cue" -> cueTargets(rom)
            ext in IMAGE_EXTS -> listOf(rom)
            else -> emptyList()   // chd/zip/exe: needs the core to open
        }
    }

    private val CUE_FILE_LINE = Regex("""(?i)^\s*FILE\s+(?:"([^"]+)"|(\S+))""")

    /** Absolute paths of every `FILE "…"` line in [cue], resolved next to the cue itself.
     *  Shared with [Ps1Library], which uses it to collapse a cue+bin rip to its single entry. */
    fun cueReferencedPaths(cue: File): List<String> = try {
        cue.useLines { lines ->
            lines.take(512).mapNotNull { line ->
                val m = CUE_FILE_LINE.find(line) ?: return@mapNotNull null
                val name = m.groupValues[1].ifEmpty { m.groupValues[2] }.trim('"').trim()
                name.takeIf { it.isNotEmpty() }?.let { File(cue.parentFile, it).absolutePath }
            }.toList()
        }
    } catch (_: Exception) {
        emptyList()
    }

    private fun cueTargets(cue: File): List<File> {
        val siblings = HashMap<String, File>()
        runCatching { cue.parentFile?.listFiles() }.getOrNull()?.forEach { f ->
            if (f.isFile) siblings.putIfAbsent(f.name.lowercase(Locale.US), f)
        }
        val out = LinkedHashSet<File>()
        // Every FILE the cue names. Resolved case-insensitively as well as literally: a rip that
        // travelled through a case-preserving filesystem routinely disagrees with its own cue on
        // the case of the .bin, and on Android that is a hard miss.
        cueReferencedPaths(cue).forEach { p ->
            val named = File(p)
            val hit = if (named.isFile) named else siblings[named.name.lowercase(Locale.US)]
            if (hit != null && hit.isFile) out += hit
        }
        // Cue naming something that simply is not there (renamed dump): try any sibling image
        // sharing the cue's own stem before giving up.
        if (out.isEmpty()) {
            val stem = cue.nameWithoutExtension.lowercase(Locale.US)
            siblings.values.forEach { f ->
                if (f.extension.lowercase(Locale.US) in IMAGE_EXTS &&
                    f.nameWithoutExtension.lowercase(Locale.US) == stem
                ) out += f
            }
        }
        return out.toList()
    }

    // ---- ISO9660 ---------------------------------------------------------------------------

    /** Sector geometry plus the file sector that ISO sector 0 sits at (non-zero if the image
     *  carries a pregap ahead of the filesystem). */
    private data class Layout(val sectorBytes: Int, val userOffset: Int, val isoBase: Int)

    private fun probeFile(data: File, trace: StringBuilder): Pair<String, String>? {
        val length = runCatching { data.length() }.getOrDefault(0L)
        trace.append(data.name).append(" [").append(length).append(" bytes]")
        try {
            RandomAccessFile(data, "r").use { raf ->
                val buf = ByteArray(USER_BYTES)
                val layout = detectLayout(raf, length, buf)
                if (layout == null) {
                    trace.append(": no ISO9660 volume descriptor in the first ")
                        .append(VD_SEARCH_SECTORS).append(" sectors")
                } else {
                    trace.append(": ISO9660 sector=").append(layout.sectorBytes)
                        .append("+").append(layout.userOffset)
                    if (layout.isoBase != 0) trace.append(" base=").append(layout.isoBase)
                    val serial = serialFromFilesystem(raf, length, layout, buf, trace)
                    if (serial != null) {
                        trace.append('\n')
                        return serial to "iso9660"
                    }
                }
                // Last resort: the old byte scan. Only reached when the image has no filesystem we
                // can walk, so the megabytes it reads are no longer on the common path.
                val raw = rawScan(raf, length)
                trace.append(if (raw != null) "; raw scan found $raw" else "; raw scan found nothing")
                trace.append('\n')
                if (raw != null) return raw to "rawscan"
            }
        } catch (e: Exception) {
            trace.append(": read failed (").append(e.javaClass.simpleName)
                .append(": ").append(e.message).append(")\n")
        }
        return null
    }

    /** First geometry whose leading sectors contain a primary volume descriptor. */
    private fun detectLayout(raf: RandomAccessFile, length: Long, buf: ByteArray): Layout? {
        for ((sectorBytes, userOffset) in LAYOUTS) {
            for (sector in 0 until VD_SEARCH_SECTORS) {
                val at = sector.toLong() * sectorBytes + userOffset
                if (at + USER_BYTES > length) break
                if (!readAt(raf, at, buf)) break
                // Type 1 + "CD001" is the primary volume descriptor. Requiring both is what stops
                // a non-ISO image from being walked as if its bytes were directory records.
                if (buf[0].toInt() == 1 && matchesAscii(buf, 1, "CD001")) {
                    return Layout(sectorBytes, userOffset, sector - PVD_SECTOR)
                }
            }
        }
        return null
    }

    /**
     * Walk the root directory for SYSTEM.CNF, read it, and pull the serial out of its BOOT line.
     * Appends what it found (or how far it got) to [trace].
     */
    private fun serialFromFilesystem(
        raf: RandomAccessFile,
        length: Long,
        layout: Layout,
        buf: ByteArray,
        trace: StringBuilder,
    ): String? {
        if (!readIso(raf, length, layout, PVD_SECTOR.toLong(), buf)) {
            trace.append("; volume descriptor unreadable")
            return null
        }
        val rootExtent = le32(buf, DR_ROOT + DR_EXTENT)
        val rootSize = le32(buf, DR_ROOT + DR_SIZE)
        trace.append(" root=").append(rootExtent).append("/").append(rootSize)
        if (rootSize <= 0L || rootSize > MAX_ROOT_BYTES) {
            trace.append("; root directory size out of range")
            return null
        }

        val found = findInRoot(raf, length, layout, rootExtent, rootSize, "SYSTEM.CNF", buf)
        if (found == null) {
            trace.append("; SYSTEM.CNF not in the root directory")
            return null
        }
        val (extent, size) = found
        // The byte offset the OLD scanner would have had to reach. Printed because it is the
        // whole difference between "scans fine" and "no serial" for a blind window scan.
        val byteOffset = (extent + layout.isoBase).toLong() * layout.sectorBytes + layout.userOffset
        trace.append("; SYSTEM.CNF extent=").append(extent).append(" size=").append(size)
            .append(" @").append(byteOffset).append("B")

        if (!readIso(raf, length, layout, extent, buf)) {
            trace.append("; SYSTEM.CNF unreadable")
            return null
        }
        val usable = if (size in 1..USER_BYTES.toLong()) size.toInt() else USER_BYTES
        val text = String(buf, 0, usable, Charsets.ISO_8859_1)
        val bootName = parseBootName(text)
        if (bootName == null) {
            trace.append("; no BOOT line (")
                .append(text.take(60).replace('\n', ' ').replace('\r', ' ').trim())
                .append(")")
            return null
        }
        trace.append("; BOOT=").append(bootName)
        val serial = serialFromExeName(bootName)
        if (serial == null) {
            // A real thing on homebrew and a few licensed discs: the boot executable is named
            // PSX.EXE / MAIN.EXE rather than after the serial. Nothing to key on, but say so.
            trace.append(" (not a serial-shaped name)")
            return null
        }
        trace.append(" -> ").append(serial)
        return serial
    }

    /** Directory-record search, case-insensitive and ignoring ISO9660's `;1` version suffix. */
    private fun findInRoot(
        raf: RandomAccessFile,
        length: Long,
        layout: Layout,
        extent: Long,
        size: Long,
        wanted: String,
        buf: ByteArray,
    ): Pair<Long, Long>? {
        val sectors = ((size + USER_BYTES - 1) / USER_BYTES).toInt()
        for (i in 0 until sectors) {
            if (!readIso(raf, length, layout, extent + i, buf)) return null
            var offset = 0
            while (offset < USER_BYTES) {
                val recordLength = buf[offset].toInt() and 0xFF
                // Zero length means the rest of this sector is padding; the next record starts at
                // the next sector boundary.
                if (recordLength == 0) break
                if (offset + recordLength > USER_BYTES) break
                val nameLen = buf[offset + DR_NAME_LEN].toInt() and 0xFF
                if (nameLen > 0 && DR_NAME + nameLen <= recordLength &&
                    nameMatches(buf, offset + DR_NAME, nameLen, wanted)
                ) {
                    val fileExtent = le32(buf, offset + DR_EXTENT)
                    val fileSize = le32(buf, offset + DR_SIZE)
                    if (fileSize > 0L) return fileExtent to fileSize
                }
                offset += recordLength
            }
        }
        return null
    }

    private fun nameMatches(buf: ByteArray, at: Int, len: Int, wanted: String): Boolean {
        var i = 0
        while (i < len) {
            val c = (buf[at + i].toInt() and 0xFF).toChar()
            if (c == ';') break
            if (i >= wanted.length) return false
            if (c.lowercaseChar() != wanted[i].lowercaseChar()) return false
            i++
        }
        return i == wanted.length
    }

    /**
     * The executable named by SYSTEM.CNF's BOOT line, or null.
     *
     * Tolerant of every spelling real discs use, exactly as the core's fast-boot is: `BOOT=` with
     * or without spaces, lower-case `boot`, `cdrom:` or `cdrom0:` or neither, forward or back
     * slashes, a leading slash or none, and a missing `;1`.
     */
    private fun parseBootName(text: String): String? {
        var i = 0
        while (i + 4 <= text.length) {
            if (!text.regionMatches(i, "BOOT", 0, 4, ignoreCase = true)) { i++; continue }
            var p = i + 4
            while (p < text.length && (text[p] == ' ' || text[p] == '\t')) p++
            // "BOOT2" (a PS2 spelling) and any other BOOT-prefixed key fall out here rather than
            // being read as the boot line.
            if (p >= text.length || text[p] != '=') { i++; continue }
            p++
            while (p < text.length && (text[p] == ' ' || text[p] == '\t')) p++
            if (text.regionMatches(p, "cdrom0:", 0, 7, ignoreCase = true)) p += 7
            else if (text.regionMatches(p, "cdrom:", 0, 6, ignoreCase = true)) p += 6
            while (p < text.length && (text[p] == '\\' || text[p] == '/')) p++
            val start = p
            while (p < text.length) {
                val c = text[p]
                if (c == ';' || c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '\u0000') break
                p++
            }
            if (p > start) return text.substring(start, p)
            i++
        }
        return null
    }

    /** `SLUS_006.64` → `SLUS-00664`. Null for a boot name that is not serial-shaped. */
    private fun serialFromExeName(name: String): String? {
        val bare = name.substringAfterLast('\\').substringAfterLast('/')
        val m = EXE_NAME_REGEX.find(bare) ?: return null
        return "${m.groupValues[1].uppercase(Locale.US)}-${m.groupValues[2]}${m.groupValues[3]}"
    }

    // ---- raw byte scan (fallback) ------------------------------------------------------------

    private fun rawScan(raf: RandomAccessFile, length: Long): String? {
        val limit = minOf(length, SCAN_BYTES.toLong())
        var pos = 0L
        val buf = ByteArray(CHUNK + OVERLAP)
        var carry = ByteArray(0)
        while (pos < limit) {
            raf.seek(pos)
            val toRead = minOf(CHUNK.toLong(), limit - pos).toInt()
            val read = raf.read(buf, 0, toRead)
            if (read <= 0) break
            // Prepend carry-over so a token spanning the boundary is still found.
            val window = if (carry.isEmpty()) String(buf, 0, read, Charsets.ISO_8859_1)
            else String(carry + buf.copyOf(read), Charsets.ISO_8859_1)
            BOOT_REGEX.find(window)?.let { m ->
                return "${m.groupValues[1].uppercase(Locale.US)}-${m.groupValues[2]}${m.groupValues[3]}"
            }
            carry = buf.copyOfRange(maxOf(0, read - OVERLAP), read)
            pos += read
        }
        return null
    }

    // ---- byte helpers ------------------------------------------------------------------------

    /** Reads one ISO logical sector's 2048 user bytes. */
    private fun readIso(
        raf: RandomAccessFile,
        length: Long,
        layout: Layout,
        isoSector: Long,
        out: ByteArray,
    ): Boolean {
        val fileSector = isoSector + layout.isoBase
        if (fileSector < 0) return false
        val at = fileSector * layout.sectorBytes + layout.userOffset
        if (at < 0 || at + USER_BYTES > length) return false
        return readAt(raf, at, out)
    }

    private fun readAt(raf: RandomAccessFile, offset: Long, out: ByteArray): Boolean = try {
        raf.seek(offset)
        raf.readFully(out)
        true
    } catch (_: Exception) {
        false
    }

    private fun le32(buf: ByteArray, at: Int): Long =
        (buf[at].toLong() and 0xFF) or
            ((buf[at + 1].toLong() and 0xFF) shl 8) or
            ((buf[at + 2].toLong() and 0xFF) shl 16) or
            ((buf[at + 3].toLong() and 0xFF) shl 24)

    private fun matchesAscii(buf: ByteArray, at: Int, text: String): Boolean {
        for (i in text.indices) if ((buf[at + i].toInt() and 0xFF).toChar() != text[i]) return false
        return true
    }
}
