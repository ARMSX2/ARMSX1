#ifdef USE_CHD

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "chd.h"
#include "../../log.h"
#include "libchdr/chd.h"
#include "libchdr/cdrom.h"

#define CHD_CD_FRAME_SIZE   CD_FRAME_SIZE
#define CHD_CD_TRACK_ALIGN  4u
#define CHD_TRACK1_PREGAP_FRAMES  (2u * 75u)

typedef enum {
    CHD_TRACK_MODE_AUDIO = 0,
    CHD_TRACK_MODE_DATA = 1
} chd_track_mode_t;

typedef enum {
    CHD_SUBCHANNEL_NONE = 0,
    CHD_SUBCHANNEL_COOKED,
    CHD_SUBCHANNEL_RAW
} chd_subchannel_mode_t;

typedef struct {
    uint32_t track_number;
    uint32_t start_lba;
    uint64_t file_lba;
    uint32_t pregap_frames;
    uint32_t data_frames;
    uint32_t postgap_frames;
    chd_track_mode_t mode;
    chd_subchannel_mode_t subchannel_mode;
} chd_track_t;

struct chd_s {
    FILE* file;
    chd_file* handle;
    uint8_t* hunk_buffer;
    uint32_t hunk_size;
    uint32_t sectors_per_hunk;
    uint32_t current_hunk_index;
    chd_track_t* tracks;
    size_t track_count;
    size_t track_capacity;
    uint32_t disc_lba_count;
};

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

static void chd_reset(chd_t* chd) {
    if (!chd) {
        return;
    }

    if (chd->handle) {
        chd_close(chd->handle);
        chd->handle = NULL;
    }

    if (chd->file) {
        fclose(chd->file);
        chd->file = NULL;
    }

    free(chd->hunk_buffer);
    chd->hunk_buffer = NULL;

    free(chd->tracks);
    chd->tracks = NULL;
    chd->track_count = 0;
    chd->track_capacity = 0;

    chd->hunk_size = 0;
    chd->sectors_per_hunk = 0;
    chd->current_hunk_index = UINT32_MAX;
    chd->disc_lba_count = 0;
}

static int chd_append_track(chd_t* chd, const chd_track_t* track) {
    if (!chd || !track) {
        return 0;
    }

    if (chd->track_count >= chd->track_capacity) {
        size_t next_capacity = chd->track_capacity ? (chd->track_capacity * 2u) : 8u;
        chd_track_t* next_tracks = (chd_track_t*)realloc(chd->tracks, next_capacity * sizeof(*next_tracks));
        if (!next_tracks) {
            return 0;
        }

        chd->tracks = next_tracks;
        chd->track_capacity = next_capacity;
    }

    chd->tracks[chd->track_count++] = *track;
    return 1;
}

static int chd_parse_track_mode(const char* type_str, chd_track_mode_t* mode_out) {
    if (!type_str || !mode_out) {
        return 0;
    }

    if (strncmp(type_str, "AUDIO", 5) == 0) {
        *mode_out = CHD_TRACK_MODE_AUDIO;
        return 1;
    }

    if (strncmp(type_str, "MODE1", 5) == 0 ||
        strncmp(type_str, "MODE2", 5) == 0 ||
        strncmp(type_str, "MODE1_RAW", 9) == 0 ||
        strncmp(type_str, "MODE2_RAW", 9) == 0 ||
        strncmp(type_str, "MODE2_FORM1", 10) == 0 ||
        strncmp(type_str, "MODE2_FORM2", 10) == 0 ||
        strncmp(type_str, "MODE2_FORM_MIX", 14) == 0) {
        *mode_out = CHD_TRACK_MODE_DATA;
        return 1;
    }

    return 0;
}

static int chd_parse_subchannel_mode(const char* subtype, chd_subchannel_mode_t* mode_out) {
    if (!subtype || !mode_out)
        return 0;

    if (!strcmp(subtype, "NONE")) {
        *mode_out = CHD_SUBCHANNEL_NONE;
        return 1;
    }
    if (!strcmp(subtype, "RW")) {
        *mode_out = CHD_SUBCHANNEL_COOKED;
        return 1;
    }
    if (!strcmp(subtype, "RW_RAW")) {
        *mode_out = CHD_SUBCHANNEL_RAW;
        return 1;
    }
    return 0;
}

static int chd_read_hunk(chd_t* chd, uint32_t hunk_index) {
    if (!chd || !chd->handle || !chd->hunk_buffer) {
        return 0;
    }

    if (chd->current_hunk_index == hunk_index) {
        return 1;
    }

    if (chd_read(chd->handle, hunk_index, chd->hunk_buffer) != CHDERR_NONE) {
        chd->current_hunk_index = UINT32_MAX;
        return 0;
    }

    chd->current_hunk_index = hunk_index;
    return 1;
}

