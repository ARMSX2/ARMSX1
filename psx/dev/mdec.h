#ifndef MDEC_H
#define MDEC_H

#include <stdint.h>

#include "../log.h"
#include "../state.h"

#define PSX_MDEC_SIZE    0x8
#define PSX_MDEC_BEGIN   0x1f801820
#define PSX_MDEC_END     0x1f801827

#define MDEC_SCALE_TABLE_SIZE 64
#define MDEC_QUANT_TABLE_SIZE 64

enum {
    MDEC_RECV_CMD,
    MDEC_RECV_BLOCK,
    MDEC_RECV_QUANT,
    MDEC_RECV_QUANT_COLOR,
    MDEC_RECV_SCALE
};

/*
  31    Data-Out Fifo Empty (0=No, 1=Empty)
  30    Data-In Fifo Full   (0=No, 1=Full, or Last word received)
  29    Command Busy  (0=Ready, 1=Busy receiving or processing parameters)
  28    Data-In Request  (set when DMA0 enabled and ready to receive data)
  27    Data-Out Request (set when DMA1 enabled and ready to send data)
  26-25 Data Output Depth  (0=4bit, 1=8bit, 2=24bit, 3=15bit)      ;CMD.28-27
  24    Data Output Signed (0=Unsigned, 1=Signed)                  ;CMD.26
  23    Data Output Bit15  (0=Clear, 1=Set) (for 15bit depth only) ;CMD.25
  22-19 Not used (seems to be always zero)
  18-16 Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always 4=Y)
  15-0  Number of Parameter Words remaining minus 1  (FFFFh=None)  ;CMD.Bit0-15
*/

enum {
    MDEC_CMD_NOP,
    MDEC_CMD_DECODE,
    MDEC_CMD_SET_QT,
    MDEC_CMD_SET_ST
};

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint32_t cmd;

    int state;
    int data_remaining;
    int index;

    uint32_t* input;
    int input_index;
    size_t input_size;

    uint8_t* output;
    int output_index;
    uint32_t output_words_remaining;

    uint32_t words_remaining;
    int current_block;
    int output_bit15;
    int output_signed;
    int output_depth;
    int input_request;
    int output_request;
    int busy;
    int input_full;
    int output_empty;

    int enable_dma0;
    int enable_dma1;

    int recv_color;
    uint8_t uv_quant_table[MDEC_QUANT_TABLE_SIZE];
    uint8_t y_quant_table[MDEC_QUANT_TABLE_SIZE];
    int16_t scale_table[MDEC_SCALE_TABLE_SIZE];

    int16_t yblk[64];
    int16_t crblk[64];
    int16_t cbblk[64];

    uint32_t status;
} psx_mdec_t;

psx_mdec_t* psx_mdec_create(void);
void psx_mdec_init(psx_mdec_t*);
uint32_t psx_mdec_read32(psx_mdec_t*, uint32_t);
uint16_t psx_mdec_read16(psx_mdec_t*, uint32_t);
uint8_t psx_mdec_read8(psx_mdec_t*, uint32_t);
void psx_mdec_write32(psx_mdec_t*, uint32_t, uint32_t);
void psx_mdec_write16(psx_mdec_t*, uint32_t, uint16_t);
void psx_mdec_write8(psx_mdec_t*, uint32_t, uint8_t);
/* Command/decoder state, the quant + scale tables, the three coefficient
   blocks, and the live portions of the heap-allocated input/output FIFOs (see
   the long comment in mdec.c — neither buffer tracks its own size). */
void psx_mdec_save_state(psx_mdec_t*, psx_state_writer_t*);
int psx_mdec_load_state(psx_mdec_t*, psx_state_reader_t*);
void psx_mdec_destroy(psx_mdec_t*);

typedef void (*mdec_fn_t)(psx_mdec_t*);

#endif