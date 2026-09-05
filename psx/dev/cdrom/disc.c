#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "disc.h"
#include "cue.h"
#include "chd.h"
#include "pbp.h"
#include "../../log.h"
#include "../../perf.h"

#define MSF_TO_LBA(m, s, f) ((m * 4500) + (s * 75) + f)

const char* disc_cd_extensions[] = {
    "cue",
    "bin",
    "iso",
#ifdef USE_CHD
    "chd",
#endif
    "pbp",
    /* Raw image with 2048- or 2352-byte sectors. */
    "img",
    0
};

typedef struct {
    FILE* file;
    uint32_t sector_size;
    uint32_t sector_count;
} raw_disc_t;

/* Raw images omit the standard 150-sector pregap. */
#define RAW_LBA_BIAS 150u

static int raw_read(void* udata, uint32_t lba, void* buf) {
    raw_disc_t* raw = (raw_disc_t*)udata;

    if (!raw || !raw->file || !buf)
        return 0;

    if (lba >= raw->sector_count + RAW_LBA_BIAS)
        return 0;

    memset(buf, 0, CD_SECTOR_SIZE);

    if (lba < RAW_LBA_BIAS) {
        /* Synthesize the omitted track-one pregap. */
        memset((uint8_t*)buf + 1, 0xff, 10);
        return TS_PREGAP;
    }

    if (fseek(raw->file, (long long)(lba - RAW_LBA_BIAS) * raw->sector_size, SEEK_SET))
        return 0;

    uint8_t* dst = (uint8_t*)buf;

    if (raw->sector_size == 2048)
        dst += 24;

    return fread(dst, 1, raw->sector_size, raw->file) == raw->sector_size ? TS_DATA : 0;
}

static int raw_query(void* udata, uint32_t lba) {
    raw_disc_t* raw = (raw_disc_t*)udata;

    if (!raw || lba >= raw->sector_count + RAW_LBA_BIAS)
        return TS_FAR;

    return (lba < RAW_LBA_BIAS) ? TS_PREGAP : TS_DATA;
}

static int raw_get_track_number(void* udata, uint32_t lba) {
    (void)udata; (void)lba;
    return 1;
}

static int raw_get_track_count(void* udata) {
    (void)udata;
    return 1;
}

static uint32_t raw_get_track_lba(void* udata, int track) {
    raw_disc_t* raw = (raw_disc_t*)udata;

    /* Track zero reports lead-out. */
    if (track == 0)
        return raw ? raw->sector_count + RAW_LBA_BIAS : RAW_LBA_BIAS;

    return (track == 1) ? RAW_LBA_BIAS : TS_FAR;
}

static void raw_destroy(void* udata) {
    raw_disc_t* raw = (raw_disc_t*)udata;

    if (raw && raw->file)
        fclose(raw->file);

    free(raw);
}

psx_disc_t* psx_disc_create(void) {
    psx_disc_t* disc = malloc(sizeof(psx_disc_t));

    if (disc)
        memset(disc, 0, sizeof(psx_disc_t));

    return disc;
}

int disc_get_extension(const char* path) {
    char ext[16];
    const char* ptr;
    size_t len;
    int i = 0;

    if (!path || !path[0])
        return CD_EXT_UNSUPPORTED;

    ptr = strrchr(path, '.');
    if (!ptr || !ptr[1])
        return CD_EXT_UNSUPPORTED;

    len = strlen(ptr + 1);
    if (len >= sizeof(ext))
        return CD_EXT_UNSUPPORTED;

    for (size_t index = 0; index < len; ++index)
        ext[index] = (char)tolower((unsigned char)ptr[1 + index]);
    ext[len] = '\0';

    while (disc_cd_extensions[i]) {
        if (!strcmp(ext, disc_cd_extensions[i]))
            return i;
        
        ++i;
    }

    return CD_EXT_UNSUPPORTED;
}

int disc_get_cd_type(psx_disc_t* disc) {
    char buf[CD_SECTOR_SIZE];

    // If the disc is smaller than 16 sectors
    // then it can't be a PlayStation game.
    // Audio discs should also have ISO volume
    // descriptors, so it's probably something else
    // entirely.
    if (!psx_disc_read(disc, MSF_TO_LBA(0, 2, 16), buf))
        return CDT_UNKNOWN;

    // Check for the "PLAYSTATION" string at PVD offset 20h
    // Patch 20 byte so comparison is done correctly
    buf[0x2b] = 0;

    if (strncmp(&buf[0x20], "PLAYSTATION", 12))
        return CDT_AUDIO;

    return CDT_LICENSED;
}

int psx_disc_open(psx_disc_t* disc, const char* path) {
    if (!path)
        return CDT_ERROR;

    int ext = disc_get_extension(path);

    return psx_disc_open_as(disc, path, ext);
}

