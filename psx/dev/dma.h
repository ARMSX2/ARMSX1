#ifndef DMA_H
#define DMA_H

#include <stdint.h>

#define PSX_DMAR_BEGIN 0x1f801080
#define PSX_DMAR_SIZE  0x80
#define PSX_DMAR_END   0x1f8010ff

#include "../bus.h"
#include "../state.h"
#include "ic.h"

typedef struct {
    uint32_t madr;
    uint32_t bcr;
    uint32_t chcr;
} dma_channel_t;

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    psx_bus_t* bus;
    psx_ic_t* ic;

    dma_channel_t mdec_in;
    dma_channel_t mdec_out;
    dma_channel_t gpu;
    dma_channel_t cdrom;
    dma_channel_t spu;
    dma_channel_t pio;
    dma_channel_t otc;

    int mdec_in_irq_delay;
    int mdec_out_irq_delay;
    int cdrom_irq_delay;
    int spu_irq_delay;
    int gpu_irq_delay;
    int otc_irq_delay;

    uint32_t dpcr;
    uint32_t dicr;
} psx_dma_t;

psx_dma_t* psx_dma_create(void);
void psx_dma_init(psx_dma_t*, psx_bus_t*, psx_ic_t*);
void psx_dma_do_mdec_in(psx_dma_t*);
void psx_dma_do_mdec_out(psx_dma_t*);
void psx_dma_do_gpu(psx_dma_t*);
void psx_dma_do_cdrom(psx_dma_t*);
void psx_dma_do_spu(psx_dma_t*);
void psx_dma_do_pio(psx_dma_t*);
void psx_dma_do_otc(psx_dma_t*);
void psx_dma_perform(psx_dma_t*, int);
uint32_t psx_dma_read32(psx_dma_t*, uint32_t);
uint16_t psx_dma_read16(psx_dma_t*, uint32_t);
uint8_t psx_dma_read8(psx_dma_t*, uint32_t);
void psx_dma_write32(psx_dma_t*, uint32_t, uint32_t);
void psx_dma_write16(psx_dma_t*, uint32_t, uint16_t);
void psx_dma_write8(psx_dma_t*, uint32_t, uint8_t);
/* All seven channels, the pending completion-IRQ countdowns, DPCR and DICR.
   dma->bus / dma->ic are pointers and are not saved. */
void psx_dma_save_state(psx_dma_t*, psx_state_writer_t*);
int psx_dma_load_state(psx_dma_t*, psx_state_reader_t*);
void psx_dma_destroy(psx_dma_t*);
void psx_dma_update(psx_dma_t*, int);

typedef void (*psx_dma_do_fn_t)(psx_dma_t*);

/*
  0-2   DMA0, MDECin  Priority      (0..7; 0=Highest, 7=Lowest)
  3     DMA0, MDECin  Master Enable (0=Disable, 1=Enable)
  4-6   DMA1, MDECout Priority      (0..7; 0=Highest, 7=Lowest)
  7     DMA1, MDECout Master Enable (0=Disable, 1=Enable)
  8-10  DMA2, GPU     Priority      (0..7; 0=Highest, 7=Lowest)
  11    DMA2, GPU     Master Enable (0=Disable, 1=Enable)
  12-14 DMA3, CDROM   Priority      (0..7; 0=Highest, 7=Lowest)
  15    DMA3, CDROM   Master Enable (0=Disable, 1=Enable)
  16-18 DMA4, SPU     Priority      (0..7; 0=Highest, 7=Lowest)
  19    DMA4, SPU     Master Enable (0=Disable, 1=Enable)
  20-22 DMA5, PIO     Priority      (0..7; 0=Highest, 7=Lowest)
  23    DMA5, PIO     Master Enable (0=Disable, 1=Enable)
  24-26 DMA6, OTC     Priority      (0..7; 0=Highest, 7=Lowest)
  27    DMA6, OTC     Master Enable (0=Disable, 1=Enable)
  28-30 Unknown, Priority Offset or so? (R/W)
  31    Unknown, no effect? (R/W)
*/

#define DPCR_DMA0EN 0x00000008
#define DPCR_DMA1EN 0x00000080
#define DPCR_DMA2EN 0x00000800
#define DPCR_DMA3EN 0x00008000
#define DPCR_DMA4EN 0x00080000
#define DPCR_DMA5EN 0x00800000
#define DPCR_DMA6EN 0x08000000

