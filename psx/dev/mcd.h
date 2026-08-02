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
void psx_mcd_destroy(psx_mcd_t*);

#endif
