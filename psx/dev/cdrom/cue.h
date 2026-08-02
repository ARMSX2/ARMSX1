#ifndef CUE_H
#define CUE_H

#include "frontend/diagnostics.h"

#include "list.h"
#include "disc.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum {
    CUE_OK = 0,
    CUE_FILE_NOT_FOUND,
    CUE_TRACK_FILE_NOT_FOUND,
    CUE_TRACK_READ_ERROR
};

enum {
    CUE_4CH = 0,
    CUE_AIFF,
    CUE_AUDIO,
    CUE_BINARY,
    CUE_CATALOG,
    CUE_CDG,
    CUE_CDI_2336,
    CUE_CDI_2352,
    CUE_CDTEXTFILE,
    CUE_DCP,
    CUE_FILE,
    CUE_FLAGS,
    CUE_INDEX,
    CUE_ISRC,
    CUE_MODE1_2048,
    CUE_MODE1_2352,
    CUE_MODE2_2336,
    CUE_MODE2_2352,
    CUE_MOTOROLA,
    CUE_MP3,
    CUE_PERFORMER,
    CUE_POSTGAP,
    CUE_PRE,
    CUE_PREGAP,
    CUE_REM,
    CUE_SCMS,
    CUE_SONGWRITER,
    CUE_TITLE,
    CUE_TRACK,
    CUE_WAVE,
    CUE_NONE = 255
};

enum {
    LD_BUFFERED,
    LD_FILE
};

typedef struct {
    char* name;
    int buf_mode;
    void* buf;
    size_t size;
    uint32_t start;
    list_t* tracks;
} cue_file_t;

typedef struct {
    int number;
    int mode;

    int32_t index[2];
    uint32_t pregap;
    uint32_t start;
    uint32_t end;

    cue_file_t* file;
} cue_track_t;

typedef struct {
    list_t* files;
    list_t* tracks;

    char c;
    FILE* file;
} cue_t;

cue_t* cue_create(void);
void cue_init(cue_t* cue);
int cue_parse(cue_t* cue, const char* path);
int cue_load(cue_t* cue, int mode);

// Disc interface
int cue_read(cue_t* cue, uint32_t lba, void* buf);
int cue_query(cue_t* cue, uint32_t lba);
int cue_get_track_number(cue_t* cue, uint32_t lba);
int cue_get_track_count(cue_t* cue);
int cue_get_track_lba(cue_t* cue, int track);
void cue_init_disc(cue_t* cue, psx_disc_t* disc);
void cue_destroy(cue_t* cue);

#endif