static void chd_swap_audio_sector(uint8_t* sector) {
    for (size_t offset = 0; offset < CD_SECTOR_SIZE; offset += sizeof(uint16_t)) {
        uint16_t value;
        memcpy(&value, &sector[offset], sizeof(value));
        value = (uint16_t)((value << 8) | (value >> 8));
        memcpy(&sector[offset], &value, sizeof(value));
    }
}

static int chd_find_track(const chd_t* chd, uint32_t lba, int* is_pregap_out) {
    if (!chd || !chd->tracks) {
        return -1;
    }

    for (size_t index = 0; index < chd->track_count; ++index) {
        const chd_track_t* track = &chd->tracks[index];
        const uint32_t pregap_start = (track->start_lba > track->pregap_frames)
            ? (track->start_lba - track->pregap_frames)
            : 0u;
        const uint32_t track_end = track->start_lba + track->data_frames;
        const uint32_t postgap_end = track_end + track->postgap_frames;

        if (lba >= pregap_start && lba < track->start_lba) {
            if (is_pregap_out) {
                *is_pregap_out = 1;
            }
            return (int)index;
        }

        if (lba >= track->start_lba && lba < track_end) {
            if (is_pregap_out) {
                *is_pregap_out = 0;
            }
            return (int)index;
        }

        if (lba >= track_end && lba < postgap_end) {
            if (is_pregap_out) {
                *is_pregap_out = 1;
            }
            return (int)index;
        }
    }

    if (is_pregap_out) {
        *is_pregap_out = 0;
    }

    return -1;
}

chd_t* chd_create(void) {
    chd_t* chd = (chd_t*)calloc(1, sizeof(*chd));

    if (chd) {
        chd->current_hunk_index = UINT32_MAX;
    }

    return chd;
}

void chd_init(chd_t* chd) {
    if (!chd) {
        return;
    }

    chd->current_hunk_index = UINT32_MAX;
}

