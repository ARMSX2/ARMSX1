/*
    Host gate for disc serial identification (psx/discid.c).

    What this is defending.

    The serial a disc reports about itself is its IDENTITY inside ARMSX: the box art is fetched
    by it, per-game settings are keyed on it, play time accrues under it and RetroAchievements
    identifies against it. A .chd used to produce no serial at all — the Kotlin extractor cannot
    see inside a compressed container — so every CHD in a library fell back to a curated
    filename table and, failing that, to a blank placeholder tile.

    psx/discid.c fixes that by reading the disc through the SAME vtable the emulated drive reads
    through, so whatever libchdr can boot, this can identify. That makes the risk here a
    DIFFERENT one: a serial that is confidently WRONG is worse than one that is absent, because
    it silently attaches one game's settings, art and achievements to another. So every case
    below is either "this exact disc yields this exact serial" or "this disc yields NOTHING".

    Geometry is the whole difficulty. The same filesystem arrives at these functions in four
    shapes, and nothing in the container says which:

        CHD          ISO sector 0 at LBA 150 (its LBA space includes the lead-in), user data
                     24 bytes into a 2352-byte MODE2 sector
        raw MODE2    ISO sector 0 at LBA 0, user data +24
        raw MODE1    ISO sector 0 at LBA 0, user data +16
        plain ISO    ISO sector 0 at LBA 0, 2048-byte sectors, user data +0

    The CHD geometry is the one that matters most and the one no file-backed fixture can produce,
    so it is driven here through a synthetic disc vtable that mimics exactly that addressing.
    libchdr's own decompression is not re-tested: CHDs already boot, and this deliberately does
    not contain a second CHD decoder that could disagree with the one that does.

    Pass a real image path as argv[1] to identify it and print the result — that is the
    end-to-end check against an actual .chd, which cannot be synthesised here (writing one needs
    chdman).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/discid.h"
#include "../psx/dev/cdrom/disc.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

static int g_failures = 0;

static void check(int condition, const char* group, const char* what) {
    if (!condition) {
        printf("  FAIL [%s] %s\n", group, what);
        ++g_failures;
    }
}

static void check_serial(const char* got, int found, const char* expected,
                         const char* group, const char* what) {
    if (expected) {
        if (!found || strcmp(got, expected) != 0) {
            printf("  FAIL [%s] %s: expected %s, got %s\n",
                   group, what, expected, found ? got : "(nothing)");
            ++g_failures;
        }
    } else if (found) {
        printf("  FAIL [%s] %s: expected nothing, got %s\n", group, what, got);
        ++g_failures;
    }
}

/* ---- synthetic ISO9660 -------------------------------------------------------------------- */

#define USER_BYTES 2048
#define ROOT_SECTOR 22

static void put_le32(unsigned char* p, unsigned int value) {
    p[0] = (unsigned char)(value & 0xff);
    p[1] = (unsigned char)((value >> 8) & 0xff);
    p[2] = (unsigned char)((value >> 16) & 0xff);
    p[3] = (unsigned char)((value >> 24) & 0xff);
}

static void put_be32(unsigned char* p, unsigned int value) {
    p[0] = (unsigned char)((value >> 24) & 0xff);
    p[1] = (unsigned char)((value >> 16) & 0xff);
    p[2] = (unsigned char)((value >> 8) & 0xff);
    p[3] = (unsigned char)(value & 0xff);
}

/* One ISO9660 directory record, both-endian as the spec requires, padded to an even length.
   Returns how many bytes were written. */
static unsigned int directory_record(unsigned char* out, unsigned int extent, unsigned int size,
                                     const char* name) {
    const unsigned int name_len = (unsigned int)strlen(name);
    unsigned int length = 33 + name_len;

    if (length & 1)
        ++length;

    memset(out, 0, length);
    out[0] = (unsigned char)length;
    put_le32(out + 2, extent);
    put_be32(out + 6, extent);
    put_le32(out + 10, size);
    put_be32(out + 14, size);
    out[32] = (unsigned char)name_len;
    memcpy(out + 33, name, name_len);

    return length;
}

