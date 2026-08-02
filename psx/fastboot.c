/*
    ARMSX — Skip BIOS (fast boot). See fastboot.h for the design and the failure policy.
*/

#include "fastboot.h"

#include <string.h>
#include <stdlib.h>

#include "bus_init.h"
#include "exe.h"
#include "log.h"
#include "dev/cdrom/cdrom.h"
#include "dev/cdrom/disc.h"
#include "dev/ram.h"

/* A raw sector is 2352 bytes; the 2048 bytes of user data start at offset 24 (12 sync,
   4 header, 8 subheader). Confirmed against disc.c's PVD probe, which finds the ISO system
   identifier — user-data offset 8 — at sector byte 0x20. 24 + 8 = 32 = 0x20. */
#define FB_SECTOR_BYTES     2352
#define FB_USER_DATA_OFFSET 24
#define FB_USER_DATA_BYTES  2048

/* psx_disc_read() takes an LBA that includes the 150-sector lead-in: disc.c reads the PVD, ISO
   sector 16, as MSF_TO_LBA(0, 2, 16) = 2*75 + 16 = 166. So ISO sector N is LBA N + 150. */
#define FB_LEAD_IN          150
#define FB_ISO_PVD_SECTOR   16

/* ISO9660 directory record layout, from the start of the record. */
#define FB_DR_LENGTH        0
#define FB_DR_EXTENT_LE     2
#define FB_DR_SIZE_LE       10
#define FB_DR_NAME_LEN      32
#define FB_DR_NAME          33
/* Root directory record, from the start of the PVD's user data. */
#define FB_PVD_ROOT_DR      156

/* Default ON, matching the documented default in the settings.toml template and the launcher's
   own default. This must NOT be 0: an existing settings.toml written before the key existed has
   no [cpu] fast_boot line at all, so config.c never calls the setter and whatever sits here is
   what every upgrading install gets. Defaulting off would silently disable the feature for
   exactly the people who already had the (previously inert) switch showing "on". */
static int g_fastboot_enabled = 1;
static int g_fastboot_armed = 1;

void psx_fastboot_set_enabled(int enabled) {
    g_fastboot_enabled = enabled ? 1 : 0;
    /* Turning it on mid-session should still take effect on the NEXT boot rather than silently
       waiting for a reset that may never come. */
    g_fastboot_armed = g_fastboot_enabled;
}

int psx_fastboot_enabled(void) {
    return g_fastboot_enabled;
}

void psx_fastboot_rearm(void) {
    g_fastboot_armed = g_fastboot_enabled;
}

static char tolower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static uint32_t fb_read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reads one ISO sector's 2048 user bytes. Returns 1 on success. */
static int fb_read_iso_sector(psx_disc_t* disc, uint32_t iso_lba, uint8_t* out) {
    uint8_t sector[FB_SECTOR_BYTES];

    if (!disc)
        return 0;

    if (!psx_disc_read(disc, iso_lba + FB_LEAD_IN, sector))
        return 0;

    memcpy(out, sector + FB_USER_DATA_OFFSET, FB_USER_DATA_BYTES);

    return 1;
}

/*
    Compares an ISO9660 directory-record name against a plain filename, case-insensitively and
    ignoring the ";1" version suffix ISO9660 appends. Both matter: discs store "SYSTEM.CNF;1",
    and the BOOT= line's spelling of the executable does not reliably match the directory's case.
*/
static int fb_name_matches(const uint8_t* record_name, int record_len, const char* wanted) {
    int i = 0;

    for (; i < record_len; ++i) {
        char c = (char)record_name[i];

        if (c == ';')
            break;

        if (!wanted[i])
            return 0;

        if (tolower_ascii(c) != tolower_ascii(wanted[i]))
            return 0;
    }

    return wanted[i] == '\0';
}

/* Walks the root directory looking for `name`. On success fills extent/size and returns 1. */
static int fb_find_in_root(psx_disc_t* disc, const char* name,
                           uint32_t* out_extent, uint32_t* out_size) {
    uint8_t buffer[FB_USER_DATA_BYTES];
    uint32_t root_extent, root_size, sector, sectors;

    if (!fb_read_iso_sector(disc, FB_ISO_PVD_SECTOR, buffer))
        return 0;

    /* "CD001" at user-data offset 1 marks a valid volume descriptor. Without this check a
       non-ISO disc walks arbitrary bytes as if they were directory records. */
    if (memcmp(buffer + 1, "CD001", 5) != 0)
        return 0;

    root_extent = fb_read_le32(buffer + FB_PVD_ROOT_DR + FB_DR_EXTENT_LE);
    root_size = fb_read_le32(buffer + FB_PVD_ROOT_DR + FB_DR_SIZE_LE);

    if (!root_size || root_size > (16u * 1024u * 1024u))
        return 0;

    sectors = (root_size + FB_USER_DATA_BYTES - 1) / FB_USER_DATA_BYTES;

    for (sector = 0; sector < sectors; ++sector) {
        uint32_t offset = 0;

        if (!fb_read_iso_sector(disc, root_extent + sector, buffer))
            return 0;

        while (offset < FB_USER_DATA_BYTES) {
            const uint8_t* record = buffer + offset;
            const uint8_t length = record[FB_DR_LENGTH];
            uint8_t name_len;

            /* A zero length means "no more records in this sector" — the rest is padding, and
               the next record starts at the next sector boundary. */
            if (length == 0)
                break;

            if (offset + length > FB_USER_DATA_BYTES)
                break;

            name_len = record[FB_DR_NAME_LEN];

            if (name_len && (uint32_t)(FB_DR_NAME + name_len) <= length) {
                if (fb_name_matches(record + FB_DR_NAME, name_len, name)) {
                    *out_extent = fb_read_le32(record + FB_DR_EXTENT_LE);
                    *out_size = fb_read_le32(record + FB_DR_SIZE_LE);

                    return (*out_size != 0);
                }
            }

            offset += length;
        }
    }

    return 0;
}