int chd_load(chd_t* chd, const char* path) {
    if (!chd || !path || !path[0]) {
        return 1;
    }

    chd_reset(chd);
    chd_init(chd);

    chd->file = fopen(path, "rb");
    if (!chd->file) {
        log_error("Failed to open CHD file: %s", path);
        chd_reset(chd);
        return 1;
    }

    chd_error err = chd_open_file(chd->file, CHD_OPEN_READ, NULL, &chd->handle);
    if (err != CHDERR_NONE) {
        log_error("Failed to open CHD '%s': %s", path, chd_error_string(err));
        chd_reset(chd);
        return 1;
    }

    const chd_header* header = chd_get_header(chd->handle);
    if (!header || header->hunkbytes == 0 || (header->hunkbytes % CHD_CD_FRAME_SIZE) != 0) {
        log_error("Unsupported CHD hunk size for '%s'", path);
        chd_reset(chd);
        return 1;
    }

    chd->hunk_size = header->hunkbytes;
    chd->sectors_per_hunk = chd->hunk_size / CHD_CD_FRAME_SIZE;
    chd->hunk_buffer = (uint8_t*)malloc(chd->hunk_size);
    if (!chd->hunk_buffer) {
        log_error("Out of memory while opening CHD: %s", path);
        chd_reset(chd);
        return 1;
    }

    uint64_t disc_lba = 0;
    uint64_t file_lba = 0;
    size_t track_index = 0;

    for (;; ++track_index) {
        char metadata[256];
        char type_str[64];
        char subtype_str[64];
        char pgtype_str[64];
        char pgsub_str[64];
        int track_num = 0;
        int frames = 0;
        int pregap_frames = 0;
        int postgap_frames = 0;
        uint32_t metadata_length = 0;
        chd_track_mode_t track_mode = CHD_TRACK_MODE_DATA;
        chd_subchannel_mode_t subchannel_mode = CHD_SUBCHANNEL_NONE;
        int have_metadata = 0;

        memset(metadata, 0, sizeof(metadata));
        memset(type_str, 0, sizeof(type_str));
        memset(subtype_str, 0, sizeof(subtype_str));
        memset(pgtype_str, 0, sizeof(pgtype_str));
        memset(pgsub_str, 0, sizeof(pgsub_str));

        err = chd_get_metadata(chd->handle, CDROM_TRACK_METADATA2_TAG, (uint32_t)track_index,
            metadata, sizeof(metadata) - 1, &metadata_length, NULL, NULL);
        if (err == CHDERR_NONE) {
            have_metadata = 1;
            metadata[sizeof(metadata) - 1] = '\0';
            if (sscanf(metadata, CDROM_TRACK_METADATA2_FORMAT, &track_num, type_str, subtype_str, &frames,
                &pregap_frames, pgtype_str, pgsub_str, &postgap_frames) != 8) {
                log_error("Invalid CHD track metadata: %s", metadata);
                chd_reset(chd);
                return 1;
            }
        } else {
            err = chd_get_metadata(chd->handle, CDROM_TRACK_METADATA_TAG, (uint32_t)track_index,
                metadata, sizeof(metadata) - 1, &metadata_length, NULL, NULL);
            if (err != CHDERR_NONE) {
                break;
            }

            have_metadata = 1;
            metadata[sizeof(metadata) - 1] = '\0';
            if (sscanf(metadata, CDROM_TRACK_METADATA_FORMAT, &track_num, type_str, subtype_str, &frames) != 4) {
                log_error("Invalid CHD track metadata: %s", metadata);
                chd_reset(chd);
                return 1;
            }
        }

        if (!have_metadata) {
            break;
        }

        if (track_num != (int)(track_index + 1u)) {
            log_error("Unexpected CHD track number %d at index %zu in %s", track_num, track_index, path);
            chd_reset(chd);
            return 1;
        }

        if (!chd_parse_track_mode(type_str, &track_mode)) {
            log_error("Unsupported CHD track mode '%s' in %s", type_str, path);
            chd_reset(chd);
            return 1;
        }

        if (!chd_parse_subchannel_mode(subtype_str, &subchannel_mode)) {
            log_error("Unsupported CHD subchannel mode '%s' in %s", subtype_str, path);
            chd_reset(chd);
            return 1;
        }

        if (pregap_frames <= 0 && track_num == 1) {
            pregap_frames = CHD_TRACK1_PREGAP_FRAMES;
        }

        const int pregap_in_file = (pregap_frames > 0 && toupper((unsigned char)pgtype_str[0]) == 'V');
        if (pregap_in_file && frames < pregap_frames) {
            log_error("CHD pregap exceeds track length in %s", path);
            chd_reset(chd);
            return 1;
        }

        if (pregap_frames > 0) {
            disc_lba += (uint32_t)pregap_frames;
            if (pregap_in_file) {
                file_lba += (uint64_t)pregap_frames;
            }
        }

        uint64_t track_file_lba = file_lba;
        uint32_t data_frames = (uint32_t)frames;
        if (pregap_in_file) {
            data_frames = (uint32_t)(frames - pregap_frames);
        }

        if (data_frames == 0) {
            log_error("CHD track %d has no data frames in %s", track_num, path);
            chd_reset(chd);
            return 1;
        }

        chd_track_t track = {
            .track_number = (uint32_t)track_num,
            .start_lba = (uint32_t)disc_lba,
            .file_lba = track_file_lba,
            .pregap_frames = (uint32_t)pregap_frames,
            .data_frames = data_frames,
            .postgap_frames = (uint32_t)(postgap_frames > 0 ? postgap_frames : 0),
            .mode = track_mode,
            .subchannel_mode = subchannel_mode,
        };

        if (!chd_append_track(chd, &track)) {
            log_error("Out of memory while loading CHD tracks from %s", path);
            chd_reset(chd);
            return 1;
        }

        disc_lba += data_frames;
        if (postgap_frames > 0) {
            disc_lba += (uint32_t)postgap_frames;
        }
        file_lba += data_frames;
        file_lba = align_up_u64(file_lba, CHD_CD_TRACK_ALIGN);
    }

    if (chd->track_count == 0) {
        log_error("No CHD tracks found in %s", path);
        chd_reset(chd);
        return 1;
    }

    chd->disc_lba_count = (uint32_t)disc_lba;
    chd->current_hunk_index = UINT32_MAX;

    return 0;
}

