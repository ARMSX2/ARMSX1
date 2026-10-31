/*
    ARMSX — disc serial identification. See discid.h for why this exists and what it guarantees.
*/

#include "discid.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* ISO9660 logical sector: the 2048 bytes of user data inside whatever the container hands back. */
#define DISCID_USER_BYTES   2048

/* Where the primary volume descriptor lives, in ISO sectors. */
#define DISCID_PVD_SECTOR   16

/* How many sectors past a start point to look for it. Wide enough to cover a rip that captured
   the pregap ahead of the filesystem, narrow enough that a container with no filesystem at all
   costs a handful of reads rather than a scan of the whole image. */
#define DISCID_VD_SEARCH_SECTORS 64

/* ISO9660 directory-record layout, from the start of the record. */
#define DISCID_DR_LENGTH    0
#define DISCID_DR_EXTENT_LE 2
#define DISCID_DR_SIZE_LE   10
#define DISCID_DR_NAME_LEN  32
#define DISCID_DR_NAME      33

/* Root directory record, from the start of the PVD's user data. */
#define DISCID_PVD_ROOT_DR  156

/* A root directory bigger than this is a corrupt extent, not a directory. */
#define DISCID_MAX_ROOT_BYTES (16u * 1024u * 1024u)

/* Longest boot name we will carry ("SLUS_005.94" and friends, plus room for oddities). */
#define DISCID_MAX_BOOT_NAME 64

typedef struct {
    /* LBA that ISO sector 0 sits at. 150 on a CHD (the lead-in is part of its LBA space),
       0 on a raw file-backed rip whose first byte is ISO sector 0. */
    uint32_t base_lba;
    /* Byte offset of the 2048 user bytes inside the sector the container returns: 24 for
       MODE2/2352 (12 sync + 4 header + 8 subheader), 16 for MODE1/2352, 0 for a plain
       2048-byte ISO. */
    uint32_t user_offset;
} discid_layout_t;

static char discid_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static char discid_toupper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int discid_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int discid_is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static uint32_t discid_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
    Reads one ISO sector's 2048 user bytes through `layout`. Returns 1 on success.

    Deliberately tolerant of the sector TYPE: the cue reader reports anything that is not
    MODE2/2352 as TS_AUDIO even when it is a MODE1 data track, so demanding TS_DATA here would
    refuse to identify perfectly ordinary MODE1 rips. Nothing is trusted on that basis — the
    "CD001" check below is what decides whether these bytes are a filesystem.
*/
static int discid_read_iso(psx_disc_t* disc, const discid_layout_t* layout,
                           uint32_t iso_sector, uint8_t* user_out) {
    uint8_t sector[CD_SECTOR_SIZE];
    int type;

    memset(sector, 0, sizeof(sector));

    type = psx_disc_read(disc, layout->base_lba + iso_sector, sector);

    if (type == 0 || type == TS_FAR)
        return 0;

    memcpy(user_out, sector + layout->user_offset, DISCID_USER_BYTES);

    return 1;
}

/* Type 1 + "CD001" is a primary volume descriptor. Both are required: without the type byte a
   non-ISO image can be walked as if arbitrary bytes were directory records. */
static int discid_is_pvd(const uint8_t* user) {
    return user[0] == 1 && memcmp(user + 1, "CD001", 5) == 0;
}

/*
    Compares an ISO9660 directory-record name against a plain filename, case-insensitively and
    ignoring the ";1" version suffix. Both matter: discs store "SYSTEM.CNF;1", and mastering
    tools do not agree on case.
*/
static int discid_name_matches(const uint8_t* record_name, int record_len, const char* wanted) {
    int i = 0;

    for (; i < record_len; ++i) {
        const char c = (char)record_name[i];

        if (c == ';')
            break;

        if (!wanted[i])
            return 0;

        if (discid_tolower(c) != discid_tolower(wanted[i]))
            return 0;
    }

    return wanted[i] == '\0';
}