/*
    A whole disc's worth of ISO9660 user data, sparse: only the volume descriptor, the root
    directory and SYSTEM.CNF are populated. `boot_line` is written verbatim as SYSTEM.CNF, so a
    case can spell it however a real mastering tool would.
*/
typedef struct {
    unsigned char* sectors;     /* iso_sector_count * USER_BYTES */
    unsigned int iso_sector_count;
} iso_image_t;

static void iso_free(iso_image_t* image) {
    free(image->sectors);
    image->sectors = NULL;
    image->iso_sector_count = 0;
}

static void iso_build(iso_image_t* image, const char* boot_line, unsigned int cnf_sector,
                      int include_system_cnf) {
    unsigned char* pvd;
    unsigned char* root;
    unsigned int at = 0;
    const unsigned int cnf_len = (unsigned int)strlen(boot_line);

    image->iso_sector_count = cnf_sector + 2;
    if (image->iso_sector_count < ROOT_SECTOR + 2)
        image->iso_sector_count = ROOT_SECTOR + 2;

    image->sectors = calloc(image->iso_sector_count, USER_BYTES);

    pvd = image->sectors + (size_t)16 * USER_BYTES;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 8, "PLAYSTATION ", 12);
    directory_record(pvd + 156, ROOT_SECTOR, USER_BYTES, " ");

    root = image->sectors + (size_t)ROOT_SECTOR * USER_BYTES;
    at += directory_record(root + at, ROOT_SECTOR, USER_BYTES, " ");
    at += directory_record(root + at, ROOT_SECTOR, USER_BYTES, "");
    if (include_system_cnf)
        at += directory_record(root + at, cnf_sector, cnf_len, "SYSTEM.CNF;1");
    directory_record(root + at, cnf_sector + 1, 4096, "OTHER.DAT;1");

    if (include_system_cnf)
        memcpy(image->sectors + (size_t)cnf_sector * USER_BYTES, boot_line, cnf_len);
}

/* ---- synthetic disc vtable ----------------------------------------------------------------- */

/*
    The container the emulated drive sees, parameterised by the two things that actually differ
    between a CHD and a raw rip: where ISO sector 0 sits in LBA space, and how far into the
    returned sector the user data starts. This is how the CHD geometry gets covered without a
    CHD: chd_read_sector hands back a full 2352-byte MODE2 sector addressed from LBA 150, and so
    does this.
*/
typedef struct {
    const iso_image_t* image;
    unsigned int base_lba;
    unsigned int user_offset;
    int report_track_lba;       /* what get_track_lba(1) answers; -1 = no track table */
} fake_disc_t;

static int fake_read(void* udata, uint32_t lba, void* buf) {
    fake_disc_t* fake = (fake_disc_t*)udata;
    unsigned char* out = (unsigned char*)buf;

    memset(out, 0, CD_SECTOR_SIZE);

    if (lba < fake->base_lba)
        return TS_PREGAP;

    {
        const uint32_t iso_sector = lba - fake->base_lba;

        if (iso_sector >= fake->image->iso_sector_count)
            return TS_FAR;

        memcpy(out + fake->user_offset,
               fake->image->sectors + (size_t)iso_sector * USER_BYTES, USER_BYTES);
    }

    return TS_DATA;
}

static int fake_query(void* udata, uint32_t lba) {
    (void)udata; (void)lba;
    return TS_DATA;
}

static int fake_track_number(void* udata, uint32_t lba) {
    (void)udata; (void)lba;
    return 1;
}

static int fake_track_count(void* udata) {
    (void)udata;
    return 1;
}

static uint32_t fake_track_lba(void* udata, int track) {
    fake_disc_t* fake = (fake_disc_t*)udata;
    (void)track;
    return (fake->report_track_lba < 0) ? 0u : (uint32_t)fake->report_track_lba;
}

static void fake_destroy(void* udata) {
    (void)udata;
}

static void fake_disc_init(psx_disc_t* disc, fake_disc_t* fake) {
    memset(disc, 0, sizeof(*disc));
    disc->udata = fake;
    disc->read_sector = fake_read;
    disc->query_sector = fake_query;
    disc->get_track_number = fake_track_number;
    disc->get_track_count = fake_track_count;
    disc->get_track_lba = fake_track_lba;
    disc->destroy = fake_destroy;
}