int chd_read_sector(chd_t* chd, uint32_t lba, void* buf) {
    if (!chd || !buf || !chd->handle || !chd->hunk_buffer) {
        return 0;
    }

    int is_pregap = 0;
    const int track_index = chd_find_track(chd, lba, &is_pregap);
    if (track_index < 0) {
        if (lba < chd->disc_lba_count) {
            memset(buf, 0, CD_SECTOR_SIZE);
            memset((uint8_t*)buf + 1, 0xff, 10);
            return TS_PREGAP;
        }

        return TS_FAR;
    }

    const chd_track_t* track = &chd->tracks[track_index];
    if (is_pregap) {
        memset(buf, 0, CD_SECTOR_SIZE);
        memset((uint8_t*)buf + 1, 0xff, 10);
        return TS_PREGAP;
    }

    const uint64_t file_frame = track->file_lba + (uint64_t)(lba - track->start_lba);
    const uint32_t hunk_index = (uint32_t)(file_frame / chd->sectors_per_hunk);
    const uint32_t hunk_offset = (uint32_t)((file_frame % chd->sectors_per_hunk) * CHD_CD_FRAME_SIZE);

    if ((hunk_offset + CHD_CD_FRAME_SIZE) > chd->hunk_size) {
        return 0;
    }

    if (!chd_read_hunk(chd, hunk_index)) {
        return 0;
    }

    uint8_t sector[CD_SECTOR_SIZE];
    memcpy(sector, &chd->hunk_buffer[hunk_offset], CD_SECTOR_SIZE);

    if (track->mode == CHD_TRACK_MODE_AUDIO) {
        chd_swap_audio_sector(sector);
        memcpy(buf, sector, CD_SECTOR_SIZE);
        return TS_AUDIO;
    }

    memcpy(buf, sector, CD_SECTOR_SIZE);
    return TS_DATA;
}

int chd_query(chd_t* chd, uint32_t lba) {
    if (!chd || !chd->handle) {
        return TS_FAR;
    }

    int is_pregap = 0;
    const int track_index = chd_find_track(chd, lba, &is_pregap);
    if (track_index < 0) {
        return (lba < chd->disc_lba_count) ? TS_PREGAP : TS_FAR;
    }

    if (is_pregap) {
        return TS_PREGAP;
    }

    return (chd->tracks[track_index].mode == CHD_TRACK_MODE_AUDIO) ? TS_AUDIO : TS_DATA;
}

int chd_get_track_number(chd_t* chd, uint32_t lba) {
    if (!chd || !chd->handle) {
        return 0;
    }

    int is_pregap = 0;
    const int track_index = chd_find_track(chd, lba, &is_pregap);
    if (track_index < 0) {
        return 0;
    }

    return (int)chd->tracks[track_index].track_number;
}

int chd_get_track_count(chd_t* chd) {
    return chd ? (int)chd->track_count : 0;
}

int chd_get_track_lba(chd_t* chd, int track) {
    if (!chd || !chd->handle) {
        return 0;
    }

    if (track == 0) {
        return (int)chd->disc_lba_count;
    }

    if (track < 0 || (size_t)track > chd->track_count) {
        return TS_FAR;
    }

    return (int)chd->tracks[(size_t)track - 1u].start_lba;
}

static void chd_deinterleave_subchannel(const uint8_t raw[96], uint8_t cooked[96]) {
    memset(cooked, 0, 96);
    for (unsigned symbol = 0; symbol < 96; ++symbol) {
        for (unsigned channel = 0; channel < 8; ++channel) {
            const uint8_t bit = (uint8_t)((raw[symbol] >> (7u - channel)) & 1u);
            cooked[channel * 12u + symbol / 8u] |= (uint8_t)(bit << (7u - (symbol & 7u)));
        }
    }
}

int chd_read_subchannel_q(chd_t* chd, uint32_t lba, uint8_t q[12]) {
    if (!chd || !q || !chd->handle || !chd->hunk_buffer)
        return 0;

    int is_gap = 0;
    const int track_index = chd_find_track(chd, lba, &is_gap);
    if (track_index < 0 || is_gap)
        return 0;

    const chd_track_t* track = &chd->tracks[track_index];
    if (track->subchannel_mode == CHD_SUBCHANNEL_NONE)
        return 0;

    const uint64_t file_frame = track->file_lba + (uint64_t)(lba - track->start_lba);
    const uint32_t hunk_index = (uint32_t)(file_frame / chd->sectors_per_hunk);
    const uint32_t hunk_offset = (uint32_t)((file_frame % chd->sectors_per_hunk) * CHD_CD_FRAME_SIZE);
    if ((hunk_offset + CHD_CD_FRAME_SIZE) > chd->hunk_size || !chd_read_hunk(chd, hunk_index))
        return 0;

    const uint8_t* subchannel = &chd->hunk_buffer[hunk_offset + CD_SECTOR_SIZE];
    if (track->subchannel_mode == CHD_SUBCHANNEL_COOKED) {
        memcpy(q, subchannel + 12, 12);
    } else {
        uint8_t cooked[96];
        chd_deinterleave_subchannel(subchannel, cooked);
        memcpy(q, cooked + 12, 12);
    }
    return 1;
}

void chd_destroy(chd_t* chd) {
    if (!chd) {
        return;
    }

    chd_reset(chd);
    free(chd);
}

#endif
