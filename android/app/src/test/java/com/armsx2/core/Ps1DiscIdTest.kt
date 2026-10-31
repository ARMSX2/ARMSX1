package com.armsx2.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.RandomAccessFile

/**
 * Disc identification ([Ps1DiscId]) against synthetic PS1 images.
 *
 * Runs on the build machine, on purpose: the interesting cases are disc LAYOUTS, and a layout is
 * a handful of bytes at known offsets — reproducible here, and otherwise unverifiable until a user
 * reports a game with no cover art. The case this file was written for is [systemCnfBeyondTheOldScanWindow]:
 * identification used to be a blind regex over the first 16 MB of the image, so a disc that put
 * SYSTEM.CNF outside that window came back with NO SERIAL and lost its cover, its
 * RetroAchievements identity and its per-game settings key, with nothing anywhere saying why.
 * ISO9660 does not promise the file is near the front — it records where the file is, and that
 * record is what identification reads now.
 */
class Ps1DiscIdTest {

    @get:Rule
    val temp = TemporaryFolder()

    // ---- the regression this was written for -------------------------------------------------

    @Test
    fun systemCnfBeyondTheOldScanWindow() {
        // 7500 * 2352 = 17.6 MB in, i.e. past the 16 MB the old byte scan could see.
        val cnfSector = 7500
        val image = writeImage(
            temp.newFile("deep.bin"),
            sectorBytes = 2352, userOffset = 24,
            systemCnfSector = cnfSector,
            bootLine = "BOOT = cdrom:\\SLUS_006.64;1\r\n",
        )
        assertTrue(
            "the fixture must actually be past the old window",
            cnfSector.toLong() * 2352 > 16L * 1024 * 1024,
        )
        val probe = Ps1DiscId.probe(image)
        assertEquals("SLUS-00664", probe.serial)
        assertEquals("iso9660", probe.method)
        // The trace has to carry the byte offset, or a future failure is undiagnosable again.
        assertTrue(probe.detail, probe.detail.contains("SYSTEM.CNF extent=$cnfSector"))
    }

    // ---- layouts -----------------------------------------------------------------------------

    @Test
    fun rawMode2Image() {
        val image = writeImage(
            temp.newFile("mode2.bin"), 2352, 24, 24, "BOOT = cdrom:\\SLUS_005.94;1\r\n",
        )
        assertEquals("SLUS-00594", Ps1DiscId.serialOf(image))
    }

    @Test
    fun rawMode1Image() {
        val image = writeImage(
            temp.newFile("mode1.bin"), 2352, 16, 24, "BOOT = cdrom:\\SLUS_007.07;1\r\n",
        )
        assertEquals("SLUS-00707", Ps1DiscId.serialOf(image))
    }

    @Test
    fun plainIsoImage() {
        val image = writeImage(
            temp.newFile("plain.iso"), 2048, 0, 24, "BOOT = cdrom:\\SLUS_000.67;1\r\n",
        )
        assertEquals("SLUS-00067", Ps1DiscId.serialOf(image))
    }

    @Test
    fun imageWithAPregapAheadOfTheFilesystem() {
        // The volume descriptor is not at sector 16 of the FILE when the rip carries a pregap.
        val image = writeImage(
            temp.newFile("pregap.bin"), 2352, 24, 40, "BOOT = cdrom:\\SLUS_006.69;1\r\n",
            isoBase = 12,
        )
        assertEquals("SLUS-00669", Ps1DiscId.serialOf(image))
    }

    // ---- BOOT line spellings -------------------------------------------------------------------

    @Test
    fun bootLineSpellings() {
        val spellings = mapOf(
            "BOOT=cdrom:\\SLUS_006.64;1\n" to "SLUS-00664",           // no spaces
            "boot = cdrom:SLUS_006.64;1\n" to "SLUS-00664",           // lower case, no slash
            "BOOT\t=\tcdrom0:\\SLUS_006.64;1\n" to "SLUS-00664",      // tabs, cdrom0:
            "BOOT = cdrom:\\\\SLUS_006.64;1\n" to "SLUS-00664",       // doubled slash
            "BOOT = cdrom:/SLUS_006.64;1\n" to "SLUS-00664",          // forward slash
            "BOOT = cdrom:\\SLUS_006.64\n" to "SLUS-00664",           // no ;1
            "TCB = 4\nEVENT = 10\nBOOT = cdrom:\\SCUS_941.63;1\n" to "SCUS-94163", // not first line
        )
        spellings.forEach { (line, expected) ->
            val image = writeImage(temp.newFile("boot${line.hashCode()}.bin"), 2352, 24, 24, line)
            assertEquals(line, expected, Ps1DiscId.serialOf(image))
        }
    }