/* Identify `image` presented with the given geometry. */
static int identify_fake(const iso_image_t* image, unsigned int base_lba, unsigned int user_offset,
                         int report_track_lba, char* out, size_t out_size) {
    psx_disc_t disc;
    fake_disc_t fake;

    fake.image = image;
    fake.base_lba = base_lba;
    fake.user_offset = user_offset;
    fake.report_track_lba = report_track_lba;

    fake_disc_init(&disc, &fake);

    return psx_discid_from_disc(&disc, out, out_size);
}

/* ---- file-backed fixtures ------------------------------------------------------------------- */

/* Writes `image` out as a real file the disc reader will open by extension. `lead_sectors` of
   blank space go in front of it, which is how a rip that captured the pregap ahead of the
   filesystem is laid out — the volume descriptor is then NOT at file sector 16. */
static int write_image_file(const iso_image_t* image, const char* path,
                            unsigned int sector_bytes, unsigned int user_offset,
                            unsigned int lead_sectors) {
    FILE* f = fopen(path, "wb");
    unsigned char* sector;
    unsigned int i;

    if (!f)
        return 0;

    sector = calloc(1, sector_bytes);

    for (i = 0; i < lead_sectors; ++i) {
        if (fwrite(sector, 1, sector_bytes, f) != sector_bytes) {
            free(sector);
            fclose(f);
            return 0;
        }
    }

    for (i = 0; i < image->iso_sector_count; ++i) {
        memset(sector, 0, sector_bytes);
        memcpy(sector + user_offset, image->sectors + (size_t)i * USER_BYTES, USER_BYTES);

        if (fwrite(sector, 1, sector_bytes, f) != sector_bytes) {
            free(sector);
            fclose(f);
            return 0;
        }
    }

    free(sector);
    fclose(f);

    return 1;
}

static void join(char* out, size_t out_size, const char* dir, const char* name) {
    snprintf(out, out_size, "%s%s%s", dir, (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
}

/* ---- cases ----------------------------------------------------------------------------------- */

/*
    The case this file exists for. A CHD addresses ISO sector 0 at the track-1 LBA — 150 on every
    standard disc, because its LBA space includes the lead-in — and returns a full 2352-byte
    MODE2 sector. Read it as if it were a raw rip (base 0) and sector 16 is 150 sectors short of
    the volume descriptor: no filesystem, no serial, blank tile.
*/
static void case_chd_geometry(void) {
    const char* name = "chd geometry";
    iso_image_t image;
    char serial[PSX_DISCID_MAX];
    int found;

    iso_build(&image, "BOOT = cdrom:\\SLUS_006.64;1\r\n", 24, 1);

    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "lead-in offset filesystem (LBA 150, +24)");

    /* Same disc, but the container does not report a track table. The serial still has to come
       out: a reader that only trusts get_track_lba() would go blind here. */
    found = identify_fake(&image, 150, 24, -1, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "LBA 150 with no track table reported");

    /* MODE1 sectors put the user data 8 bytes earlier. */
    found = identify_fake(&image, 150, 16, 150, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "MODE1 sectors at LBA 150");

    /* And the raw-rip geometry, for completeness. */
    found = identify_fake(&image, 0, 24, 0, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "raw MODE2 rip at LBA 0");

    found = identify_fake(&image, 0, 0, 0, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "2048-byte sectors at LBA 0");

    /* A track table that does not match the filesystem. Trusting it and stopping there is how a
       disc silently loses its identity; the search has to fall through to the next start point. */
    found = identify_fake(&image, 0, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, "SLUS-00664", name, "track table says 150, filesystem is at 0");

    iso_free(&image);
}

/* SYSTEM.CNF is wherever the mastering tool put it — ISO9660 records the location, it does not
   promise proximity. A walk that follows the directory record does not care; a window scan does. */
static void case_system_cnf_far_into_the_disc(void) {
    const char* name = "deep SYSTEM.CNF";
    iso_image_t image;
    char serial[PSX_DISCID_MAX];
    int found;

    iso_build(&image, "BOOT = cdrom:\\SCUS_941.63;1\r\n", 9000, 1);

    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, "SCUS-94163", name, "SYSTEM.CNF at ISO sector 9000");

    iso_free(&image);
}

