#ifndef SPU_H
#define SPU_H

#include <stdint.h>

#include "ic.h"
#include "../state.h"

#define PSX_SPU_BEGIN 0x1f801c00
#define PSX_SPU_SIZE  0x400
#define PSX_SPU_END   0x1f801fff

#define SPU_RAM_SIZE 0x80000

/* Depth of the sample ring psx_spu_tick() fills. Only ever has to hold ONE frame's pull —
   886 samples at the PAL rate, 744 at NTSC — because the front-end hands out a per-frame
   budget and drains the whole thing again at the end of the same frame. Rounded up to a
   power of two with better than 2x headroom so the modulo is a mask and no plausible
   frame-rate value can overrun it. */
#define PSX_SPU_SAMPLE_RING 2048

/*
    1F801D88h - Voice 0..23 Key ON (Start Attack/Decay/Sustain) (KON) (W)
    1F801D8Ch - Voice 0..23 Key OFF (Start Release) (KOFF) (W)
    1F801D9Ch - Voice 0..23 ON/OFF (status) (ENDX) (R)
    1F801DA6h - Sound RAM Data Transfer Address
    1F801DA8h - Sound RAM Data Transfer Fifo
    1F801DACh - Sound RAM Data Transfer Control (should be 0004h)
*/

#define SPUR_KONL    0x188
#define SPUR_KONH    0x18a
#define SPUR_KOFFL   0x18c
#define SPUR_KOFFH   0x18e
#define SPUR_EONL    0x198
#define SPUR_EONH    0x19a
#define SPUR_ENDXL   0x19c
#define SPUR_ENDXH   0x19e
#define SPUR_TADDR   0x1a6
#define SPUR_TFIFO   0x1a8
#define SPUR_SPUCNT  0x1aa
#define SPUR_TCTRL   0x1ac
#define SPUR_SPUSTAT 0x1ae
#define SPUR_MBASE   0x1a2
#define SPUR_SPUIRQA 0x1a4

typedef struct __attribute__((__packed__)) {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    psx_ic_t* ic;
    uint8_t* ram;

    struct __attribute__((__packed__)) {
        uint16_t volumel;
        uint16_t volumer;
        uint16_t adsampr;
        uint16_t adsaddr;
        uint16_t envctl1;
        uint16_t envctl2;
        uint16_t envcvol;
        uint16_t adraddr;
    } voice[24];

    uint16_t mainlvol;
    uint16_t mainrvol;
    uint16_t vlout;
    uint16_t vrout;
    uint32_t kon;
    uint32_t koff;
    uint32_t pmon;
    uint32_t non;
    uint32_t eon;
    uint32_t endx;
    uint16_t unk_da0;
    uint16_t mbase;
    uint16_t irq9addr;
    uint16_t ramdta;
    uint16_t ramdtf;
    uint16_t spucnt;
    uint16_t ramdtc;
    uint16_t spustat;
    uint32_t cdaivol;
    uint32_t extivol;
    uint32_t currvol;
    uint32_t unk_dbc;
    uint16_t dapf1;
    uint16_t dapf2;
    int16_t  viir;
    int16_t  vcomb1;
    int16_t  vcomb2;
    int16_t  vcomb3;
    int16_t  vcomb4;
    int16_t  vwall;
    int16_t  vapf1;
    int16_t  vapf2;
    uint16_t mlsame;
    uint16_t mrsame;
    uint16_t mlcomb1;
    uint16_t mrcomb1;
    uint16_t mlcomb2;
    uint16_t mrcomb2;
    uint16_t dlsame;
    uint16_t drsame;
    uint16_t mldiff;
    uint16_t mrdiff;
    uint16_t mlcomb3;
    uint16_t mrcomb3;
    uint16_t mlcomb4;
    uint16_t mrcomb4;
    uint16_t dldiff;
    uint16_t drdiff;
    uint16_t mlapf1;
    uint16_t mrapf1;
    uint16_t mlapf2;
    uint16_t mrapf2;
    int16_t  vlin;
    int16_t  vrin;

    // Internal registers unimplemented

    uint32_t taddr;
    uint16_t tfifo[32];
    uint16_t tfifo_index;
    uint32_t revbaddr;
    int lrsl;
    int lrsr;
    int even_cycle;

    struct {
        int playing;
        uint32_t counter;
        uint32_t current_addr;
        uint32_t repeat_addr;
        uint32_t prev_sample_index;
        int16_t s[4];
        int block_flags;
        int16_t buf[28];
        int16_t h[2];
        float lvol;
        float rvol;
        int cvol;
        int eon;
        int reverbl;
        int reverbr;

        /*
        ____lower 16bit (at 1F801C08h+N*10h)___________________________________
        15    Attack Mode       (0=Linear, 1=Exponential)
        -     Attack Direction  (Fixed, always Increase) (until Level 7FFFh)
        14-10 Attack Shift      (0..1Fh = Fast..Slow)
        9-8   Attack Step       (0..3 = "+7,+6,+5,+4")
        -     Decay Mode        (Fixed, always Exponential)
        -     Decay Direction   (Fixed, always Decrease) (until Sustain Level)
        7-4   Decay Shift       (0..0Fh = Fast..Slow)
        -     Decay Step        (Fixed, always "-8")
        3-0   Sustain Level     (0..0Fh)  ;Level=(N+1)*800h
        ____upper 16bit (at 1F801C0Ah+N*10h)___________________________________
        31    Sustain Mode      (0=Linear, 1=Exponential)
        30    Sustain Direction (0=Increase, 1=Decrease) (until Key OFF flag)
        29    Not used?         (should be zero)
        28-24 Sustain Shift     (0..1Fh = Fast..Slow)
        23-22 Sustain Step      (0..3 = "+7,+6,+5,+4" or "-8,-7,-6,-5") (inc/dec)
        21    Release Mode      (0=Linear, 1=Exponential)
        -     Release Direction (Fixed, always Decrease) (until Level 0000h)
        20-16 Release Shift     (0..1Fh = Fast..Slow)
        -     Release Step      (Fixed, always "-8")
        */

        int adsr_phase;
        int adsr_cycles_reload;
        int adsr_cycles;
        int adsr_mode;
        int adsr_dir;
        int adsr_shift;
        int adsr_step;
        int adsr_pending_step;
        int adsr_sustain_level;
        uint32_t envctl;

        /* Set when the CPU writes this voice's ADPCM Repeat Address register (1F801C0Eh),
           cleared on key-on. While set, a Loop Start flag in the sample data must NOT
           overwrite the repeat address — the game has taken ownership of it. Deliberately
           absent from the save-state payload so the existing format is unchanged; it is
           reset on load, and the next register write re-establishes it. */
        int ignore_loop_addr;
    } data[24];

    /* Host-side reverb bypass ([audio] skip_reverb). Not guest state, so it is
       deliberately outside the save-state payload: a state saved with reverb on
       must not force it back on for someone running the low-power path. */
    int reverb_disabled;

    /* Sample ring written by psx_spu_tick() and drained by the front-end. Host-side
       scheduling state, not guest state: absent from the save-state payload, flushed on
       load. See the block comment above psx_spu_tick() in spu.c for why it exists. */
    uint32_t gen_ring[PSX_SPU_SAMPLE_RING];
    int gen_head;
    int gen_tail;
    int gen_count;
    int gen_budget;
    uint32_t gen_cycles;
} psx_spu_t;