int psx_disc_open_as(psx_disc_t* disc, const char* path, int type) {
    switch (type) {
        case CD_EXT_CUE: {
            cue_t* cue = cue_create();

            cue_init(cue);
            cue_init_disc(cue, disc);

            if (cue_parse(cue, path))
                return CDT_ERROR;

            if (cue_load(cue, LD_FILE))
                return CDT_ERROR;
        } break;

        case CD_EXT_BIN:
        case CD_EXT_ISO:
        case CD_EXT_IMG: {
            FILE* f = fopen(path, "rb");
            uint32_t sector_size;

            if (!f)
                return CDT_ERROR;

            if (fseek(f, 0, SEEK_END)) {
                fclose(f);
                return CDT_ERROR;
            }

            long long size = ftell(f);

            if (size <= 0 || fseek(f, 0, SEEK_SET)) {
                fclose(f);
                return CDT_ERROR;
            }

            if (type == CD_EXT_BIN) {
                sector_size = CD_SECTOR_SIZE;
            } else if (type == CD_EXT_ISO) {
                sector_size = 2048;
            } else {
                const int is_2048 = (size % 2048) == 0;
                const int is_2352 = (size % CD_SECTOR_SIZE) == 0;

                if (!is_2048 && !is_2352) {
                    fclose(f);
                    return CDT_ERROR;
                }

                if (is_2048 && is_2352) {
                    static const uint8_t sync[12] = {
                        0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                        0xff, 0xff, 0xff, 0xff, 0xff, 0x00
                    };
                    uint8_t header[sizeof(sync)];

                    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
                        fseek(f, 0, SEEK_SET)) {
                        fclose(f);
                        return CDT_ERROR;
                    }

                    sector_size = !memcmp(header, sync, sizeof(sync)) ? CD_SECTOR_SIZE : 2048;
                } else {
                    sector_size = is_2352 ? CD_SECTOR_SIZE : 2048;
                }
            }

            raw_disc_t* raw = (raw_disc_t*)malloc(sizeof(raw_disc_t));

            if (!raw) {
                fclose(f);
                return CDT_ERROR;
            }

            raw->file = f;
            raw->sector_size = sector_size;
            raw->sector_count = (uint32_t)(size / raw->sector_size);

            disc->udata = raw;
            disc->read_sector = raw_read;
            disc->query_sector = raw_query;
            disc->get_track_number = raw_get_track_number;
            disc->get_track_count = raw_get_track_count;
            disc->get_track_lba = raw_get_track_lba;
            disc->destroy = raw_destroy;
        } break;

        case CD_EXT_PBP: {
            pbp_t* pbp = pbp_create();

            pbp_init(pbp);

            if (pbp_load(pbp, path)) {
                pbp_destroy(pbp);

                return CDT_ERROR;
            }

            disc->udata = pbp;
            disc->read_sector = (read_sector_func)pbp_read_sector;
            disc->query_sector = (query_sector_func)pbp_query;
            disc->get_track_number = (get_track_number_func)pbp_get_track_number;
            disc->get_track_count = (get_track_count_func)pbp_get_track_count;
            disc->get_track_lba = (get_track_lba_func)pbp_get_track_lba;
            disc->read_subchannel_q = (read_subchannel_q_func)pbp_read_subchannel_q;
            disc->destroy = (destroy_func)pbp_destroy;
        } break;

#ifdef USE_CHD
        case CD_EXT_CHD: {
            chd_t* chd = chd_create();

            chd_init(chd);

            if (chd_load(chd, path)) {
                chd_destroy(chd);

                return CDT_ERROR;
            }

            disc->udata = chd;
            disc->read_sector = (read_sector_func)chd_read_sector;
            disc->query_sector = (query_sector_func)chd_query;
            disc->get_track_number = (get_track_number_func)chd_get_track_number;
            disc->get_track_count = (get_track_count_func)chd_get_track_count;
            disc->get_track_lba = (get_track_lba_func)chd_get_track_lba;
            disc->read_subchannel_q = (read_subchannel_q_func)chd_read_subchannel_q;
            disc->destroy = (destroy_func)chd_destroy;
        } break;
#endif

        default:
            return CDT_ERROR;
    }

    if (!disc->read_sector) {
        log_error("Unsupported or failed to open disc image: %s", path);
        return CDT_ERROR;
    }

    return disc_get_cd_type(disc);
}

/* Opening may fail before the backend vtable is installed. */
int psx_disc_read(psx_disc_t* disc, uint32_t lba, void* buf) {
    if (!disc || !disc->read_sector)
        return 0;

    /* The single funnel every container goes through (cue/bin, CHD, raw), so one counter
       here is every sector the drive actually pulled off the image. */
    PSX_PERF_INC(cdrom_sectors);

    return disc->read_sector(disc->udata, lba, buf);
}

int psx_disc_query(psx_disc_t* disc, uint32_t lba) {
    if (!disc || !disc->query_sector)
        return TS_FAR;

    return disc->query_sector(disc->udata, lba);
}

int psx_disc_get_track_number(psx_disc_t* disc, uint32_t lba) {
    if (!disc || !disc->get_track_number)
        return 1;

    return disc->get_track_number(disc->udata, lba);
}

int psx_disc_get_track_count(psx_disc_t* disc) {
    if (!disc || !disc->get_track_count)
        return 0;

    return disc->get_track_count(disc->udata);
}

int psx_disc_get_track_lba(psx_disc_t* disc, int track) {
    if (!disc || !disc->get_track_lba)
        return 0;

    return disc->get_track_lba(disc->udata, track);
}

int psx_disc_read_subchannel_q(psx_disc_t* disc, uint32_t lba, uint8_t q[12]) {
    if (!disc || !disc->read_subchannel_q || !q)
        return 0;

    return disc->read_subchannel_q(disc->udata, lba, q);
}

void psx_disc_destroy(psx_disc_t* disc) {
    if (!disc)
        return;

    if (disc->destroy)
        disc->destroy(disc->udata);

    free(disc);
}