/*
    Pulls the executable name out of SYSTEM.CNF's BOOT line, e.g.

        BOOT = cdrom:\SLUS_005.94;1

    Returns 1 and fills `out` with "SLUS_005.94". Tolerates the many spellings real discs use:
    "BOOT=" with no spaces, lower-case "cdrom:", forward or back slashes, a leading slash or
    none, and a missing ";1".
*/
static int fb_parse_boot_line(const char* text, size_t length, char* out, size_t out_size) {
    size_t i;

    for (i = 0; i + 4 < length; ++i) {
        const char* p;
        size_t written = 0;

        if (!(text[i] == 'B' || text[i] == 'b'))
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

        /* Skip the "cdrom:" scheme and any leading slashes. */
        if ((size_t)(p - text) + 6 <= length &&
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

        return written > 0;
    }

    return 0;
}

int psx_fastboot_try(psx_cpu_t* cpu) {
    uint8_t sector[FB_USER_DATA_BYTES];
    char boot_name[64];
    char cnf[FB_USER_DATA_BYTES + 1];
    psx_disc_t* disc;
    psx_exe_hdr_t hdr;
    uint32_t extent = 0, size = 0, ram_offset, remaining, iso_sector;
    uint8_t* dst;

    if (!g_fastboot_armed || !cpu || !cpu->bus || !cpu->bus->cdrom || !cpu->bus->ram)
        return 0;

    /* Fires once. Disarm FIRST so that any failure below cannot re-enter on the next
       instruction and retry the whole disc walk every cycle. */
    g_fastboot_armed = 0;

    disc = cpu->bus->cdrom->disc;

    if (!disc)
        return 0;

    /* SYSTEM.CNF names the executable. A disc without one is not a standard game disc
       (an audio CD, or a homebrew that boots differently) — let the BIOS handle it. */
    if (!fb_find_in_root(disc, "SYSTEM.CNF", &extent, &size)) {
        log_info("fastboot: no SYSTEM.CNF; booting the BIOS normally");

        return 0;
    }

    if (!fb_read_iso_sector(disc, extent, sector))
        return 0;

    memcpy(cnf, sector, FB_USER_DATA_BYTES);
    cnf[FB_USER_DATA_BYTES] = '\0';

    if (!fb_parse_boot_line(cnf, (size < FB_USER_DATA_BYTES) ? size : FB_USER_DATA_BYTES,
                            boot_name, sizeof(boot_name))) {
        log_info("fastboot: SYSTEM.CNF has no usable BOOT line; booting the BIOS normally");

        return 0;
    }

    if (!fb_find_in_root(disc, boot_name, &extent, &size)) {
        log_info("fastboot: '%s' not in the root directory; booting the BIOS normally", boot_name);

        return 0;
    }

    /* Header first: it says where the image belongs and how much of it there is. */
    if (!fb_read_iso_sector(disc, extent, sector))
        return 0;

    memcpy(&hdr, sector, sizeof(hdr));

    if (memcmp(hdr.id, "PS-X EXE", 8) != 0) {
        log_info("fastboot: '%s' is not a PS-X EXE; booting the BIOS normally", boot_name);

        return 0;
    }

    ram_offset = hdr.ramdest & 0x7fffffff;

    /* Refuse anything that would land outside RAM rather than trusting the disc's header —
       a truncated or corrupt image must not be able to write past the buffer. */
    if (!hdr.filesz || ram_offset >= PSX_RAM_SIZE || hdr.filesz > (PSX_RAM_SIZE - ram_offset)) {
        log_error("fastboot: '%s' header out of range (dest=%08x size=%u); booting normally",
                  boot_name, hdr.ramdest, hdr.filesz);

        return 0;
    }

    /* The image starts after the 0x800-byte header, i.e. one sector in. */
    dst = cpu->bus->ram->buf + ram_offset;
    remaining = hdr.filesz;
    iso_sector = extent + 1;

    while (remaining) {
        const uint32_t chunk = (remaining < FB_USER_DATA_BYTES) ? remaining : FB_USER_DATA_BYTES;

        if (!fb_read_iso_sector(disc, iso_sector, sector)) {
            log_error("fastboot: read failed %u bytes in; the BIOS has already been skipped, "
                      "so this boot cannot recover", hdr.filesz - remaining);

            return 0;
        }

        memcpy(dst, sector, chunk);

        dst += chunk;
        remaining -= chunk;
        ++iso_sector;
    }

    /* The recompiler's decode cache may hold entries for these addresses from the BIOS's own
       use of them. Without this the game runs stale handlers. */
    psx_cpu_invalidate_range(cpu, ram_offset, hdr.filesz);

    cpu->pc = hdr.ipc;
    cpu->next_pc = cpu->pc + 4;
    cpu->r[28] = hdr.igp;

    if (hdr.ispb) {
        cpu->r[29] = hdr.ispb + hdr.ispoff;
        cpu->r[30] = cpu->r[29];
    }

    /* Deliberately log_info, not log_fatal. psx_exe_load() uses log_fatal for its register dump,
       which bypasses log_level and spams the diagnostic file. */
    log_info("fastboot: booted '%s' directly (PC=%08x SP=%08x GP=%08x, %u bytes at %08x)",
             boot_name, cpu->pc, cpu->r[29], cpu->r[28], hdr.filesz, hdr.ramdest);

    return 1;
}