static void case_boot_line_spellings(void) {
    const char* name = "BOOT spellings";
    static const struct {
        const char* line;
        const char* expected;
    } cases[] = {
        { "BOOT=cdrom:\\SLUS_006.64;1\n",                        "SLUS-00664" },
        { "boot = cdrom:SLUS_006.64;1\n",                        "SLUS-00664" },
        { "BOOT\t=\tcdrom0:\\SLUS_006.64;1\n",                   "SLUS-00664" },
        { "BOOT = cdrom:\\\\SLUS_006.64;1\n",                    "SLUS-00664" },
        { "BOOT = cdrom:/SLUS_006.64;1\n",                       "SLUS-00664" },
        { "BOOT = cdrom:\\SLUS_006.64\n",                        "SLUS-00664" },
        { "BOOT = cdrom:\\SLUS_00664;1\n",                       "SLUS-00664" },
        { "BOOT = cdrom:\\SCES-023.80;1\n",                      "SCES-02380" },
        { "BOOT = cdrom:\\slus_006.64;1\n",                      "SLUS-00664" },
        { "TCB = 4\nEVENT = 10\nBOOT = cdrom:\\SCUS_941.63;1\n", "SCUS-94163" },
        /* Not a serial: homebrew and a handful of licensed discs name their executable outright.
           There is nothing to key on, and inventing one would be the wrong kind of helpful. */
        { "BOOT = cdrom:\\PSX.EXE;1\n",                          NULL },
        { "BOOT = cdrom:\\MAIN.EXE;1\n",                         NULL },
        /* A PS2 spelling has no business identifying a PS1 disc. */
        { "BOOT2 = cdrom0:\\SLUS_200.02;1\n",                    NULL },
        /* Too few digits to be a serial. */
        { "BOOT = cdrom:\\SLUS_006.6;1\n",                       NULL },
    };
    const size_t count = sizeof(cases) / sizeof(cases[0]);
    size_t i;

    for (i = 0; i < count; ++i) {
        iso_image_t image;
        char serial[PSX_DISCID_MAX];
        int found;

        iso_build(&image, cases[i].line, 24, 1);
        found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
        check_serial(serial, found, cases[i].expected, name, cases[i].line);
        iso_free(&image);
    }
}

/* Every one of these has to come back with NOTHING. A wrong serial is worse than no serial. */
static void case_discs_with_no_identity(void) {
    const char* name = "no identity";
    iso_image_t image;
    char serial[PSX_DISCID_MAX];
    int found;

    iso_build(&image, "BOOT = cdrom:\\SLUS_006.64;1\r\n", 24, 0);
    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, NULL, name, "no SYSTEM.CNF in the root directory");
    check(serial[0] == '\0', name, "a failed identification leaves an empty string");
    iso_free(&image);

    iso_build(&image, "TCB = 4\nEVENT = 10\nSTACK = 801FFFF0\n", 24, 1);
    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, NULL, name, "SYSTEM.CNF with no BOOT line");
    iso_free(&image);

    /* No volume descriptor anywhere: an audio CD, or an image whose filesystem is gone. The old
       Kotlin extractor falls back to a byte scan here; this deliberately does not, because a
       scan over a container the drive can decompress is neither cheap nor more correct. */
    iso_build(&image, "BOOT = cdrom:\\SLUS_006.64;1\r\n", 24, 1);
    memset(image.sectors + (size_t)16 * USER_BYTES, 0, USER_BYTES);
    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, NULL, name, "no volume descriptor");
    iso_free(&image);

    /* A root directory extent claiming to be enormous is corruption, not a directory. */
    iso_build(&image, "BOOT = cdrom:\\SLUS_006.64;1\r\n", 24, 1);
    put_le32(image.sectors + (size_t)16 * USER_BYTES + 156 + 10, 0x7fffffffu);
    found = identify_fake(&image, 150, 24, 150, serial, sizeof(serial));
    check_serial(serial, found, NULL, name, "absurd root directory size");
    iso_free(&image);
}