/*
  0       Transfer Direction    (0=To Main RAM, 1=From Main RAM)
  1       Memory Address Step   (0=Forward;+4, 1=Backward;-4)
  2-7     Not used              (always zero)
  8       Chopping Enable       (0=Normal, 1=Chopping; run CPU during DMA gaps)
  9-10    SyncMode, Transfer Synchronisation/Mode (0-3):
            0  Start immediately and transfer all at once (used for CDROM, OTC)
            1  Sync blocks to DMA requests   (used for MDEC, SPU, and GPU-data)
            2  Linked-List mode              (used for GPU-command-lists)
            3  Reserved                      (not used)
  11-15   Not used              (always zero)
  16-18   Chopping DMA Window Size (1 SHL N words)
  19      Not used              (always zero)
  20-22   Chopping CPU Window Size (1 SHL N clks)
  23      Not used              (always zero)
  24      Start/Busy            (0=Stopped/Completed, 1=Start/Enable/Busy)
  25-27   Not used              (always zero)
  28      Start/Trigger         (0=Normal, 1=Manual Start; use for SyncMode=0)
  29      Unknown (R/W) Pause?  (0=No, 1=Pause?)     (For SyncMode=0 only?)
  30      Unknown (R/W)
  31      Not used              (always zero)
*/

#define CHCR_TDIR_MASK 0x00000001
#define CHCR_STEP_MASK 0x00000002
#define CHCR_CPEN_MASK 0x00000100
#define CHCR_SYNC_MASK 0x00000600
#define CHCR_CDWS_MASK 0x00070000
#define CHCR_CCWS_MASK 0x00380000
#define CHCR_BUSY_MASK 0x01000000
#define CHCR_TRIG_MASK 0x10000000

#define SYNC_SHIF 9
#define CDWS_SHIF 16
#define CCWS_SHIF 19

#define CHCR_TDIR(c) (dma->c.chcr & CHCR_TDIR_MASK)
#define CHCR_STEP(c) (dma->c.chcr & CHCR_STEP_MASK)
#define CHCR_CPEN(c) (dma->c.chcr & CHCR_CPEN_MASK)
#define CHCR_SYNC(c) ((dma->c.chcr & CHCR_SYNC_MASK) >> SYNC_SHIF)
#define CHCR_CDWS(c) ((dma->c.chcr & CHCR_CDWS_MASK) >> CDWS_SHIF)
#define CHCR_CCWS(c) ((dma->c.chcr & CHCR_CCWS_MASK) >> CCWS_SHIF)
#define CHCR_BUSY(c) (dma->c.chcr & CHCR_BUSY_MASK)
#define CHCR_TRIG(c) (dma->c.chcr & CHCR_TRIG_MASK)

#define BCR_SIZE(c) (dma->c.bcr & 0xffff)
#define BCR_BCNT(c) ((dma->c.bcr >> 16) & 0xffff)

/*
  0-5   Unknown  (read/write-able)
  6-14  Not used (always zero)
  15    Force IRQ (sets bit31)                        (0=None, 1=Force Bit31=1)
  16-22 IRQ Enable setting bit24-30 upon DMA0..DMA6    (0=None, 1=Enable)
  23    IRQ Enable setting bit31 when bit24-30=nonzero (0=None, 1=Enable)
  24-30 IRQ Flags for DMA0..DMA6    (Write 1 to reset) (0=None, 1=IRQ)
  31    IRQ Signal (0-to-1 triggers 1F801070h.bit3)    (0=None, 1=IRQ) (R)
*/

#define DICR_FORCE 0x00008000
#define DICR_FLGEN 0x007f0000
#define DICR_IRQEN 0x00800000
#define DICR_FLAGS 0x7f000000
#define DICR_IRQSI 0x80000000
#define DICR_DMA0EN 0x00010000
#define DICR_DMA1EN 0x00020000
#define DICR_DMA2EN 0x00040000
#define DICR_DMA3EN 0x00080000
#define DICR_DMA4EN 0x00100000
#define DICR_DMA5EN 0x00200000
#define DICR_DMA6EN 0x00400000
#define DICR_DMA0FL 0x01000000
#define DICR_DMA1FL 0x02000000
#define DICR_DMA2FL 0x04000000
#define DICR_DMA3FL 0x08000000
#define DICR_DMA4FL 0x10000000
#define DICR_DMA5FL 0x20000000
#define DICR_DMA6FL 0x40000000

#endif