/* Walks the root directory for `name`. Fills extent/size and returns 1 on success. */
static int discid_find_in_root(psx_disc_t* disc, const discid_layout_t* layout, const char* name,
                               uint32_t* out_extent, uint32_t* out_size) {
    uint8_t buffer[DISCID_USER_BYTES];
    uint32_t root_extent, root_size, sector, sectors;

    if (!discid_read_iso(disc, layout, DISCID_PVD_SECTOR, buffer))
        return 0;

    if (!discid_is_pvd(buffer))
        return 0;

    root_extent = discid_read_le32(buffer + DISCID_PVD_ROOT_DR + DISCID_DR_EXTENT_LE);
    root_size = discid_read_le32(buffer + DISCID_PVD_ROOT_DR + DISCID_DR_SIZE_LE);

    if (!root_size || root_size > DISCID_MAX_ROOT_BYTES)
        return 0;

    sectors = (root_size + DISCID_USER_BYTES - 1) / DISCID_USER_BYTES;

    for (sector = 0; sector < sectors; ++sector) {
        uint32_t offset = 0;

        if (!discid_read_iso(disc, layout, root_extent + sector, buffer))
            return 0;

        while (offset < DISCID_USER_BYTES) {
            const uint8_t* record = buffer + offset;
            const uint8_t length = record[DISCID_DR_LENGTH];
            uint8_t name_len;

            /* Zero length means the rest of this sector is padding; the next record starts at
               the next sector boundary. */
            if (length == 0)
                break;

            if (offset + length > DISCID_USER_BYTES)
                break;

            name_len = record[DISCID_DR_NAME_LEN];

            if (name_len && (uint32_t)(DISCID_DR_NAME + name_len) <= length &&
                discid_name_matches(record + DISCID_DR_NAME, name_len, name)) {
                *out_extent = discid_read_le32(record + DISCID_DR_EXTENT_LE);
                *out_size = discid_read_le32(record + DISCID_DR_SIZE_LE);

                return (*out_size != 0);
            }

            offset += length;
        }
    }

    return 0;
}

/*
    Pulls the executable name out of SYSTEM.CNF's BOOT line:

        BOOT = cdrom:\SLUS_005.94;1   ->   "SLUS_005.94"

    Tolerates every spelling real discs use — "BOOT=" with no spaces, lower-case, "cdrom:" or
    "cdrom0:" or neither, forward or back slashes, a leading slash or none, a missing ";1".
    "BOOT2" (a PS2 spelling) falls out because the '=' check runs after the key name.
*/
static int discid_parse_boot_line(const char* text, size_t length, char* out, size_t out_size) {
    size_t i;

    for (i = 0; i + 4 < length; ++i) {
        const char* p;
        size_t written = 0;

        if (discid_tolower(text[i]) != 'b')
            continue;
        if (strncmp(text + i, "BOOT", 4) != 0 && strncmp(text + i, "boot", 4) != 0)
            continue;

        p = text + i + 4;

        while ((size_t)(p - text) < length && (*p == ' ' || *p == '\t'))
            ++p;
        if ((size_t)(p - text) >= length || *p != '=')
            continue;
        ++p;
        while ((size_t)(p - text) < length && (*p == ' ' || *p == '\t'))
            ++p;

        if ((size_t)(p - text) + 7 <= length &&
            (strncmp(p, "cdrom0:", 7) == 0 || strncmp(p, "CDROM0:", 7) == 0)) {
            p += 7;
        } else if ((size_t)(p - text) + 6 <= length &&
                   (strncmp(p, "cdrom:", 6) == 0 || strncmp(p, "CDROM:", 6) == 0)) {
            p += 6;
        }

        while ((size_t)(p - text) < length && (*p == '\\' || *p == '/'))
            ++p;

        while ((size_t)(p - text) < length && written + 1 < out_size) {
            const char c = *p;

            if (c == ';' || c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == '\0')
                break;

            out[written++] = c;
            ++p;
        }

        out[written] = '\0';

        if (written)
            return 1;
    }

    return 0;
}

