#ifndef IC_H
#define IC_H

#include <stdint.h>

#include "../cpu.h"
#include "../state.h"

#define PSX_IC_BEGIN 0x1f801070
#define PSX_IC_SIZE  0x8
#define PSX_IC_END   0x1F801077

/*
    0     IRQ0 VBLANK (PAL=50Hz, NTSC=60Hz)
    1     IRQ1 GPU   Can be requested via GP0(1Fh) command (rarely used)
    2     IRQ2 CDROM
    3     IRQ3 DMA
    4     IRQ4 TMR0  Timer 0 aka Root Counter 0 (Sysclk or Dotclk)
    5     IRQ5 TMR1  Timer 1 aka Root Counter 1 (Sysclk or H-blank)
    6     IRQ6 TMR2  Timer 2 aka Root Counter 2 (Sysclk or Sysclk/8)
    7     IRQ7 Controller and Memory Card - Byte Received Interrupt
    8     IRQ8 SIO
    9     IRQ9 SPU
    10    IRQ10 Controller - Lightpen Interrupt (reportedly also PIO...?)
    11-15 Not used (always zero)
    16-31 Garbage
*/
enum {
    IC_VBLANK       = 0x001,
    IC_GPU          = 0x002,
    IC_CDROM        = 0x004,
    IC_DMA          = 0x008,
    IC_TIMER0       = 0x010,
    IC_TIMER1       = 0x020,
    IC_TIMER2       = 0x040,
    IC_JOY          = 0x080,
    IC_SIO          = 0x100,
    IC_SPU          = 0x200,
    IC_LP_PIO       = 0x400
};

/*
    1F801070h 2    I_STAT - Interrupt status register
    1F801074h 2    I_MASK - Interrupt mask register
*/

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint16_t stat;
    uint16_t mask;

    psx_cpu_t* cpu;
} psx_ic_t;

psx_ic_t* psx_ic_create(void);
void psx_ic_init(psx_ic_t*, psx_cpu_t*);
uint32_t psx_ic_read32(psx_ic_t*, uint32_t);
uint16_t psx_ic_read16(psx_ic_t*, uint32_t);
uint8_t psx_ic_read8(psx_ic_t*, uint32_t);
void psx_ic_write32(psx_ic_t*, uint32_t, uint32_t);
void psx_ic_write16(psx_ic_t*, uint32_t, uint16_t);
void psx_ic_write8(psx_ic_t*, uint32_t, uint8_t);
void psx_ic_irq(psx_ic_t*, int);
/* I_STAT / I_MASK. ic->cpu is a pointer and is not saved. */
void psx_ic_save_state(psx_ic_t*, psx_state_writer_t*);
int psx_ic_load_state(psx_ic_t*, psx_state_reader_t*);
void psx_ic_destroy(psx_ic_t*);

#endif