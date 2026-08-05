#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pbp.h"
#include "../../log.h"

#include "miniz.h"

#define PBP_BLOCK_SECTORS 16
#define PBP_BLOCK_BYTES   (PBP_BLOCK_SECTORS * CD_SECTOR_SIZE)   /* 37632 */
#define PBP_INDEX_OFFSET  0x4000
#define PBP_DATA_OFFSET   0x100000
#define PBP_TOC_OFFSET    0x800
#define PBP_SERIAL_OFFSET 0x400
#define PBP_MAX_DISCS     5
#define PBP_MAX_TRACKS    100
#define PBP_NO_BLOCK      0xffffffffu

typedef struct {
    uint32_t offset;
    uint32_t size;
} pbp_index_t;

struct pbp_s {
    FILE* file;

    int disc_count;
    uint32_t disc_offset[PBP_MAX_DISCS];   /* absolute file offsets of each PSISOIMG */
    int disc_index;

    uint32_t iso_base;                     /* absolute offset of the mounted PSISOIMG */
    uint32_t data_base;                    /* iso_base + PBP_DATA_OFFSET */
    pbp_index_t* index;
    uint32_t block_count;
    uint32_t sector_count;

    uint8_t block[PBP_BLOCK_BYTES];
    uint8_t scratch[PBP_BLOCK_BYTES * 2];  /* compressed source is never larger in practice */
    uint32_t cached_block;

    int track_count;
    uint32_t track_lba[PBP_MAX_TRACKS + 1];
    int track_audio[PBP_MAX_TRACKS + 1];
    uint32_t lead_out_lba;

    char serial[16];
};