psx_spu_t* psx_spu_create(void);
void psx_spu_init(psx_spu_t*, psx_ic_t*);
uint32_t psx_spu_read32(psx_spu_t*, uint32_t);
uint16_t psx_spu_read16(psx_spu_t*, uint32_t);
uint8_t psx_spu_read8(psx_spu_t*, uint32_t);
void psx_spu_write32(psx_spu_t*, uint32_t, uint32_t);
void psx_spu_write16(psx_spu_t*, uint32_t, uint16_t);
void psx_spu_write8(psx_spu_t*, uint32_t, uint8_t);
/* Sound RAM + every voice/mixer/reverb register + the 24 ADPCM decoder and
   ADSR envelope contexts. spu->ic and the spu->ram allocation are not saved
   (the RAM contents are, into the existing buffer). */
void psx_spu_save_state(psx_spu_t*, psx_state_writer_t*);
int psx_spu_load_state(psx_spu_t*, psx_state_reader_t*);
void psx_spu_destroy(psx_spu_t*);
void psx_spu_update_cdda_buffer(psx_spu_t*, void*);
uint32_t psx_spu_get_sample(psx_spu_t*);
/* Sample generation on the CPU's timeline rather than in one lump at the frame boundary.
   psx_spu_begin_frame() grants the number of samples the front-end is going to pull for the
   frame it is about to run; psx_spu_tick() is charged the same device cycles as every other
   peripheral from psx_update() and spends that grant across the frame; psx_spu_pop_sample()
   hands the results back. Read the block comment above psx_spu_tick() before changing any
   of it — the pacing, not the audio, is the point. */
void psx_spu_begin_frame(psx_spu_t*, int sample_budget);
void psx_spu_tick(psx_spu_t*, uint32_t cycles);
int psx_spu_pop_sample(psx_spu_t*, uint32_t* out);
void psx_spu_flush_samples(psx_spu_t*);
/* Skip the reverb all-pass/comb network in psx_spu_get_sample(). Purely a host
   performance lever (spu_get_reverb_sample() is the SPU's hot spot and is still
   marked "To-do: Optimize"); games that use reverb lose their echo when set. */
void psx_spu_set_reverb_disabled(psx_spu_t*, int);

#endif