/*
    "SLUS_005.94" -> "SLUS-00594". Returns 0 for a boot name that is not serial-shaped, which is
    a real and expected outcome: homebrew and a few licensed discs boot "PSX.EXE" or "MAIN.EXE".

    The accepted shape is deliberately the SAME one Ps1DiscId.kt accepts —
    ^([A-Za-z]{4})[_\-.]?(\d{3})\.?(\d{2}) — so a disc identified through this path and the same
    disc identified through the Kotlin path cannot produce two different keys for one game.
*/
static int discid_normalise(const char* boot_name, char* out, size_t out_size) {
    const char* name = boot_name;
    const char* p;
    size_t i;

    if (out_size < 11)
        return 0;

    /* The BOOT line may still carry a directory component on an unusual disc. */
    for (p = boot_name; *p; ++p) {
        if (*p == '\\' || *p == '/')
            name = p + 1;
    }

    for (i = 0; i < 4; ++i) {
        if (!discid_is_alpha(name[i]))
            return 0;
    }

    p = name + 4;

    if (*p == '_' || *p == '-' || *p == '.')
        ++p;

    for (i = 0; i < 3; ++i) {
        if (!discid_is_digit(p[i]))
            return 0;
    }

    /* Digits 4 and 5, with the "005.94" decimal point optional. */
    if (p[3] == '.') {
        if (!discid_is_digit(p[4]) || !discid_is_digit(p[5]))
            return 0;

        out[0] = discid_toupper(name[0]);
        out[1] = discid_toupper(name[1]);
        out[2] = discid_toupper(name[2]);
        out[3] = discid_toupper(name[3]);
        out[4] = '-';
        out[5] = p[0]; out[6] = p[1]; out[7] = p[2];
        out[8] = p[4]; out[9] = p[5];
        out[10] = '\0';

        return 1;
    }

    if (!discid_is_digit(p[3]) || !discid_is_digit(p[4]))
        return 0;

    out[0] = discid_toupper(name[0]);
    out[1] = discid_toupper(name[1]);
    out[2] = discid_toupper(name[2]);
    out[3] = discid_toupper(name[3]);
    out[4] = '-';
    out[5] = p[0]; out[6] = p[1]; out[7] = p[2]; out[8] = p[3]; out[9] = p[4];
    out[10] = '\0';

    return 1;
}

/* One full attempt at a single geometry: PVD -> root directory -> SYSTEM.CNF -> BOOT -> serial. */
static int discid_try_layout(psx_disc_t* disc, const discid_layout_t* layout,
                             char* out, size_t out_size) {
    uint8_t user[DISCID_USER_BYTES];
    char cnf[DISCID_USER_BYTES + 1];
    char boot_name[DISCID_MAX_BOOT_NAME];
    uint32_t extent = 0, size = 0, usable;

    if (!discid_read_iso(disc, layout, DISCID_PVD_SECTOR, user))
        return 0;

    if (!discid_is_pvd(user))
        return 0;

    if (!discid_find_in_root(disc, layout, "SYSTEM.CNF", &extent, &size))
        return 0;

    if (!discid_read_iso(disc, layout, extent, user))
        return 0;

    usable = (size && size < DISCID_USER_BYTES) ? size : DISCID_USER_BYTES;

    memcpy(cnf, user, usable);
    cnf[usable] = '\0';

    if (!discid_parse_boot_line(cnf, usable, boot_name, sizeof(boot_name)))
        return 0;

    if (!discid_normalise(boot_name, out, out_size)) {
        log_info("discid: BOOT names '%s', which is not a serial", boot_name);

        return 0;
    }

    return 1;
}

/* Appends `lba` to a start-point list unless it is already there. */
static void discid_push_start(uint32_t* starts, size_t* count, size_t capacity, uint32_t lba) {
    size_t i;

    for (i = 0; i < *count; ++i) {
        if (starts[i] == lba)
            return;
    }

    if (*count < capacity)
        starts[(*count)++] = lba;
}

