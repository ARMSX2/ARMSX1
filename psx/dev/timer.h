#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#include "ic.h"
#include "gpu.h"
#include "../state.h"

#define PSX_TIMER_BEGIN 0x1f801100
#define PSX_TIMER_SIZE  0x30
#define PSX_TIMER_END   0x1f80112f

/*
  0     Synchronization Enable (0=Free Run, 1=Synchronize via Bit1-2)
  1-2   Synchronization Mode   (0-3, see lists below)
         Synchronization Modes for Counter 0:
           0 = Pause counter during Hblank(s)
           1 = Reset counter to 0000h at Hblank(s)
           2 = Reset counter to 0000h at Hblank(s) and pause outside of Hblank
           3 = Pause until Hblank occurs once, then switch to Free Run
         Synchronization Modes for Counter 1:
           Same as above, but using Vblank instead of Hblank
         Synchronization Modes for Counter 2:
           0 or 3 = Stop counter at current value (forever, no h/v-blank start)
           1 or 2 = Free Run (same as when Synchronization Disabled)
  3     Reset counter to 0000h  (0=After Counter=FFFFh, 1=After Counter=Target)
  4     IRQ when Counter=Target (0=Disable, 1=Enable)
  5     IRQ when Counter=FFFFh  (0=Disable, 1=Enable)
  6     IRQ Once/Repeat Mode    (0=One-shot, 1=Repeatedly)
  7     IRQ Pulse/Toggle Mode   (0=Short Bit10=0 Pulse, 1=Toggle Bit10 on/off)
  8-9   Clock Source (0-3, see list below)
         Counter 0:  0 or 2 = System Clock,  1 or 3 = Dotclock
         Counter 1:  0 or 2 = System Clock,  1 or 3 = Hblank
         Counter 2:  0 or 1 = System Clock,  2 or 3 = System Clock/8
  10    Interrupt Request       (0=Yes, 1=No) (Set after Writing)    (W=1) (R)
  11    Reached Target Value    (0=No, 1=Yes) (Reset after Reading)        (R)
  12    Reached FFFFh Value     (0=No, 1=Yes) (Reset after Reading)        (R)
  13-15 Unknown (seems to be always zero)
  16-31 Garbage (next opcode)
*/
#define MODE_SYNCEN 0x0001
#define MODE_SYNCMD 0x0006
#define T0MD_HBLPAUSE 0
#define T0MD_HBLRESET 1
#define T0MD_HBLRANDP 2
#define T0MD_HBLOSHOT 3
// #define T1MD_VBLPAUSE 0
// #define T1MD_VBLRESET 1
// #define T1MD_VBLRANDP 2
// #define T1MD_VBLOSHOT 3
// #define T2MD_STOPMODE 0
// #define T2MD_FREEMODE 1
#define MODE_RESETC 0x0008
#define MODE_TGTIRQ 0x0010
#define MODE_MAXIRQ 0x0020
#define MODE_IRQRMD 0x0040
#define MODE_IRQPMD 0x0080
#define MODE_CLK 0x0080

/*
  0     Synchronization Enable (0=Free Run, 1=Synchronize via Bit1-2)
  1-2   Synchronization Mode   (0-3, see lists below)
         Synchronization Modes for Counter 0:
           0 = Pause counter during Hblank(s)
           1 = Reset counter to 0000h at Hblank(s)
           2 = Reset counter to 0000h at Hblank(s) and pause outside of Hblank
           3 = Pause until Hblank occurs once, then switch to Free Run
         Synchronization Modes for Counter 1:
           Same as above, but using Vblank instead of Hblank
         Synchronization Modes for Counter 2:
           0 or 3 = Stop counter at current value (forever, no h/v-blank start)
           1 or 2 = Free Run (same as when Synchronization Disabled)
  3     Reset counter to 0000h  (0=After Counter=FFFFh, 1=After Counter=Target)
  4     IRQ when Counter=Target (0=Disable, 1=Enable)
  5     IRQ when Counter=FFFFh  (0=Disable, 1=Enable)
  6     IRQ Once/Repeat Mode    (0=One-shot, 1=Repeatedly)
  7     IRQ Pulse/Toggle Mode   (0=Short Bit10=0 Pulse, 1=Toggle Bit10 on/off)
  8-9   Clock Source (0-3, see list below)
         Counter 0:  0 or 2 = System Clock,  1 or 3 = Dotclock
         Counter 1:  0 or 2 = System Clock,  1 or 3 = Hblank
         Counter 2:  0 or 1 = System Clock,  2 or 3 = System Clock/8
  10    Interrupt Request       (0=Yes, 1=No) (Set after Writing)    (W=1) (R)
  11    Reached Target Value    (0=No, 1=Yes) (Reset after Reading)        (R)
  12    Reached FFFFh Value     (0=No, 1=Yes) (Reset after Reading)        (R)
  13-15 Unknown (seems to be always zero)
  16-31 Garbage (next opcode)
*/

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    psx_ic_t* ic;
    psx_gpu_t* gpu;

    int hblank, prev_hblank;
    int vblank, prev_vblank;

    struct {
        float counter;
        uint32_t target;
        int sync_enable;
        int sync_mode;
        int reset_target;
        int irq_target;
        int irq_max;
        int irq_repeat;
        int irq_toggle;
        int clk_source;
        int irq;
        int target_reached;
        int max_reached;
        int irq_fired;
        uint32_t div_counter;

        int paused;
        int blank_once;
    } timer[3];
} psx_timer_t;

psx_timer_t* psx_timer_create(void);
void psx_timer_init(psx_timer_t*, psx_ic_t*, psx_gpu_t*);
uint32_t psx_timer_read32(psx_timer_t*, uint32_t);
uint16_t psx_timer_read16(psx_timer_t*, uint32_t);
uint8_t psx_timer_read8(psx_timer_t*, uint32_t);
void psx_timer_write32(psx_timer_t*, uint32_t, uint32_t);
void psx_timer_write16(psx_timer_t*, uint32_t, uint16_t);
void psx_timer_write8(psx_timer_t*, uint32_t, uint8_t);
void psx_timer_update(psx_timer_t*, int);
/* The three root counters and the blank-edge latches. ic/gpu are pointers. */
void psx_timer_save_state(psx_timer_t*, psx_state_writer_t*);
int psx_timer_load_state(psx_timer_t*, psx_state_reader_t*);
void psx_timer_destroy(psx_timer_t*);

// GPU event handlers
void psxe_gpu_hblank_event_cb(psx_gpu_t*);
void psxe_gpu_hblank_end_event_cb(psx_gpu_t*);
void psxe_gpu_vblank_timer_event_cb(psx_gpu_t*);
void psxe_gpu_vblank_end_event_cb(psx_gpu_t*);

#endif