static uint32_t pbp_rd32(FILE* f, long offset) {
    uint8_t b[4];

    if (fseek(f, offset, SEEK_SET))
        return 0;

    if (fread(b, 1, 4, f) != 4)
        return 0;

    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int pbp_from_bcd(uint8_t v) {
    return ((v >> 4) & 0xf) * 10 + (v & 0xf);
}

/* The core's disc LBA space is absolute: image sector 0 sits at LBA 150 (MSF 00:02:00),
   exactly like cue.c and chd.c. Track tables are kept in that space; only the block fetch
   converts down to PSISOIMG image sectors. */
#define PBP_LBA_BIAS 150u

static uint32_t pbp_msf_to_lba(uint8_t m, uint8_t s, uint8_t f) {
    /* TOC positions are absolute MSF, which is the disc LBA space directly. */
    int mm = pbp_from_bcd(m), ss = pbp_from_bcd(s), ff = pbp_from_bcd(f);

    return (uint32_t)((mm * 60 + ss) * 75 + ff);
}

pbp_t* pbp_create(void) {
    return (pbp_t*)malloc(sizeof(pbp_t));
}

void pbp_init(pbp_t* pbp) {
    if (!pbp)
        return;

    memset(pbp, 0, sizeof(pbp_t));

    pbp->cached_block = PBP_NO_BLOCK;
    pbp->disc_index = -1;
}

/* Read the TOC at iso_base + 0x800 into track tables. Entries are 10 bytes and shaped like
   subchannel Q: control/adr, TNO, POINT, then MSF, a zero, then PMIN/PSEC/PFRAME in BCD.
   POINT A0/A1/A2 carry first track, last track and the lead-out; anything else is a track. */
static void pbp_read_toc(pbp_t* pbp) {
    uint8_t entry[10];
    int first = 1, last = 1;

    pbp->track_count = 0;
    pbp->lead_out_lba = 0;

    if (fseek(pbp->file, (long)(pbp->iso_base + PBP_TOC_OFFSET), SEEK_SET))
        return;

    for (int i = 0; i < PBP_MAX_TRACKS + 3; i++) {
        if (fread(entry, 1, sizeof(entry), pbp->file) != sizeof(entry))
            break;

        /* A zeroed row terminates the table. */
        int empty = 1;

        for (size_t b = 0; b < sizeof(entry); b++)
            if (entry[b]) { empty = 0; break; }

        if (empty)
            break;

        const int control = (entry[0] >> 4) & 0xf;
        const int point = entry[2];
        const uint32_t lba = pbp_msf_to_lba(entry[7], entry[8], entry[9]);

        if (point == 0xa0) {
            first = pbp_from_bcd(entry[7]);
        } else if (point == 0xa1) {
            last = pbp_from_bcd(entry[7]);
        } else if (point == 0xa2) {
            pbp->lead_out_lba = lba;
        } else {
            const int track = pbp_from_bcd((uint8_t)point);

            if (track >= 1 && track <= PBP_MAX_TRACKS) {
                pbp->track_lba[track] = lba;
                /* Control bit 2 set = data track; clear = audio. */
                pbp->track_audio[track] = (control & 0x4) ? 0 : 1;

                if (track > pbp->track_count)
                    pbp->track_count = track;
            }
        }
    }

    if (pbp->track_count < last)
        pbp->track_count = last;

    if (pbp->track_count < 1) {
        /* No usable TOC: present the whole image as one data track, which is what a
           single-track rip is anyway. */
        pbp->track_count = 1;
        pbp->track_lba[1] = PBP_LBA_BIAS;
        pbp->track_audio[1] = 0;
    }

    if (!pbp->lead_out_lba)
        pbp->lead_out_lba = pbp->sector_count + PBP_LBA_BIAS;

    (void)first;
}

static void pbp_read_serial(pbp_t* pbp) {
    char raw[16];

    pbp->serial[0] = '\0';

    if (fseek(pbp->file, (long)(pbp->iso_base + PBP_SERIAL_OFFSET), SEEK_SET))
        return;

    if (fread(raw, 1, sizeof(raw) - 1, pbp->file) != sizeof(raw) - 1)
        return;

    raw[sizeof(raw) - 1] = '\0';

    /* Stored as "_SCES_02380"; the world writes it "SCES-02380". */
    const char* src = raw;

    while (*src == '_')
        src++;

    size_t out = 0;

    for (size_t i = 0; src[i] && out < sizeof(pbp->serial) - 1; i++) {
        char c = src[i];

        if (c == '_' || c == '.')
            c = '-';

        if (c == ' ')
            continue;

        pbp->serial[out++] = c;
    }

    pbp->serial[out] = '\0';
}

static int pbp_mount(pbp_t* pbp, int index) {
    if (index < 0 || index >= pbp->disc_count)
        return 1;

    pbp->iso_base = pbp->disc_offset[index];
    pbp->data_base = pbp->iso_base + PBP_DATA_OFFSET;
    pbp->disc_index = index;
    pbp->cached_block = PBP_NO_BLOCK;

    const uint32_t iso_size = pbp_rd32(pbp->file, (long)(pbp->iso_base + 0x0c));

    pbp->sector_count = iso_size / CD_SECTOR_SIZE;
    pbp->block_count = (pbp->sector_count + PBP_BLOCK_SECTORS - 1) / PBP_BLOCK_SECTORS;

    if (!pbp->block_count) {
        log_error("PBP: disc %d reports a zero-length image", index);

        return 1;
    }

    free(pbp->index);
    pbp->index = (pbp_index_t*)malloc(sizeof(pbp_index_t) * pbp->block_count);

    if (!pbp->index)
        return 1;

    if (fseek(pbp->file, (long)(pbp->iso_base + PBP_INDEX_OFFSET), SEEK_SET)) {
        free(pbp->index);
        pbp->index = NULL;

        return 1;
    }

    for (uint32_t i = 0; i < pbp->block_count; i++) {
        uint8_t e[32];

        if (fread(e, 1, sizeof(e), pbp->file) != sizeof(e)) {
            /* A short index is not fatal — clamp to what we actually have. */
            pbp->block_count = i;
            break;
        }

        pbp->index[i].offset = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                               ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
        pbp->index[i].size   = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
                               ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
    }

    if (!pbp->block_count)
        return 1;

    pbp_read_toc(pbp);
    pbp_read_serial(pbp);

    log_info("PBP: disc %d/%d serial=%s sectors=%u blocks=%u tracks=%d",
             index + 1, pbp->disc_count,
             pbp->serial[0] ? pbp->serial : "(none)",
             pbp->sector_count, pbp->block_count, pbp->track_count);

    return 0;
}

int pbp_load(pbp_t* pbp, const char* path) {
    if (!pbp || !path)
        return 1;

    pbp->file = fopen(path, "rb");

    if (!pbp->file) {
        log_error("PBP: cannot open '%s'", path);

        return 1;
    }

    uint8_t magic[4];

    if (fread(magic, 1, 4, pbp->file) != 4 || memcmp(magic, "\0PBP", 4)) {
        log_error("PBP: '%s' is not a PBP container", path);

        return 1;
    }

    const uint32_t psar = pbp_rd32(pbp->file, 0x24);

    if (!psar) {
        log_error("PBP: '%s' has no DATA.PSAR", path);

        return 1;
    }

    char psar_magic[17];

    if (fseek(pbp->file, (long)psar, SEEK_SET) ||
        fread(psar_magic, 1, 16, pbp->file) != 16) {
        log_error("PBP: '%s' PSAR is unreadable", path);

        return 1;
    }

    psar_magic[16] = '\0';

    if (!memcmp(psar_magic, "PSISOIMG0000", 12)) {
        pbp->disc_count = 1;
        pbp->disc_offset[0] = psar;
    } else if (!memcmp(psar_magic, "PSTITLEIMG000000", 16)) {
        /* Multi-disc: five u32 offsets relative to the PSAR, zero-terminated. */
        pbp->disc_count = 0;

        for (int i = 0; i < PBP_MAX_DISCS; i++) {
            const uint32_t rel = pbp_rd32(pbp->file, (long)(psar + 0x200 + i * 4));

            if (!rel)
                break;

            char m[12];

            if (fseek(pbp->file, (long)(psar + rel), SEEK_SET) ||
                fread(m, 1, sizeof(m), pbp->file) != sizeof(m) ||
                memcmp(m, "PSISOIMG0000", sizeof(m)))
                break;

            pbp->disc_offset[pbp->disc_count++] = psar + rel;
        }

        if (!pbp->disc_count) {
            log_error("PBP: '%s' is a multi-disc container with no readable discs", path);

            return 1;
        }
    } else {
        log_error("PBP: '%s' PSAR is '%s', which is not a PlayStation disc image", path, psar_magic);

        return 1;
    }

    return pbp_mount(pbp, 0);
}

int pbp_get_disc_count(pbp_t* pbp) {
    return pbp ? pbp->disc_count : 0;
}

int pbp_set_disc(pbp_t* pbp, int index) {
    if (!pbp)
        return 1;

    if (index == pbp->disc_index)
        return 0;

    return pbp_mount(pbp, index);
}

const char* pbp_get_serial(pbp_t* pbp) {
    return pbp ? pbp->serial : "";
}

/* Inflate the block holding [lba] into pbp->block. Blocks are cached one deep, which is all a
   sequential read needs — 16 sectors is 16 consecutive hits per decompress. */
static int pbp_cache_block(pbp_t* pbp, uint32_t block) {
    if (block == pbp->cached_block)
        return 0;

    if (block >= pbp->block_count)
        return 1;

    const pbp_index_t* e = &pbp->index[block];

    if (!e->size || e->size > sizeof(pbp->scratch))
        return 1;

    if (fseek(pbp->file, (long)(pbp->data_base + e->offset), SEEK_SET))
        return 1;

    if (fread(pbp->scratch, 1, e->size, pbp->file) != e->size)
        return 1;

    if (e->size == PBP_BLOCK_BYTES) {
        /* Stored verbatim: an incompressible block is written raw rather than deflated. */
        memcpy(pbp->block, pbp->scratch, PBP_BLOCK_BYTES);
    } else {
        /* RAW deflate — no zlib wrapper, so flags = 0 (not TINFL_FLAG_PARSE_ZLIB_HEADER). */
        const size_t out = tinfl_decompress_mem_to_mem(
            pbp->block, PBP_BLOCK_BYTES, pbp->scratch, e->size, 0);

        if (out != PBP_BLOCK_BYTES) {
            log_error("PBP: block %u inflated to %zu bytes, expected %d",
                      block, out, PBP_BLOCK_BYTES);

            return 1;
        }
    }

    pbp->cached_block = block;

    return 0;
}

/* Returns the track-type TS_* on success and 0 (TS_FAR) on failure, the same contract as
   cue_read/chd_read_sector — disc.c's type probe treats 0 as "unreadable", and the
   achievements hasher requires a strict TS_DATA. */
int pbp_read_sector(pbp_t* pbp, uint32_t lba, void* buf) {
    if (!pbp || !buf || lba >= pbp->sector_count + PBP_LBA_BIAS)
        return TS_FAR;

    if (lba < PBP_LBA_BIAS) {
        /* The track-1 pregap is not stored in a PSISOIMG. Hand back an empty raw sector
           with a sync header, the shape cue/chd return for unstored pregap. */
        memset(buf, 0, CD_SECTOR_SIZE);
        memset((uint8_t*)buf + 1, 0xff, 10);
        return TS_PREGAP;
    }

    const uint32_t img = lba - PBP_LBA_BIAS;
    const uint32_t block = img / PBP_BLOCK_SECTORS;
    const uint32_t within = img % PBP_BLOCK_SECTORS;

    if (pbp_cache_block(pbp, block))
        return TS_FAR;

    memcpy(buf, pbp->block + within * CD_SECTOR_SIZE, CD_SECTOR_SIZE);

    return pbp->track_audio[pbp_get_track_number(pbp, lba)] ? TS_AUDIO : TS_DATA;
}

int pbp_query(pbp_t* pbp, uint32_t lba) {
    if (!pbp || lba >= pbp->sector_count + PBP_LBA_BIAS)
        return TS_FAR;

    if (lba < PBP_LBA_BIAS)
        return TS_PREGAP;

    const int track = pbp_get_track_number(pbp, lba);

    if (track >= 1 && track <= pbp->track_count && pbp->track_audio[track])
        return TS_AUDIO;

    return TS_DATA;
}

int pbp_get_track_number(pbp_t* pbp, uint32_t lba) {
    if (!pbp)
        return 1;

    int found = 1;

    for (int t = 1; t <= pbp->track_count; t++) {
        if (lba >= pbp->track_lba[t])
            found = t;
    }

    return found;
}

int pbp_get_track_count(pbp_t* pbp) {
    return pbp ? pbp->track_count : 0;
}

uint32_t pbp_get_track_lba(pbp_t* pbp, int track) {
    if (!pbp)
        return 0;

    if (track < 1 || track > pbp->track_count)
        return pbp->lead_out_lba;

    return pbp->track_lba[track];
}

/* Returns 1 on success and 0 on failure, matching chd_read_subchannel_q — impl.c treats a
   nonzero return as a filled-in q. */
int pbp_read_subchannel_q(pbp_t* pbp, uint32_t lba, uint8_t q[12]) {
    if (!pbp || !q)
        return 0;

    const int track = pbp_get_track_number(pbp, lba);
    const uint32_t start = pbp_get_track_lba(pbp, track);
    const uint32_t rel = lba >= start ? lba - start : 0;

    memset(q, 0, 12);

    q[0] = pbp->track_audio[track] ? 0x01 : 0x41;   /* adr=1, data control for data tracks */
    q[1] = (uint8_t)(((track / 10) << 4) | (track % 10));
    q[2] = 0x01;

    /* lba is absolute (image sector 0 = LBA 150), so it IS the absolute MSF frame count. */
    const uint32_t rm = rel / (60 * 75), rs = (rel / 75) % 60, rf = rel % 75;
    const uint32_t am = lba / (60 * 75), as = (lba / 75) % 60, af = lba % 75;

    q[3] = (uint8_t)(((rm / 10) << 4) | (rm % 10));
    q[4] = (uint8_t)(((rs / 10) << 4) | (rs % 10));
    q[5] = (uint8_t)(((rf / 10) << 4) | (rf % 10));
    q[7] = (uint8_t)(((am / 10) << 4) | (am % 10));
    q[8] = (uint8_t)(((as / 10) << 4) | (as % 10));
    q[9] = (uint8_t)(((af / 10) << 4) | (af % 10));

    return 1;
}

void pbp_destroy(pbp_t* pbp) {
    if (!pbp)
        return;

    if (pbp->file)
        fclose(pbp->file);

    free(pbp->index);
    free(pbp);
}
