#ifndef DISC_H
#define DISC_H

#include <stdint.h>

/*
    PSX disc reader API version 2 specification:

    Mandatory formats:
    - BIN/CUE (Multi track)
    - BIN (Single track)

    Optional (but encouraged) formats:
    - ISO (Raw ISO9660 images)
    - CHD (Compressed MAME "Hunks of Data")

    Optional formats:
    - MDS/MDF (Alcohol 120% images)
*/

enum {
    TS_FAR = 0,
    TS_DATA,
    TS_AUDIO,
    TS_PREGAP
};

enum {
    CD_EXT_CUE = 0,
    CD_EXT_BIN,
    CD_EXT_ISO,
#ifdef USE_CHD
    CD_EXT_CHD,
#endif
    CD_EXT_UNSUPPORTED
};

enum {
    CDT_ERROR = 0,
    CDT_LICENSED,
    CDT_AUDIO,
    CDT_UNKNOWN
};

#define CD_SECTOR_SIZE 2352

typedef int (*read_sector_func)(void*, uint32_t, void*);
typedef int (*query_sector_func)(void*, uint32_t);
typedef int (*get_track_number_func)(void*, uint32_t);
typedef int (*get_track_count_func)(void*);
typedef uint32_t (*get_track_lba_func)(void*, int);
typedef int (*read_subchannel_q_func)(void*, uint32_t, uint8_t*);
typedef void (*destroy_func)(void*);

typedef struct {
    void* udata;
    read_sector_func read_sector;
    query_sector_func query_sector;
    get_track_number_func get_track_number;
    get_track_count_func get_track_count;
    get_track_lba_func get_track_lba;
    read_subchannel_q_func read_subchannel_q;
    destroy_func destroy;
} psx_disc_t;

psx_disc_t* psx_disc_create(void);
int psx_disc_open(psx_disc_t* disc, const char* path);
int psx_disc_open_as(psx_disc_t* disc, const char* path, int type);
int psx_disc_read(psx_disc_t* disc, uint32_t lba, void* buf);
int psx_disc_query(psx_disc_t* disc, uint32_t lba);
int psx_disc_get_track_number(psx_disc_t* disc, uint32_t lba);
int psx_disc_get_track_count(psx_disc_t* disc);
int psx_disc_get_track_lba(psx_disc_t* disc, int track);
int psx_disc_read_subchannel_q(psx_disc_t* disc, uint32_t lba, uint8_t q[12]);
void psx_disc_close(psx_disc_t* disc);
void psx_disc_destroy(psx_disc_t* disc);

#endif