    @Test
    fun bootNameThatIsNotASerialIsReportedRatherThanGuessed() {
        val image = writeImage(
            temp.newFile("homebrew.bin"), 2352, 24, 24, "BOOT = cdrom:\\PSX.EXE;1\r\n",
        )
        val probe = Ps1DiscId.probe(image)
        assertNull(probe.serial)
        assertTrue(probe.detail, probe.detail.contains("BOOT=PSX.EXE"))
    }

    @Test
    fun discWithNoSystemCnfSaysSo() {
        val image = writeImage(
            temp.newFile("nocnf.bin"), 2352, 24, 24, "BOOT = cdrom:\\SLUS_006.64;1\r\n",
            includeSystemCnf = false,
        )
        val probe = Ps1DiscId.probe(image)
        assertNull(probe.serial)
        assertTrue(probe.detail, probe.detail.contains("SYSTEM.CNF not in the root directory"))
    }

    // ---- cue resolution ------------------------------------------------------------------------

    @Test
    fun cueIsFollowedToItsBin() {
        val bin = writeImage(
            temp.newFile("Xenogears (USA) (Disc 1).bin"), 2352, 24, 24,
            "BOOT = cdrom:\\SLUS_006.64;1\r\n",
        )
        val cue = File(bin.parentFile, "Xenogears (USA) (Disc 1).cue")
        cue.writeText("FILE \"Xenogears (USA) (Disc 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n")
        assertEquals("SLUS-00664", Ps1DiscId.serialOf(cue))
    }

    @Test
    fun cueWhoseFileLineDisagreesOnCase() {
        val bin = writeImage(
            temp.newFile("Game (USA).bin"), 2352, 24, 24, "BOOT = cdrom:\\SLUS_006.64;1\r\n",
        )
        val cue = File(bin.parentFile, "Game (USA).cue")
        cue.writeText("FILE \"GAME (USA).BIN\" BINARY\n  TRACK 01 MODE2/2352\n")
        assertEquals("SLUS-00664", Ps1DiscId.serialOf(cue))
    }

    @Test
    fun cueNamingAMissingFileFallsBackToItsOwnStem() {
        val bin = writeImage(
            temp.newFile("Renamed.bin"), 2352, 24, 24, "BOOT = cdrom:\\SLUS_006.69;1\r\n",
        )
        val cue = File(bin.parentFile, "Renamed.cue")
        cue.writeText("FILE \"Original Dump Name.bin\" BINARY\n  TRACK 01 MODE2/2352\n")
        assertEquals("SLUS-00669", Ps1DiscId.serialOf(cue))
    }

    @Test
    fun audioFirstCueFallsThroughToTheDataTrack() {
        val dir = temp.newFolder("multitrack")
        File(dir, "track01.bin").writeBytes(ByteArray(4 * 2352))   // audio: no filesystem
        val data = writeImage(File(dir, "track02.bin"), 2352, 24, 24, "BOOT = cdrom:\\SLUS_010.40;1\r\n")
        val cue = File(dir, "game.cue")
        cue.writeText(
            "FILE \"track01.bin\" BINARY\n  TRACK 01 AUDIO\n" +
                "FILE \"${data.name}\" BINARY\n  TRACK 02 MODE2/2352\n",
        )
        assertEquals("SLUS-01040", Ps1DiscId.serialOf(cue))
    }

    // ---- fallback --------------------------------------------------------------------------------

