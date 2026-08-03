#ifndef PBP_H
#define PBP_H

#include "disc.h"

/*
    PSP EBOOT (.PBP) containers holding PlayStation 1 discs.

    Layout, as verified against a real multi-disc EBOOT (Gran Turismo 2, SCES-02380):

      PBP file
        0x00  magic "\0PBP"
        0x24  u32 offset of DATA.PSAR

      DATA.PSAR
        "PSTITLEIMG000000"  multi-disc: +0x200 holds 5 u32 disc offsets (relative to
                            the PSAR), zero-terminated.
        "PSISOIMG0000"      single-disc: the PSAR *is* the disc.

      each disc
        +0x00000   "PSISOIMG0000"
        +0x0000C   u32 uncompressed disc size in bytes
        +0x00400   NUL-terminated ASCII serial, "_SCES_02380" style
        +0x00800   CD TOC, 10 bytes per entry, subchannel-Q shaped (BCD)
        +0x04000   block index, 32 bytes per entry: u32 offset, u32 size, then padding
        +0x100000  block data. Each block is RAW DEFLATE (no zlib header) and inflates
                   to exactly 16 sectors * 2352 = 37632 bytes. A block whose stored size
                   is already 37632 is held verbatim rather than compressed.

    The index offset is relative to the 0x100000 data base, NOT to the disc header — that
    distinction is the one that silently yields garbage if you get it wrong, because block 0
    at "offset 0" then lands on the PSISOIMG magic and inflates to nothing.
*/

typedef struct pbp_s pbp_t;

pbp_t* pbp_create(void);
void pbp_init(pbp_t* pbp);
int pbp_load(pbp_t* pbp, const char* path);

/* Number of discs in the container (1 for a plain PSISOIMG), and which one is mounted. */
int pbp_get_disc_count(pbp_t* pbp);
int pbp_set_disc(pbp_t* pbp, int index);

/* The mounted disc's serial as it appears at +0x400, normalised to "SCES-02380".
   Empty string when the container did not carry one. */
const char* pbp_get_serial(pbp_t* pbp);

/* Disc interface */
int pbp_read_sector(pbp_t* pbp, uint32_t lba, void* buf);
int pbp_query(pbp_t* pbp, uint32_t lba);
int pbp_get_track_number(pbp_t* pbp, uint32_t lba);
int pbp_get_track_count(pbp_t* pbp);
uint32_t pbp_get_track_lba(pbp_t* pbp, int track);
int pbp_read_subchannel_q(pbp_t* pbp, uint32_t lba, uint8_t q[12]);
void pbp_destroy(pbp_t* pbp);

#endif