int psx_discid_from_disc(psx_disc_t* disc, char* out, size_t out_size) {
    /* Byte offset of the user data inside whatever the container returns, most likely first. */
    static const uint32_t user_offsets[] = { 24, 16, 0 };
    static const size_t offset_count = sizeof(user_offsets) / sizeof(user_offsets[0]);

    uint32_t starts[3];
    size_t start_count = 0, i, j;
    uint32_t probe;
    int track_lba;

    if (!out || out_size == 0)
        return 0;

    out[0] = '\0';

    if (!disc || !disc->read_sector)
        return 0;

    /*
        Where to start looking for the volume descriptor.

        A CHD's LBA space includes the 150-sector lead-in, so its track 1 begins at 150 and its
        ISO sector 16 is LBA 166. A file-backed raw rip usually begins at 0 — but not always, as
        a rip that captured the pregap puts the filesystem an arbitrary few sectors in. So rather
        than assuming a base, each start point below is SEARCHED for the descriptor and the base
        is derived from where it was actually found. Same policy as the Kotlin extractor, which
        has to solve exactly this and does it by scanning the leading sectors.

        The raw bin/iso reader answers 0 for "no track table", so 150 is tried explicitly too.
    */
    track_lba = psx_disc_get_track_count(disc) >= 1 ? psx_disc_get_track_lba(disc, 1) : 0;

    if (track_lba > 0)
        discid_push_start(starts, &start_count, 3, (uint32_t)track_lba);
    discid_push_start(starts, &start_count, 3, 150);
    discid_push_start(starts, &start_count, 3, 0);

    /*
        Every candidate geometry is carried through to COMPLETION rather than being accepted on
        the volume descriptor alone. One that finds a plausible descriptor but no SYSTEM.CNF, or
        a BOOT line that is not serial-shaped, is a geometry that guessed wrong — falling through
        to the next is what stops a misread becoming a confidently WRONG serial, which would
        attach one game's cover, settings and achievements to another.
    */
    for (i = 0; i < start_count; ++i) {
        for (probe = 0; probe < DISCID_VD_SEARCH_SECTORS; ++probe) {
            uint8_t sector[CD_SECTOR_SIZE];
            const uint32_t lba = starts[i] + probe;
            int type;

            memset(sector, 0, sizeof(sector));

            type = psx_disc_read(disc, lba, sector);

            if (type == 0 || type == TS_FAR)
                break;

            /* The descriptor cannot sit before ISO sector 16, so neither can the base. */
            if (lba < DISCID_PVD_SECTOR)
                continue;

            for (j = 0; j < offset_count; ++j) {
                discid_layout_t layout;

                if (!discid_is_pvd(sector + user_offsets[j]))
                    continue;

                layout.base_lba = lba - DISCID_PVD_SECTOR;
                layout.user_offset = user_offsets[j];

                if (discid_try_layout(disc, &layout, out, out_size)) {
                    log_info("discid: %s (ISO sector 0 at LBA %u, user data +%u)",
                             out, layout.base_lba, layout.user_offset);

                    return 1;
                }
            }
        }
    }

    out[0] = '\0';

    return 0;
}

/*
    psx_disc_destroy() calls disc->destroy unconditionally, and a failed open may leave it null
    (the raw/PBP/CHD paths bail before wiring the vtable up) or set (the cue path installs it
    BEFORE it can fail to parse, and its cue_t has to be freed). psx_disc_close() is declared in
    disc.h but never implemented, so this split is the only correct teardown.
*/
static void discid_destroy_disc(psx_disc_t* disc) {
    if (!disc)
        return;

    if (disc->destroy)
        psx_disc_destroy(disc);
    else
        free(disc);
}

int psx_discid_from_path(const char* path, char* out, size_t out_size) {
    psx_disc_t* disc;
    int found;

    if (!out || out_size == 0)
        return 0;

    out[0] = '\0';

    if (!path || !path[0])
        return 0;

    disc = psx_disc_create();

    if (!disc)
        return 0;

    if (psx_disc_open(disc, path) == CDT_ERROR) {
        discid_destroy_disc(disc);

        return 0;
    }

    found = psx_discid_from_disc(disc, out, out_size);

    discid_destroy_disc(disc);

    return found;
}