    @Test
    fun rawScanStillCatchesAnImageWithNoWalkableFilesystem() {
        // No volume descriptor anywhere — only the boot text, as a damaged or unusual rip.
        val file = temp.newFile("broken.bin")
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(4L * 1024 * 1024)
            raf.seek(200_000)
            raf.write("BOOT = cdrom:\\SLUS_009.58;1".toByteArray(Charsets.ISO_8859_1))
        }
        val probe = Ps1DiscId.probe(file)
        assertEquals("SLUS-00958", probe.serial)
        assertEquals("rawscan", probe.method)
    }

    /**
     * A `.chd` is handed to the core's disc reader — nothing in Kotlin can see inside a
     * compressed container, and that is exactly why every CHD used to come back with no serial
     * and no cover.
     *
     * On this JVM there IS no core: `System.loadLibrary("armsx")` cannot resolve on the build
     * machine. So what this pins is the DEGRADATION — a probe that reaches the native reader and
     * cannot use it must come back with a null serial and a trace saying so, never a guess and
     * never a crash. The identification itself is gated host-side against real disc geometry in
     * `tests/disc_serial.c` (`make test-disc-serial`), where the reader actually exists.
     */
    @Test
    fun compressedContainersGoToTheCoreAndDegradeCleanlyWithoutIt() {
        val chd = temp.newFile("game.chd")
        chd.writeBytes(ByteArray(1024))
        val probe = Ps1DiscId.probe(chd)
        assertNull(probe.serial)
        assertTrue(probe.detail, probe.detail.contains("no readable data track"))
        assertTrue(probe.detail, probe.detail.contains("core disc reader"))
    }

    // ---- dump-name fallback (cover art only) -------------------------------------------------

    @Test
    fun titleFallbackResolvesEachXenogearsDisc() {
        assertEquals("SLUS-00664", Ps1TitleSerials.coverSerialFor("Xenogears", "Xenogears (USA) (Disc 1).chd"))
        assertEquals("SLUS-00669", Ps1TitleSerials.coverSerialFor("Xenogears", "Xenogears (USA) (Disc 2).chd"))
    }

    @Test
    fun titleFallbackLeavesOtherRegionsAlone() {
        // The table is USA serials; a European pressing has a different one, and different art.
        assertNull(Ps1TitleSerials.coverSerialFor("Xenogears", "Xenogears (Europe) (Disc 1).chd"))
        assertNull(Ps1TitleSerials.coverSerialFor("Some Unlisted Game", "Some Unlisted Game (USA).chd"))
    }

    // ---- fixtures ------------------------------------------------------------------------------

    /**
     * Writes a minimal but structurally real ISO9660 image: a primary volume descriptor at ISO
     * sector 16, a root directory holding `.`, `..` and `SYSTEM.CNF;1`, and the SYSTEM.CNF text at
     * [systemCnfSector]. Sparse — only the three sectors that matter are written.
     */
    private fun writeImage(
        file: File,
        sectorBytes: Int,
        userOffset: Int,
        systemCnfSector: Int,
        bootLine: String,
        rootSector: Int = 22,
        isoBase: Int = 0,
        includeSystemCnf: Boolean = true,
    ): File {
        val cnf = bootLine.toByteArray(Charsets.ISO_8859_1)
        val lastSector = maxOf(systemCnfSector, rootSector, 16) + isoBase + 2
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(lastSector.toLong() * sectorBytes)
            fun put(isoSector: Int, data: ByteArray) {
                raf.seek((isoSector + isoBase).toLong() * sectorBytes + userOffset)
                raf.write(data)
            }

            val pvd = ByteArray(2048)
            pvd[0] = 1
            "CD001".forEachIndexed { i, c -> pvd[1 + i] = c.code.toByte() }
            pvd[6] = 1
            // Root directory record at offset 156 of the PVD's user data.
            directoryRecord(rootSector.toLong(), 2048L, " ").copyInto(pvd, 156)
            put(16, pvd)

            val root = ByteArray(2048)
            var at = 0
            directoryRecord(rootSector.toLong(), 2048L, " ").let { it.copyInto(root, at); at += it.size }
            directoryRecord(rootSector.toLong(), 2048L, "").let { it.copyInto(root, at); at += it.size }
            if (includeSystemCnf) {
                directoryRecord(systemCnfSector.toLong(), cnf.size.toLong(), "SYSTEM.CNF;1")
                    .let { it.copyInto(root, at); at += it.size }
            }
            directoryRecord(systemCnfSector.toLong() + 1, 4096L, "OTHER.DAT;1")
                .let { it.copyInto(root, at) }
            put(rootSector, root)

            if (includeSystemCnf) put(systemCnfSector, cnf.copyOf(maxOf(cnf.size, 16)))
        }
        return file
    }

    /** One ISO9660 directory record. Length is padded even, as the spec requires. */
    private fun directoryRecord(extent: Long, size: Long, name: String): ByteArray {
        val nameBytes = name.toByteArray(Charsets.ISO_8859_1)
        var length = 33 + nameBytes.size
        if (length % 2 != 0) length++
        val record = ByteArray(length)
        record[0] = length.toByte()
        putBothEndian32(record, 2, extent)
        putBothEndian32(record, 10, size)
        record[25] = 0                      // file flags
        putBothEndian16(record, 28, 1)      // volume sequence number
        record[32] = nameBytes.size.toByte()
        nameBytes.copyInto(record, 33)
        return record
    }

    private fun putBothEndian32(buf: ByteArray, at: Int, value: Long) {
        for (i in 0 until 4) buf[at + i] = ((value shr (8 * i)) and 0xFF).toByte()
        for (i in 0 until 4) buf[at + 4 + i] = ((value shr (8 * (3 - i))) and 0xFF).toByte()
    }

    private fun putBothEndian16(buf: ByteArray, at: Int, value: Int) {
        buf[at] = (value and 0xFF).toByte()
        buf[at + 1] = ((value shr 8) and 0xFF).toByte()
        buf[at + 2] = ((value shr 8) and 0xFF).toByte()
        buf[at + 3] = (value and 0xFF).toByte()
    }
}