static void case_bad_arguments(void) {
    const char* name = "arguments";
    char serial[PSX_DISCID_MAX];

    check(psx_discid_from_disc(NULL, serial, sizeof(serial)) == 0, name, "null disc");
    check(psx_discid_from_path(NULL, serial, sizeof(serial)) == 0, name, "null path");
    check(psx_discid_from_path("", serial, sizeof(serial)) == 0, name, "empty path");
    check(psx_discid_from_path("/nonexistent/nowhere.bin", serial, sizeof(serial)) == 0,
          name, "a path that is not there");
    check(serial[0] == '\0', name, "a failed open leaves an empty string");
    check(psx_discid_from_disc(NULL, NULL, 0) == 0, name, "null output buffer");
}

/*
    The same identification, but through psx_disc_open() — i.e. the real reader, opened by
    extension, on real files. This is what the JNI entry point actually calls.
*/
static void case_real_files(const char* dir) {
    const char* name = "file-backed";
    iso_image_t image;
    char path[1024];
    char serial[PSX_DISCID_MAX];

    iso_build(&image, "BOOT = cdrom:\\SLUS_005.94;1\r\n", 30, 1);

    join(path, sizeof(path), dir, "mode2.bin");
    if (write_image_file(&image, path, 2352, 24, 0)) {
        check_serial(serial, psx_discid_from_path(path, serial, sizeof(serial)),
                     "SLUS-00594", name, "raw MODE2/2352 .bin");
        remove(path);
    } else {
        check(0, name, "could not write the .bin fixture");
    }

    join(path, sizeof(path), dir, "mode1.bin");
    if (write_image_file(&image, path, 2352, 16, 0)) {
        check_serial(serial, psx_discid_from_path(path, serial, sizeof(serial)),
                     "SLUS-00594", name, "raw MODE1/2352 .bin");
        remove(path);
    } else {
        check(0, name, "could not write the MODE1 .bin fixture");
    }

    join(path, sizeof(path), dir, "plain.iso");
    if (write_image_file(&image, path, 2048, 0, 0)) {
        check_serial(serial, psx_discid_from_path(path, serial, sizeof(serial)),
                     "SLUS-00594", name, "plain 2048-byte .iso");
        remove(path);
    } else {
        check(0, name, "could not write the .iso fixture");
    }

    /* A rip that captured the pregap: the volume descriptor is at file sector 28, not 16. The
       raw reader has no track table to ask, so the base has to be found by looking. */
    join(path, sizeof(path), dir, "pregap.bin");
    if (write_image_file(&image, path, 2352, 24, 12)) {
        check_serial(serial, psx_discid_from_path(path, serial, sizeof(serial)),
                     "SLUS-00594", name, "raw .bin with a 12-sector pregap");
        remove(path);
    } else {
        check(0, name, "could not write the pregap fixture");
    }

    /* An extension the reader does not open at all must fail, not crash. */
    join(path, sizeof(path), dir, "notadisc.txt");
    {
        FILE* f = fopen(path, "wb");

        if (f) {
            fwrite("BOOT = cdrom:\\SLUS_006.64;1\n", 1, 28, f);
            fclose(f);
            check(psx_discid_from_path(path, serial, sizeof(serial)) == 0,
                  name, "an unsupported extension identifies nothing");
            remove(path);
        }
    }

    iso_free(&image);
}

/* ---- entry point -------------------------------------------------------------------------- */

/*
    `disc_serial [scratch-dir]` runs the gate; `disc_serial --image <path>` identifies one real
    image and prints what it got. The second mode is how an actual .chd gets checked end to end —
    synthesising one here would mean vendoring a CHD writer, and the whole point of this module
    is that the build contains exactly ONE CHD decoder.
*/
int main(int argc, char** argv) {
    const char* dir;

    if (argc > 2 && strcmp(argv[1], "--image") == 0) {
        char serial[PSX_DISCID_MAX];
        const int found = psx_discid_from_path(argv[2], serial, sizeof(serial));

        printf("%s -> %s\n", argv[2], found ? serial : "NO SERIAL");

        return found ? 0 : 1;
    }

    dir = (argc > 1) ? argv[1] : ".";

    case_chd_geometry();
    case_system_cnf_far_into_the_disc();
    case_boot_line_spellings();
    case_discs_with_no_identity();
    case_bad_arguments();
    case_real_files(dir);

    if (g_failures) {
        printf("DISC_SERIAL FAILED (%d)\n", g_failures);

        return 1;
    }

    printf("DISC_SERIAL OK\n");

    return 0;
}
