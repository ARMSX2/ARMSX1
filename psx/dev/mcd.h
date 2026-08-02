#ifndef MCD_H
#define MCD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../state.h"

#define MCD_MEMORY_SIZE 0x20000 // 128 KB

enum {
    MCD_STATE_TX_HIZ = 0,
    MCD_STATE_TX_FLG,
    MCD_STATE_TX_ID1,
    MCD_STATE_TX_ID2,
    MCD_R_STATE_RX_MSB,
    MCD_R_STATE_RX_LSB,
    MCD_R_STATE_TX_ACK1,
    MCD_R_STATE_TX_ACK2,
    MCD_R_STATE_TX_MSB,
    MCD_R_STATE_TX_LSB,
    MCD_R_STATE_TX_DATA,
    MCD_R_STATE_TX_CHK,
    MCD_R_STATE_TX_MEB,
    MCD_W_STATE_RX_MSB,
    MCD_W_STATE_RX_LSB,
    MCD_W_STATE_RX_DATA,
    MCD_W_STATE_RX_CHK,
    MCD_W_STATE_RX_CHK2,
    MCD_W_STATE_TX_ACK1,
    MCD_W_STATE_TX_ACK2,
    MCD_W_STATE_TX_MEB,
    MCD_S_STATE_TX_ACK1,
    MCD_S_STATE_TX_ACK2,
    MCD_S_STATE_TX_DAT0,
    MCD_S_STATE_TX_DAT1,
    MCD_S_STATE_TX_DAT2,
    MCD_S_STATE_TX_DAT3
};

typedef struct {
    char* path; /* OWNED (strdup'd in psx_mcd_init) — never a borrowed pointer. */
    uint8_t* buf;
    uint8_t flag;
    uint16_t msb;
    uint16_t lsb;
    uint16_t addr;
    uint8_t rx_data;
    int pending_bytes;
    char mode;
    int state;
    uint8_t tx_data;
    int tx_data_ready;
    uint8_t checksum;

    /*
        Divergence tracking — see psx_mcd_content_hash() below and the
        PSX_SS_MCARD section in state.h.

        NONE of these are written into the mcd save-state payload. They are host
        bookkeeping about the card, not emulated card state, and adding them to
        psx_mcd_save_state() would change an EXISTING section's payload and so
        force a PSX_STATE_CORE_ABI bump, invalidating every state already on a
        user's device. They travel in their own optional section instead.
    */
    uint64_t session_id;       /* nonce per attach; makes generations comparable */
    uint32_t write_generation; /* ++ per 128-byte sector the game writes */
    int hash_valid;            /* hash_cached is up to date with buf */
    uint64_t hash_cached;
} psx_mcd_t;

psx_mcd_t* psx_mcd_create(void);
int psx_mcd_init(psx_mcd_t*, const char*);
uint8_t psx_mcd_read(psx_mcd_t*);
void psx_mcd_write(psx_mcd_t*, uint8_t);
int psx_mcd_query(psx_mcd_t*);
void psx_mcd_reset(psx_mcd_t*);
/* Transfer state machine + the 128 KiB card image. mcd->path is a borrowed
   host pointer and is not saved. See the comment in mcd.c for why the image is
   part of the state and how to opt out. */
void psx_mcd_save_state(psx_mcd_t*, psx_state_writer_t*);
int psx_mcd_load_state(psx_mcd_t*, psx_state_reader_t*);
void psx_mcd_set_state_restores_image(int);

/* --------------------------------------------------------------------------
   Read-only fingerprint accessors.

   Used by the save-state divergence check (psx/state.c) to tell "the card is
   exactly what this state remembers" from "the game has saved to the card
   since". They only READ the card; none of them touches mcd->path ownership or
   the file write path.

   psx_mcd_content_hash() is lazy: the 128 KiB FNV-1a is recomputed only after a
   write has invalidated it, so calling it once per state save costs nothing on
   a card the game has not touched.
   -------------------------------------------------------------------------- */
uint64_t psx_mcd_content_hash(psx_mcd_t*);
uint32_t psx_mcd_write_generation(const psx_mcd_t*);
uint64_t psx_mcd_session_id(const psx_mcd_t*);
/* Live stat() of the card file, or 0 when it has no path / does not exist. A
   secondary, cross-session direction hint: the image is only flushed to disk by
   psx_mcd_destroy(), so this does NOT move when the game saves mid-session. */
int64_t psx_mcd_file_mtime(const psx_mcd_t*);

void psx_mcd_destroy(psx_mcd_t*);

#endif
