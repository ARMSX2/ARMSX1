#ifndef EXP2_H
#define EXP2_H

#include <stdint.h>

#include "../state.h"

#define PSX_EXP2_BEGIN 0x1f802000
#define PSX_EXP2_SIZE  0x1fe000
#define PSX_EXP2_END   0x1f9fffff

#define EXP2_DTL_ATC_STAT  0x00 // 1f802000
#define EXP2_DTL_ATC_DATA  0x02 // 1f802002
#define EXP2_DTL_HDATA     0x04 // 1f802004
#define EXP2_DTL_SEC_IRQ10 0x30 // 1f802030
#define EXP2_DTL_IRQ_CTRL  0x32 // 1f802032
#define EXP2_DTL_BOOT_DIP  0x40 // 1f802040
#define EXP2_POST          0x41 // 1f802041
#define EXP2_LED           0x42 // 1f802042
#define EXP2_POST2         0x70 // 1f802070

typedef void (*exp2_tty_tx)(void*, uint8_t);

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    void* duart_udata;
    void* atcons_udata;

    exp2_tty_tx duart_tx;
    exp2_tty_tx atcons_tx;

    uint8_t atc_stat;
    uint8_t atc_rx;
} psx_exp2_t;

psx_exp2_t* psx_exp2_create(void);
void psx_exp2_init(psx_exp2_t*, exp2_tty_tx atcons_tx, exp2_tty_tx duart_tx);
void psx_exp2_atcons_put(psx_exp2_t*, char);
void psx_exp2_duart_put(psx_exp2_t*, char);
uint32_t psx_exp2_read32(psx_exp2_t*, uint32_t);
uint16_t psx_exp2_read16(psx_exp2_t*, uint32_t);
uint8_t psx_exp2_read8(psx_exp2_t*, uint32_t);
void psx_exp2_write32(psx_exp2_t*, uint32_t, uint32_t);
void psx_exp2_write16(psx_exp2_t*, uint32_t, uint16_t);
void psx_exp2_write8(psx_exp2_t*, uint32_t, uint8_t);
/* The two ATCONS latches. duart_tx / atcons_tx / *_udata are host callbacks
   and are left in place across a load. */
void psx_exp2_save_state(psx_exp2_t*, psx_state_writer_t*);
int psx_exp2_load_state(psx_exp2_t*, psx_state_reader_t*);
void psx_exp2_destroy(psx_exp2_t*);

#endif