#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spu.h"
#include "../log.h"
#include "../perf.h"

#define CLAMP(v, l, h) (((v) <= (l)) ? (l) : (((v) >= (h)) ? (h) : (v)))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SPU_RAM_ADDR_MASK (SPU_RAM_SIZE - 1)
#define SPU_RAM_WORD_ADDR_MASK (SPU_RAM_ADDR_MASK & ~1u)

#define VOICE_COUNT 24

/* One SPU RAM halfword written through the transfer FIFO, plus the `audio_diag` accounting
   that says whether the CD-data-to-SPU-RAM path is delivering anything at all and where it
   lands. Advancing taddr is part of the write, so it cannot be forgotten at a call site. */
static inline void spu_ram_diag_write(psx_spu_t* spu, uint32_t addr, uint16_t value);

static inline uint32_t spu_wrap_addr(uint32_t addr) {
    return addr & SPU_RAM_ADDR_MASK;
}

static inline uint32_t spu_wrap_word_addr(uint32_t addr) {
    return addr & SPU_RAM_WORD_ADDR_MASK;
}

static inline uint8_t spu_ram_read8(const psx_spu_t* spu, uint32_t addr) {
    return spu->ram[spu_wrap_addr(addr)];
}

static inline uint16_t spu_ram_read16(const psx_spu_t* spu, uint32_t addr) {
    const uint32_t wrapped = spu_wrap_word_addr(addr);
    const uint32_t next = spu_wrap_addr(wrapped + 1);
    return (uint16_t)(spu->ram[wrapped] | (((uint16_t)spu->ram[next]) << 8));
}

static inline void spu_ram_write16(psx_spu_t* spu, uint32_t addr, uint16_t value) {
    const uint32_t wrapped = spu_wrap_word_addr(addr);
    const uint32_t next = spu_wrap_addr(wrapped + 1);
    spu->ram[wrapped] = value & 0xff;
    spu->ram[next] = value >> 8;
}

static inline uint16_t spu_reg_read16(const uint8_t* ptr, uint32_t offset) {
    uint16_t value;
    memcpy(&value, ptr + offset, sizeof(value));
    return value;
}

static inline uint32_t spu_reg_read32(const uint8_t* ptr, uint32_t offset) {
    uint32_t value;
    memcpy(&value, ptr + offset, sizeof(value));
    return value;
}

static inline void spu_reg_write16(uint8_t* ptr, uint32_t offset, uint16_t value) {
    memcpy(ptr + offset, &value, sizeof(value));
}

static inline void spu_reg_write32(uint8_t* ptr, uint32_t offset, uint32_t value) {
    memcpy(ptr + offset, &value, sizeof(value));
}

/* ---------------------------------------------------------------------------------------
    ACCESS WIDTH AND THE BOUND OF THE REGISTER FILE.

    Every register access — 8, 16 or 32 bit — resolves against one flat mirror: the
    psx_spu_t fields from voice[0].volumel through vrin, laid out packed so a field's byte
    offset in the struct IS its offset from 1F801C00h.

    WIDTH. The SPU register file is 16 bits wide. Hardware serves a byte access out of the
    containing halfword — low byte at an even offset, high byte at an odd one — and there is
    nothing else it could do, because there is no byte-addressable storage behind it. This
    file used to implement only 16- and 32-bit access; psx_spu_read8() logged and returned 0
    and psx_spu_write8() logged and dropped the write. Xenogears reads voice ADSR registers
    a byte at a time (offsets 58h, 98h, 118h, 138h = 1F801C08h+N*10h for voices 5, 9, 17 and
    19), so it was polling envelope configuration and being told zero, several times a
    second, forever.

    BOUND. The mirror is 200h bytes: 1F801C00h..1F801DFFh. The window psx_bus dispatches
    into is PSX_SPU_SIZE = 400h, twice that. 1F801E00h upwards — the internal current-volume
    block and the unused space above it — has NO backing field, and an access there used to
    index the mirror anyway. Past the mirror's end that is not the register file, it is the
    next members of psx_spu_t: taddr, the transfer FIFO, and the 24 voice decoder/ADSR
    contexts. A 32-bit write to 1F801E00h landed squarely on the sound-RAM transfer address.

    The bound is derived from the struct instead of written as a literal so it follows the
    layout: add a register field and it moves with it.
   --------------------------------------------------------------------------------------- */

#define SPU_REG_MIRROR_SIZE \
    ((uint32_t)(offsetof(psx_spu_t, vrin) + sizeof(int16_t) - offsetof(psx_spu_t, voice)))

static inline int spu_reg_in_mirror(uint32_t offset, uint32_t width) {
    return (offset < SPU_REG_MIRROR_SIZE) && ((SPU_REG_MIRROR_SIZE - offset) >= width);
}

/*
    Unimplemented-access logging, once per offset per direction.

    The 8-bit read path was log_fatal(), on a poll that repeats several times a second: 1.1 MB
    of log in a few minutes, at the highest severity there is, for something that never stopped
    the emulator. Nothing survives that — a genuine error scrolls past unread. An access with no
    register behind it is a warn, and a warn that repeats thousands of times a second is not
    readable either, so each offset says its piece once.

    File-scope on purpose: this is host-side log bookkeeping, not guest state, and must stay out
    of the save-state payload the same way reverb_disabled and gen_ring do.
*/
static uint8_t g_spu_unimpl_read_seen[PSX_SPU_SIZE / 8];
static uint8_t g_spu_unimpl_write_seen[PSX_SPU_SIZE / 8];

static int spu_warn_once(uint8_t* seen, uint32_t offset) {
    const uint32_t masked = offset & (PSX_SPU_SIZE - 1u);
    const uint8_t bit = (uint8_t)(1u << (masked & 7u));

    if (seen[masked >> 3] & bit)
        return 0;

    seen[masked >> 3] |= bit;

    return 1;
}

/* The halfword the mirror holds, or 0 when the offset has no register behind it. No side
   effects — in particular it never touches the transfer FIFO, so it is safe to call on the
   read-modify-write path of an 8-bit write. */
static inline uint16_t spu_reg_mirror16(const psx_spu_t* spu, uint32_t offset) {
    if (!spu_reg_in_mirror(offset, 2))
        return 0;

    return spu_reg_read16((const uint8_t*)&spu->voice[0].volumel, offset);
}

static inline uint16_t spu_reg_peek16(const psx_spu_t* spu, uint32_t offset) {
    if (!spu_reg_in_mirror(offset, 2) && spu_warn_once(g_spu_unimpl_read_seen, offset)) {
        log_warn("SPU read at offset %08x has no register behind it, returning 0"
                 " (logged once per offset)", offset);
    }

    return spu_reg_mirror16(spu, offset);
}

// static float interpolate_hermite(float a, float b, float c, float d, float t) {
//     float x = -a/2.0f + (3.0f*b)/2.0f - (3.0f*c)/2.0f + d/2.0f;
//     float y = a - (5.0f*b)/2.0f + 2.0f*c - d / 2.0f;
//     float z = -a/2.0f + c/2.0f;
//     float w = b;
 
//     return (x*t*t*t) + (y*t*t) + (z*t) + w;
// }

static const int g_spu_pos_adpcm_table[] = {
    0, +60, +115, +98, +122
};

static const int g_spu_neg_adpcm_table[] = {
    0,   0,  -52, -55,  -60
};

static const int16_t g_spu_gauss_table[] = {
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
    0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
    0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
    0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
    0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
    0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
    0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
    0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
    0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
    0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
    0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
    0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
    0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
    0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
    0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
    0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
    0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
    0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
    0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
    0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
    0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
    0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
    0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
    0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
    0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
    0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
    0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
    0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
    0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
    0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
    0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
    0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
    0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
    0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
    0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
    0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
    0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
    0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
    0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
    0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
    0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
    0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
    0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
    0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
    0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
    0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
    0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
    0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
    0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
    0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
    0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
    0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
    0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
    0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
    0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
    0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
    0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
    0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
    0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
    0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
    0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
    0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3
};

psx_spu_t* psx_spu_create(void) {
    return (psx_spu_t*)malloc(sizeof(psx_spu_t));
}

void psx_spu_init(psx_spu_t* spu, psx_ic_t* ic) {
    memset(spu, 0, sizeof(psx_spu_t));

    spu->io_base = PSX_SPU_BEGIN;
    spu->io_size = PSX_SPU_SIZE;

    spu->ic = ic;
    spu->ram = (uint8_t*)malloc(SPU_RAM_SIZE);

    memset(spu->ram, 0, SPU_RAM_SIZE);

    // Mute all voices
    spu->endx = 0x00ffffff;
    spu->irq9addr = 0xffff;
}

static inline void spu_ram_diag_write(psx_spu_t* spu, uint32_t addr, uint16_t value) {
    spu_ram_write16(spu, addr, value);

    if (g_psx_audio_diag_enabled) {
        g_psx_audio_diag.spu_ram_writes++;

        if (addr < g_psx_audio_diag.spu_ram_lo)
            g_psx_audio_diag.spu_ram_lo = addr;

        if (addr > g_psx_audio_diag.spu_ram_hi)
            g_psx_audio_diag.spu_ram_hi = addr;
    }

    spu->taddr = spu_wrap_word_addr(spu->taddr + 2);
}

void psx_spu_set_reverb_disabled(psx_spu_t* spu, int disabled) {
    if (!spu)
        return;

    /* The reverb work buffer keeps feeding back off itself; leaving stale echo
       tails in it would bleed back in the moment reverb is re-enabled. */
    if (spu->reverb_disabled != (disabled != 0)) {
        spu->lrsl = 0;
        spu->lrsr = 0;
    }

    spu->reverb_disabled = disabled != 0;
}

uint32_t psx_spu_read32(psx_spu_t* spu, uint32_t offset) {
    if (!spu_reg_in_mirror(offset, 4)) {
        if (spu_warn_once(g_spu_unimpl_read_seen, offset)) {
            log_warn("SPU read at offset %08x has no register behind it, returning 0"
                     " (logged once per offset)", offset);
        }

        return 0x0;
    }

    const uint8_t* ptr = (uint8_t*)&spu->voice[0].volumel;

    return spu_reg_read32(ptr, offset);
}

uint16_t psx_spu_read16(psx_spu_t* spu, uint32_t offset) {
    if (offset == SPUR_TFIFO) {
        uint16_t data = spu_ram_read16(spu, spu->taddr);
        spu->taddr = spu_wrap_word_addr(spu->taddr + 2);

        return data;
    }

    return spu_reg_peek16(spu, offset);
}

/*
    8-bit read: the byte of the containing 16-bit register that this offset selects.

    Deliberately NOT routed through psx_spu_read16(), because that read has a side effect —
    1F801DA8h pops a halfword of sound RAM and advances the transfer address. Serving a byte
    read out of it would consume a whole halfword per byte, so reading both halves of one
    register would return bytes from two different words and would drag the transfer address
    along with it. nocash lists the FIFO port as write-only; a byte read of it gets the last
    value written (what the mirror holds), and nothing here disturbs a transfer in flight.
*/
uint8_t psx_spu_read8(psx_spu_t* spu, uint32_t offset) {
    const uint16_t reg = spu_reg_peek16(spu, offset & ~1u);

    return (offset & 1u) ? (uint8_t)(reg >> 8) : (uint8_t)(reg & 0xffu);
}

/*
    Sound RAM IRQ (1F801DA4h, enabled by SPUCNT bit 6).

    Raised when a voice reaches the watched address. It is checked HERE, in the one function
    every path funnels through — spu_read_block() is called after a linear advance, after both
    loop-end jumps, and at key-on — because it previously lived inline in the block-advance
    switch and only in the `case 0: case 2:` branch. The loop-end cases raised nothing.

    That is fatal for streamed audio. A game double-buffering a ring watches the wrap point so
    it knows when to refill the half that just played, and the wrap point is reached ONLY by the
    loop-end jump — the exact path that skipped the check. The voice then loops the same ~0.6 s
    of sound RAM forever because nothing ever tells the game to refill it, which is heard as a
    phrase replaying a fragment of itself before continuing.

    Matched over the whole 16-byte block rather than by exact equality: irq9addr is in 8-byte
    units while a voice's current address only ever advances 16 bytes at a time, so an IRQ
    address on an odd 8-byte boundary could never compare equal and the interrupt was
    unreachable for half of all legal settings.
*/
static inline void spu_check_irq(psx_spu_t* spu, uint32_t addr) {
    if (!(spu->spucnt & 0x40))
        return;

    const uint32_t irq_addr = spu_wrap_addr((uint32_t)spu->irq9addr << 3);

    if ((irq_addr & ~0xfu) != (spu_wrap_addr(addr) & ~0xfu))
        return;

    psx_ic_irq(spu->ic, IC_SPU);

    if (g_psx_audio_diag_enabled)
        g_psx_audio_diag.spu_irq_raised++;
}

void spu_read_block(psx_spu_t* spu, int v) {
    uint32_t addr = spu_wrap_addr(spu->data[v].current_addr);
    uint8_t hdr = spu_ram_read8(spu, addr);

    spu_check_irq(spu, addr);

    spu->data[v].block_flags = spu_ram_read8(spu, addr + 1);

    unsigned hdr_shift = hdr & 0x0f;

    if (hdr_shift > 12)
        hdr_shift = 9;

    unsigned shift  = 12 - hdr_shift;
    unsigned filter = (hdr >> 4) & 7;

    /* OUT-OF-BOUNDS READ. The filter field is three bits, so it takes 0..7, but both
       coefficient tables have exactly FIVE entries — filters 5..7 read straight off the end.
       Measured values for the overrun: filter=5 -> (0,-1), 6 -> (0,-1), 7 -> (-52,+65535),
       i.e. whatever .rodata follows, decoded as prediction coefficients.

       Reserved filters are not supposed to appear in well-formed samples, but "not supposed
       to" is not a bound: a mid-stream key-on, a misaligned block, or a buffer that has not
       been filled yet all put arbitrary bytes in the header byte. Real hardware has defined
       behaviour here and DuckStation encodes it as std::min(filter, 4). Note the XA decoder in
       psx/dev/cdrom/audio.c already masks with 0x30 and cannot overrun — the two ADPCM decoders
       in this tree disagreed, and this was the unbounded one. */
    if (filter > 4)
        filter = 4;

    int32_t f0 = g_spu_pos_adpcm_table[filter];
    int32_t f1 = g_spu_neg_adpcm_table[filter];

    for (int j = 0; j < 28; j++) {
        uint16_t n = (spu_ram_read8(spu, addr + 2 + (j >> 1)) >> ((j & 1) * 4)) & 0xf;

        // Sign extend t
        int16_t t = (int16_t)(n << 12) >> 12;

        /* The saturation below used to be applied to an int16_t, which made it DEAD CODE: the
           sum was truncated into int16 by the assignment, so `s < INT16_MIN` and `s > INT16_MAX`
           were both unreachable and the compiler removed the test entirely (verified: zero
           compare instructions emitted). Samples that should have clipped wrapped instead —
           +58672 came out as -6864, -62767 as +2769 — turning a loud passage into full-scale
           sign inversion.

           ADPCM prediction is recursive and h[] carries across blocks, so one wrapped sample
           poisons every sample after it. Accumulating in int32 and clamping on the way down is
           what makes the existing intent actually happen. */
        int32_t acc = (t << shift) +
                      (((spu->data[v].h[0] * f0) + (spu->data[v].h[1] * f1) + 32) / 64);

        int16_t s = (acc < INT16_MIN) ? INT16_MIN
                                      : ((acc > INT16_MAX) ? INT16_MAX : (int16_t)acc);

        spu->data[v].h[1] = spu->data[v].h[0];
        spu->data[v].h[0] = s;
        spu->data[v].buf[j] = s;
    }
}

#define PHASE spu->data[v].adsr_phase
#define CYCLES spu->data[v].adsr_cycles
#define EXPONENTIAL spu->data[v].adsr_mode
#define DECREASE spu->data[v].adsr_dir
#define SHIFT spu->data[v].adsr_shift
#define STEP spu->data[v].adsr_step
#define LEVEL_STEP spu->data[v].adsr_pending_step
#define LEVEL spu->data[v].cvol

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

enum {
    ADSR_ATTACK,
    ADSR_DECAY,
    ADSR_SUSTAIN,
    ADSR_RELEASE,
    ADSR_END
};

/*
    ADSR configuration, read LIVE from 1F801C08h+N*10h / 1F801C0Ah+N*10h.

    This is the THIRD field in this file found snapshotted at key-on, after the voice volume and
    the ADPCM repeat address. spu_adsr_cfg(spu, v) was built once in spu_kon() and every envelope
    phase was then derived from that frozen copy, so an ADSR register written after key-on did
    nothing at all.

    It matters most on RELEASE, because that is the one phase a game configures immediately
    before it needs it. The standard way to stop a looping sound effect is: write a release rate
    to 1F801C0Ah, then key off at 1F801D8Ch. spu_koff() -> adsr_load_release() was reading the
    release shift captured at key-on instead, so a voice keyed on with a slow release rate could
    take effectively forever to fall silent no matter what the game asked for on the way out —
    a sound effect that outlives the state that started it, and outlives a game over.

    The sustain level (bits 3-0 of the low half) has the same problem and is derived live too.

    data[v].envctl and data[v].adsr_sustain_level are still written at key-on and still in the
    save-state payload; keeping them preserves the existing format for fields nothing reads now.
*/
static inline uint32_t spu_adsr_cfg(const psx_spu_t* spu, int v) {
    return (((uint32_t)spu->voice[v].envctl2) << 16) | (uint32_t)spu->voice[v].envctl1;
}

static inline int spu_adsr_sustain_level(const psx_spu_t* spu, int v) {
    return (((int)(spu->voice[v].envctl1 & 0xf)) + 1) * 0x800;
}

void adsr_calculate_values(psx_spu_t* spu, int v) {
    CYCLES = 1 << MAX(0, SHIFT - 11);
    LEVEL_STEP = STEP << MAX(0, 11 - SHIFT);

    if (EXPONENTIAL && (LEVEL > 0x6000) && !DECREASE)
        CYCLES *= 4;
    
    if (EXPONENTIAL && DECREASE)
        LEVEL_STEP = (LEVEL_STEP * LEVEL) >> 15;
    
    spu->data[v].adsr_cycles_reload = CYCLES;
}

void adsr_load_attack(psx_spu_t* spu, int v) {
    EXPONENTIAL = spu_adsr_cfg(spu, v) >> 15;
    DECREASE    = 0;
    SHIFT       = (spu_adsr_cfg(spu, v) >> 10) & 0x1f;
    STEP        = 7 - ((spu_adsr_cfg(spu, v) >> 8) & 3);
    LEVEL       = 0;
    PHASE       = ADSR_ATTACK;

    adsr_calculate_values(spu, v);
}

void adsr_load_decay(psx_spu_t* spu, int v) {
    EXPONENTIAL = 1;
    DECREASE    = 1;
    SHIFT       = (spu_adsr_cfg(spu, v) >> 4) & 0xf;
    STEP        = -8;
    LEVEL       = 0x7fff;
    PHASE       = ADSR_DECAY;

    adsr_calculate_values(spu, v);
}

void adsr_load_sustain(psx_spu_t* spu, int v) {
    EXPONENTIAL = spu_adsr_cfg(spu, v) >> 31;
    DECREASE    = (spu_adsr_cfg(spu, v) >> 30) & 1;
    SHIFT       = (spu_adsr_cfg(spu, v) >> 24) & 0x1f;
    STEP        = (spu_adsr_cfg(spu, v) >> 22) & 3;
    LEVEL       = spu_adsr_sustain_level(spu, v);
    STEP        = DECREASE ? (-8 + STEP) : (7 - STEP);
    PHASE       = ADSR_SUSTAIN;

    adsr_calculate_values(spu, v);
}

void adsr_load_release(psx_spu_t* spu, int v) {
    EXPONENTIAL = (spu_adsr_cfg(spu, v) >> 21) & 1;
    DECREASE    = 1;
    SHIFT       = (spu_adsr_cfg(spu, v) >> 16) & 0x1f;
    STEP        = -8;
    PHASE       = ADSR_RELEASE;

    spu->endx |= 1 << v;

    adsr_calculate_values(spu, v);
}

void spu_handle_adsr(psx_spu_t* spu, int v) {
    if (CYCLES) {
        CYCLES -= 1;

        return;
    }

    adsr_calculate_values(spu, v);

    LEVEL += LEVEL_STEP;

    switch (spu->data[v].adsr_phase) {
        case ADSR_ATTACK: {
            LEVEL = CLAMP(LEVEL, 0x0000, 0x7fff);

            if (LEVEL == 0x7fff)
                adsr_load_decay(spu, v);
        } break;

        case ADSR_DECAY: {
            LEVEL = CLAMP(LEVEL, 0x0000, 0x7fff);

            if (LEVEL <= spu_adsr_sustain_level(spu, v))
                adsr_load_sustain(spu, v);
        } break;

        case ADSR_SUSTAIN: {
            LEVEL = CLAMP(LEVEL, 0x0000, 0x7fff);

            /* Not stopped automatically, need to KOFF */
        } break;

        case ADSR_RELEASE: {
            LEVEL = CLAMP(LEVEL, 0x0000, 0x7fff);

            if (!LEVEL) {
                PHASE = ADSR_END;
                CYCLES = 0;
                LEVEL_STEP = 0;

                spu->data[v].playing = 0;
            }
        } break;

        case ADSR_END: {
            spu->data[v].playing = 0;
        } break;
    }

    spu->voice[v].envcvol = spu->data[v].cvol;

    CYCLES = spu->data[v].adsr_cycles_reload;
}

#undef PHASE
#undef CYCLES
#undef MODE
#undef DIR
#undef SHIFT
#undef STEP
#undef PENDING_STEP

void spu_kon(psx_spu_t* spu, uint32_t value) {
    for (int i = 0; i < VOICE_COUNT; i++) {
        if ((value & (1 << i))) {
            spu->data[i].playing = 1;
            spu->data[i].current_addr = spu_wrap_addr((uint32_t)spu->voice[i].adsaddr << 3);
            spu->data[i].repeat_addr = spu_wrap_addr((uint32_t)spu->voice[i].adraddr << 3);
            /* A fresh voice takes its repeat address from the sample data again. */
            spu->data[i].ignore_loop_addr = 0;
            /* `audio_diag`. 1F801C00h+N*10h is NOT a plain unsigned magnitude: bit15 selects
               sweep mode, and in volume mode bits14-0 are a SIGNED 15-bit value (-4000h..+3FFFh).
               The conversion below reads all 16 bits as unsigned, so this counts how often the
               game actually uses the two encodings that reading cannot represent. Both are gain
               errors of up to 4x in the wrong direction, and gunfire is where a game reaches for
               them. Key-on only: ~tens of events a second, not a hot path. */
            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.kon_voices++;

                if ((spu->voice[i].volumel & 0x8000) || (spu->voice[i].volumer & 0x8000))
                    g_psx_audio_diag.kon_vol_sweep++;
                else if ((spu->voice[i].volumel & 0x4000) || (spu->voice[i].volumer & 0x4000))
                    g_psx_audio_diag.kon_vol_negative++;
            }

            spu->data[i].lvol = ((float)(spu->voice[i].volumel) / 32767.0f) * 2.0f;
            spu->data[i].rvol = ((float)(spu->voice[i].volumer) / 32767.0f) * 2.0f;
            spu->data[i].adsr_sustain_level = ((spu->voice[i].envctl1 & 0xf) + 1) * 0x800;
            spu->data[i].envctl = (((uint32_t)spu->voice[i].envctl2) << 16) |
                                    (uint32_t)spu->voice[i].envctl1;

            adsr_load_attack(spu, i);
            spu_read_block(spu, i);

            spu->voice[i].envcvol = 0x7fff;
        }
    }

    spu->endx &= ~(value & 0x00ffffff);
}

void spu_koff(psx_spu_t* spu, uint32_t value) {
    for (int i = 0; i < VOICE_COUNT; i++)
        if (value & (1 << i))
            adsr_load_release(spu, i);
}

int spu_handle_write(psx_spu_t* spu, uint32_t offset, uint32_t value) {
    switch (offset) {
        case SPUR_KONL: case SPUR_KONH: {
            int high = (offset & 2) != 0;

            if (!value)
                return 1;

            spu_kon(spu, value << (16 * high));
        } return 1;

        // case SPUR_SPUIRQA: {
        //     spu->irq9addr = value << 3;
        // } return 1;

        case SPUR_KOFFL: case SPUR_KOFFH: {
            int high = (offset & 2) != 0;

            if (!value)
                return 1;

            spu_koff(spu, value << (16 * high));
        } return 1;

        case SPUR_TADDR: {
            spu->ramdta = value;
            spu->taddr = spu_wrap_word_addr((uint32_t)value << 3);
        } return 1;

        case SPUR_TFIFO: {
            spu->ramdtf = value;

            /*
                OUT-OF-BOUNDS WRITE, fixed by the bound below.

                This was `spu->tfifo[spu->tfifo_index++] = value;` with no check against the
                32-entry array, and the index is only ever reset inside the `== 32` branch —
                which is itself gated on the transfer mode being DMA-write. Any run of more
                than 32 FIFO writes in any other mode therefore walked straight off the end.

                It is far worse than a normal overrun because psx_spu_t is PACKED and
                tfifo_index is the field immediately after the array: tfifo[32] IS
                tfifo_index. Measured layout — tfifo at 544, tfifo_index at 608, data[0] at
                626, sizeof(psx_spu_t) 4374. So the 33rd write sets tfifo_index to the ADPCM
                sample word itself, the ++ makes it value+1, and the NEXT write lands at
                tfifo[value+1] — an arbitrary offset up to 128 KB past a 4374-byte struct.
                Voice decoder state (data[24], from offset 626, i.e. tfifo[41] onward) is the
                first casualty, and everything past the allocation is heap corruption.

                Dropping the write when the FIFO is full is what a full hardware FIFO does,
                and in the working case (DMA-write mode) behaviour is bit-identical: the FIFO
                still fills to exactly 32, flushes, and resets.

                Memory-safety fix. NOT claimed to be the cause of any reported symptom until
                spu_fifo_drop / spu_ram_writes come back from a capture and say so.
            */
            if (spu->tfifo_index < 32) {
                spu->tfifo[spu->tfifo_index++] = value;
            } else if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.spu_fifo_drop++;
            }

            if (spu->tfifo_index >= 32) {
                if (((spu->spucnt >> 4) & 3) == 2) {
                    for (int i = 0; i < 32; i++) {
                        spu_ram_diag_write(spu, spu->taddr, spu->tfifo[i]);
                    }

                    spu->tfifo_index = 0;
                }
            }
        } return 1;

        case SPUR_SPUCNT: {
            spu->spucnt = value;
            spu->spustat &= 0xffc0;
            spu->spustat |= value & 0x3f;

            if ((value >> 4) & 3) {
                const int pending = (spu->tfifo_index > 32) ? 32 : (int)spu->tfifo_index;

                for (int i = 0; i < pending; i++) {
                    spu_ram_diag_write(spu, spu->taddr, spu->tfifo[i]);
                }

                spu->tfifo_index = 0;
            }
        } return 1;

        case SPUR_MBASE: {
            spu->mbase = value;
            spu->revbaddr = spu_wrap_word_addr((uint32_t)spu->mbase << 3);
        } return 1;
    }

    return 0;
}

/*
    The CPU taking ownership of a voice's ADPCM Repeat Address (1F801C0Eh+N*10h).

    Hardware latches this: once the CPU has written the register, a Loop Start flag in the
    sample data must NOT overwrite it, and the latch is released on key-on. DuckStation models
    it as Voice::ignore_loop_address for the same reason.

    Without it, a streaming voice replays a FRAGMENT: the game points the repeat address at the
    next buffer half, then the decoder walks onto a block carrying Loop Start and drags the
    repeat address backwards to that block. The next loop jump lands a couple of blocks early,
    so a word or two is heard twice before playback continues correctly — "contact me contact
    me by codec".

    Called with the byte width of the register write, and latches on any OVERLAP with either
    byte of the register: a game that sets the address with two 8-bit writes still means it.
*/
static inline void spu_note_repeat_addr_write(psx_spu_t* spu, uint32_t offset, uint32_t width) {
    const uint32_t last = offset + width - 1u;

    if (offset >= (uint32_t)(VOICE_COUNT * 16))
        return;

    for (uint32_t v = offset >> 4; (v <= (last >> 4)) && (v < (uint32_t)VOICE_COUNT); v++) {
        const uint32_t reg = (v << 4) + 0x0eu;

        if ((offset <= (reg + 1u)) && (reg <= last))
            spu->data[v].ignore_loop_addr = 1;
    }
}

void psx_spu_write32(psx_spu_t* spu, uint32_t offset, uint32_t value) {
    // Handle special cases first
    if (spu_handle_write(spu, offset, value))
        return;

    if (!spu_reg_in_mirror(offset, 4)) {
        if (spu_warn_once(g_spu_unimpl_write_seen, offset)) {
            log_warn("SPU write at offset %08x (%08x) has no register behind it, ignored"
                     " (logged once per offset)", offset, value);
        }

        return;
    }

    uint8_t* ptr = (uint8_t*)&spu->voice[0];

    spu_note_repeat_addr_write(spu, offset, 4);

    spu_reg_write32(ptr, offset, value);
}

void psx_spu_write16(psx_spu_t* spu, uint32_t offset, uint16_t value) {
    // Handle special cases first
    if (spu_handle_write(spu, offset, value))
        return;

    if (!spu_reg_in_mirror(offset, 2)) {
        if (spu_warn_once(g_spu_unimpl_write_seen, offset)) {
            log_warn("SPU write at offset %08x (%04x) has no register behind it, ignored"
                     " (logged once per offset)", offset, value);
        }

        return;
    }

    uint8_t* ptr = (uint8_t*)&spu->voice[0].volumel;

    spu_note_repeat_addr_write(spu, offset, 2);

    if (offset != 0x0c)
        spu_reg_write16(ptr, offset, value);
}

/*
    8-bit write: merge the byte into the containing 16-bit register and dispatch that.

    The register file is 16 bits wide, so hardware has no narrower write than this. Going
    back through psx_spu_write16() is the point rather than a shortcut — it is what keeps
    a byte write to a register with behaviour attached working at all: KON/KOFF still key
    voices, 1F801DA6h still recomputes the transfer address, 1F801DAAh still flushes the
    FIFO, 1F801DA2h still moves the reverb base, and spu_note_repeat_addr_write() still
    latches, which its own comment already anticipated ("a game that sets the address with
    two 8-bit writes still means it"). Before this, all of that was a printf and a dropped
    write.

    The merge reads the mirror directly and never the FIFO, so composing a halfword out of
    two byte writes cannot disturb a sound-RAM transfer. Registers whose writes are consumed
    by spu_handle_write() never reach the mirror, so KON/KOFF merge against 0 and a byte
    write keys exactly the voices that byte names — which is the hardware behaviour.
*/
void psx_spu_write8(psx_spu_t* spu, uint32_t offset, uint8_t value) {
    const uint32_t aligned = offset & ~1u;
    const uint16_t current = spu_reg_mirror16(spu, aligned);

    const uint16_t merged = (offset & 1u)
        ? (uint16_t)((current & 0x00ffu) | ((uint16_t)value << 8))
        : (uint16_t)((current & 0xff00u) | (uint16_t)value);

    psx_spu_write16(spu, aligned, merged);
}


/* ---------------------------------------------------------------------------
   Save state.

   Saved: the full 512 KiB sound RAM, the 24 voice register sets, every global
   mixer/reverb register, the data-transfer FIFO, and the 24 per-voice decoder +
   ADSR envelope contexts (including the 28-sample ADPCM block and the two
   decoder history taps, without which the first block after a load decodes
   wrong).

   NOT saved: spu->ic (host wiring) and spu->ram (the allocation itself — its
   CONTENTS are saved and restored into the existing buffer).

   Host-side audio is deliberately NOT core state. The front-end's SDL audio
   queue keeps whatever it had queued at load time; it drains within a few
   milliseconds and then follows the restored SPU. The CD audio the CD-ROM
   pushes into the SPU is part of the CD-ROM section, not this one.
   --------------------------------------------------------------------------- */

void psx_spu_save_state(psx_spu_t* spu, psx_state_writer_t* w) {
    int i;
    int j;

    psx_sw_u32(w, SPU_RAM_SIZE);
    psx_sw_bytes(w, spu->ram, SPU_RAM_SIZE);

    for (i = 0; i < 24; i++) {
        psx_sw_u16(w, spu->voice[i].volumel);
        psx_sw_u16(w, spu->voice[i].volumer);
        psx_sw_u16(w, spu->voice[i].adsampr);
        psx_sw_u16(w, spu->voice[i].adsaddr);
        psx_sw_u16(w, spu->voice[i].envctl1);
        psx_sw_u16(w, spu->voice[i].envctl2);
        psx_sw_u16(w, spu->voice[i].envcvol);
        psx_sw_u16(w, spu->voice[i].adraddr);
    }

    psx_sw_u16(w, spu->mainlvol);
    psx_sw_u16(w, spu->mainrvol);
    psx_sw_u16(w, spu->vlout);
    psx_sw_u16(w, spu->vrout);
    psx_sw_u32(w, spu->kon);
    psx_sw_u32(w, spu->koff);
    psx_sw_u32(w, spu->pmon);
    psx_sw_u32(w, spu->non);
    psx_sw_u32(w, spu->eon);
    psx_sw_u32(w, spu->endx);
    psx_sw_u16(w, spu->unk_da0);
    psx_sw_u16(w, spu->mbase);
    psx_sw_u16(w, spu->irq9addr);
    psx_sw_u16(w, spu->ramdta);
    psx_sw_u16(w, spu->ramdtf);
    psx_sw_u16(w, spu->spucnt);
    psx_sw_u16(w, spu->ramdtc);
    psx_sw_u16(w, spu->spustat);
    psx_sw_u32(w, spu->cdaivol);
    psx_sw_u32(w, spu->extivol);
    psx_sw_u32(w, spu->currvol);
    psx_sw_u32(w, spu->unk_dbc);
    psx_sw_u16(w, spu->dapf1);
    psx_sw_u16(w, spu->dapf2);
    psx_sw_u16(w, (uint16_t)spu->viir);
    psx_sw_u16(w, (uint16_t)spu->vcomb1);
    psx_sw_u16(w, (uint16_t)spu->vcomb2);
    psx_sw_u16(w, (uint16_t)spu->vcomb3);
    psx_sw_u16(w, (uint16_t)spu->vcomb4);
    psx_sw_u16(w, (uint16_t)spu->vwall);
    psx_sw_u16(w, (uint16_t)spu->vapf1);
    psx_sw_u16(w, (uint16_t)spu->vapf2);
    psx_sw_u16(w, spu->mlsame);
    psx_sw_u16(w, spu->mrsame);
    psx_sw_u16(w, spu->mlcomb1);
    psx_sw_u16(w, spu->mrcomb1);
    psx_sw_u16(w, spu->mlcomb2);
    psx_sw_u16(w, spu->mrcomb2);
    psx_sw_u16(w, spu->dlsame);
    psx_sw_u16(w, spu->drsame);
    psx_sw_u16(w, spu->mldiff);
    psx_sw_u16(w, spu->mrdiff);
    psx_sw_u16(w, spu->mlcomb3);
    psx_sw_u16(w, spu->mrcomb3);
    psx_sw_u16(w, spu->mlcomb4);
    psx_sw_u16(w, spu->mrcomb4);
    psx_sw_u16(w, spu->dldiff);
    psx_sw_u16(w, spu->drdiff);
    psx_sw_u16(w, spu->mlapf1);
    psx_sw_u16(w, spu->mrapf1);
    psx_sw_u16(w, spu->mlapf2);
    psx_sw_u16(w, spu->mrapf2);
    psx_sw_u16(w, (uint16_t)spu->vlin);
    psx_sw_u16(w, (uint16_t)spu->vrin);

    psx_sw_u32(w, spu->taddr);
    psx_sw_u16_array(w, spu->tfifo, 32);
    psx_sw_u16(w, spu->tfifo_index);
    psx_sw_u32(w, spu->revbaddr);
    psx_sw_i32(w, spu->lrsl);
    psx_sw_i32(w, spu->lrsr);
    psx_sw_i32(w, spu->even_cycle);

    for (i = 0; i < 24; i++) {
        psx_sw_i32(w, spu->data[i].playing);
        psx_sw_u32(w, spu->data[i].counter);
        psx_sw_u32(w, spu->data[i].current_addr);
        psx_sw_u32(w, spu->data[i].repeat_addr);
        psx_sw_u32(w, spu->data[i].prev_sample_index);

        for (j = 0; j < 4; j++)
            psx_sw_u16(w, (uint16_t)spu->data[i].s[j]);

        psx_sw_i32(w, spu->data[i].block_flags);
        psx_sw_i16_array(w, spu->data[i].buf, 28);

        for (j = 0; j < 2; j++)
            psx_sw_u16(w, (uint16_t)spu->data[i].h[j]);

        psx_sw_f32(w, spu->data[i].lvol);
        psx_sw_f32(w, spu->data[i].rvol);
        psx_sw_i32(w, spu->data[i].cvol);
        psx_sw_i32(w, spu->data[i].eon);
        psx_sw_i32(w, spu->data[i].reverbl);
        psx_sw_i32(w, spu->data[i].reverbr);

        psx_sw_i32(w, spu->data[i].adsr_phase);
        psx_sw_i32(w, spu->data[i].adsr_cycles_reload);
        psx_sw_i32(w, spu->data[i].adsr_cycles);
        psx_sw_i32(w, spu->data[i].adsr_mode);
        psx_sw_i32(w, spu->data[i].adsr_dir);
        psx_sw_i32(w, spu->data[i].adsr_shift);
        psx_sw_i32(w, spu->data[i].adsr_step);
        psx_sw_i32(w, spu->data[i].adsr_pending_step);
        psx_sw_i32(w, spu->data[i].adsr_sustain_level);
        psx_sw_u32(w, spu->data[i].envctl);
    }
}

int psx_spu_load_state(psx_spu_t* spu, psx_state_reader_t* r) {
    uint32_t ram_size;
    int i;
    int j;

    ram_size = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (ram_size != SPU_RAM_SIZE)
        return PSX_STATE_ERR_GEOMETRY;

    psx_sr_bytes(r, spu->ram, SPU_RAM_SIZE);

    for (i = 0; i < 24; i++) {
        spu->voice[i].volumel = psx_sr_u16(r);
        spu->voice[i].volumer = psx_sr_u16(r);
        spu->voice[i].adsampr = psx_sr_u16(r);
        spu->voice[i].adsaddr = psx_sr_u16(r);
        spu->voice[i].envctl1 = psx_sr_u16(r);
        spu->voice[i].envctl2 = psx_sr_u16(r);
        spu->voice[i].envcvol = psx_sr_u16(r);
        spu->voice[i].adraddr = psx_sr_u16(r);
    }

    spu->mainlvol = psx_sr_u16(r);
    spu->mainrvol = psx_sr_u16(r);
    spu->vlout = psx_sr_u16(r);
    spu->vrout = psx_sr_u16(r);
    spu->kon = psx_sr_u32(r);
    spu->koff = psx_sr_u32(r);
    spu->pmon = psx_sr_u32(r);
    spu->non = psx_sr_u32(r);
    spu->eon = psx_sr_u32(r);
    spu->endx = psx_sr_u32(r);
    spu->unk_da0 = psx_sr_u16(r);
    spu->mbase = psx_sr_u16(r);
    spu->irq9addr = psx_sr_u16(r);
    spu->ramdta = psx_sr_u16(r);
    spu->ramdtf = psx_sr_u16(r);
    spu->spucnt = psx_sr_u16(r);
    spu->ramdtc = psx_sr_u16(r);
    spu->spustat = psx_sr_u16(r);
    spu->cdaivol = psx_sr_u32(r);
    spu->extivol = psx_sr_u32(r);
    spu->currvol = psx_sr_u32(r);
    spu->unk_dbc = psx_sr_u32(r);
    spu->dapf1 = psx_sr_u16(r);
    spu->dapf2 = psx_sr_u16(r);
    spu->viir = (int16_t)psx_sr_u16(r);
    spu->vcomb1 = (int16_t)psx_sr_u16(r);
    spu->vcomb2 = (int16_t)psx_sr_u16(r);
    spu->vcomb3 = (int16_t)psx_sr_u16(r);
    spu->vcomb4 = (int16_t)psx_sr_u16(r);
    spu->vwall = (int16_t)psx_sr_u16(r);
    spu->vapf1 = (int16_t)psx_sr_u16(r);
    spu->vapf2 = (int16_t)psx_sr_u16(r);
    spu->mlsame = psx_sr_u16(r);
    spu->mrsame = psx_sr_u16(r);
    spu->mlcomb1 = psx_sr_u16(r);
    spu->mrcomb1 = psx_sr_u16(r);
    spu->mlcomb2 = psx_sr_u16(r);
    spu->mrcomb2 = psx_sr_u16(r);
    spu->dlsame = psx_sr_u16(r);
    spu->drsame = psx_sr_u16(r);
    spu->mldiff = psx_sr_u16(r);
    spu->mrdiff = psx_sr_u16(r);
    spu->mlcomb3 = psx_sr_u16(r);
    spu->mrcomb3 = psx_sr_u16(r);
    spu->mlcomb4 = psx_sr_u16(r);
    spu->mrcomb4 = psx_sr_u16(r);
    spu->dldiff = psx_sr_u16(r);
    spu->drdiff = psx_sr_u16(r);
    spu->mlapf1 = psx_sr_u16(r);
    spu->mrapf1 = psx_sr_u16(r);
    spu->mlapf2 = psx_sr_u16(r);
    spu->mrapf2 = psx_sr_u16(r);
    spu->vlin = (int16_t)psx_sr_u16(r);
    spu->vrin = (int16_t)psx_sr_u16(r);

    spu->taddr = psx_sr_u32(r);
    psx_sr_u16_array(r, spu->tfifo, 32);
    spu->tfifo_index = psx_sr_u16(r);
    spu->revbaddr = psx_sr_u32(r);
    spu->lrsl = psx_sr_i32(r);
    spu->lrsr = psx_sr_i32(r);
    spu->even_cycle = psx_sr_i32(r);

    for (i = 0; i < 24; i++) {
        spu->data[i].playing = psx_sr_i32(r);
        spu->data[i].counter = psx_sr_u32(r);
        spu->data[i].current_addr = psx_sr_u32(r);
        spu->data[i].repeat_addr = psx_sr_u32(r);
        spu->data[i].prev_sample_index = psx_sr_u32(r);

        for (j = 0; j < 4; j++)
            spu->data[i].s[j] = (int16_t)psx_sr_u16(r);

        spu->data[i].block_flags = psx_sr_i32(r);
        psx_sr_i16_array(r, spu->data[i].buf, 28);

        for (j = 0; j < 2; j++)
            spu->data[i].h[j] = (int16_t)psx_sr_u16(r);

        spu->data[i].lvol = psx_sr_f32(r);
        spu->data[i].rvol = psx_sr_f32(r);
        spu->data[i].cvol = psx_sr_i32(r);
        spu->data[i].eon = psx_sr_i32(r);
        spu->data[i].reverbl = psx_sr_i32(r);
        spu->data[i].reverbr = psx_sr_i32(r);

        spu->data[i].adsr_phase = psx_sr_i32(r);
        spu->data[i].adsr_cycles_reload = psx_sr_i32(r);
        spu->data[i].adsr_cycles = psx_sr_i32(r);
        spu->data[i].adsr_mode = psx_sr_i32(r);
        spu->data[i].adsr_dir = psx_sr_i32(r);
        spu->data[i].adsr_shift = psx_sr_i32(r);
        spu->data[i].adsr_step = psx_sr_i32(r);
        spu->data[i].adsr_pending_step = psx_sr_i32(r);
        spu->data[i].adsr_sustain_level = psx_sr_i32(r);
        spu->data[i].envctl = psx_sr_u32(r);

        /* Not in the payload (the save-state format is deliberately unchanged), so it is reset
           to a defined value rather than inherited from whatever this instance was doing. The
           next write to 1F801C0Eh+N*10h re-establishes it. */
        spu->data[i].ignore_loop_addr = 0;
    }

    /* Samples already generated describe the machine that was just thrown away. Dropping them
       also drops the budget, so the frame the load lands in generates nothing further and the
       front-end fills it inline — one frame of the old pull model, then back to normal. */
    psx_spu_flush_samples(spu);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_spu_destroy(psx_spu_t* spu) {
    free(spu->ram);
    free(spu);
}

// To-do: Optimize reverb

int16_t spu_read_reverb(psx_spu_t* spu, uint32_t addr) {
    uint32_t mbase = spu->mbase << 3;

    uint32_t relative = (addr + spu->revbaddr - mbase) % (0x80000 - mbase);
    uint32_t wrapped = (mbase + relative) & 0x7fffe;

    return (int16_t)spu_ram_read16(spu, wrapped);
}

void spu_write_reverb(psx_spu_t* spu, uint32_t addr, int16_t value) {
    uint32_t mbase = spu->mbase << 3;

    uint32_t relative = (addr + spu->revbaddr - mbase) % (0x80000 - mbase);
    uint32_t wrapped = (mbase + relative) & 0x7fffe;

    spu_ram_write16(spu, wrapped, (uint16_t)value);
}

#define R16(addr) (spu_read_reverb(spu, addr))
#define W16(addr, value) spu_write_reverb(spu, addr, value)

#define SAT(v) CLAMP(v, INT16_MIN, INT16_MAX)

void spu_get_reverb_sample(psx_spu_t* spu, int inl, int inr, int* outl, int* outr) {
    uint32_t mbase = spu->mbase << 3;
    uint32_t dapf1 = spu->dapf1 << 3;
    uint32_t dapf2 = spu->dapf2 << 3;
    uint32_t mlsame = spu->mlsame << 3;
    uint32_t mrsame = spu->mrsame << 3;
    uint32_t dlsame = spu->dlsame << 3;
    uint32_t drsame = spu->drsame << 3;
    uint32_t mldiff = spu->mldiff << 3;
    uint32_t mrdiff = spu->mrdiff << 3;
    uint32_t dldiff = spu->dldiff << 3;
    uint32_t drdiff = spu->drdiff << 3;
    uint32_t mlcomb1 = spu->mlcomb1 << 3;
    uint32_t mlcomb2 = spu->mlcomb2 << 3;
    uint32_t mlcomb3 = spu->mlcomb3 << 3;
    uint32_t mlcomb4 = spu->mlcomb4 << 3;
    uint32_t mrcomb1 = spu->mrcomb1 << 3;
    uint32_t mrcomb2 = spu->mrcomb2 << 3;
    uint32_t mrcomb3 = spu->mrcomb3 << 3;
    uint32_t mrcomb4 = spu->mrcomb4 << 3;
    uint32_t mlapf1 = spu->mlapf1 << 3;
    uint32_t mlapf2 = spu->mlapf2 << 3;
    uint32_t mrapf1 = spu->mrapf1 << 3;
    uint32_t mrapf2 = spu->mrapf2 << 3;

    float vlin = (float)spu->vlin;
    float vrin = (float)spu->vrin;
    float viir = (float)spu->viir;
    float vwall = (float)spu->vwall;
    float vcomb1 = (float)spu->vcomb1;
    float vcomb2 = (float)spu->vcomb2;
    float vcomb3 = (float)spu->vcomb3;
    float vcomb4 = (float)spu->vcomb4;
    float vapf1 = (float)spu->vapf1;
    float vapf2 = (float)spu->vapf2;
    float vlout = (float)spu->vlout;
    float vrout = (float)spu->vrout;

    int lin = (vlin * inl) / 32768.0f;
    int rin = (vrin * inr) / 32768.0f;

    // same side reflection ltol and rtor
    int16_t mlsamev = SAT(lin + ((R16(dlsame) * vwall) / 32768.0f) - ((R16(mlsame - 2) * viir) / 32768.0f) + R16(mlsame - 2));
    int16_t mrsamev = SAT(rin + ((R16(drsame) * vwall) / 32768.0f) - ((R16(mrsame - 2) * viir) / 32768.0f) + R16(mrsame - 2));
    W16(mlsame, mlsamev);
    W16(mrsame, mrsamev);

    /* `audio_diag`. mLSAME/mRSAME IS the feedback line: it is written here and read back on the
       next tick through R16(mlsame - 2). A loop that has railed pins this at full scale and then
       decays only as fast as vIIR/vWALL allow, which is exactly the shape of a sustained tone
       that outlives the sound that started it. Read AFTER SAT() so the arithmetic is untouched;
       see PSX_AUDIO_DIAG_RAILED in psx/perf.h for why post-clamp is enough. */
    if (g_psx_audio_diag_enabled) {
        g_psx_audio_diag.rev_calls++;

        if (PSX_AUDIO_DIAG_RAILED(mlsamev) || PSX_AUDIO_DIAG_RAILED(mrsamev))
            g_psx_audio_diag.revfb_railed++;
    }

    // different side reflection ltor and rtol
    int16_t mldiffv = SAT(lin + ((R16(drdiff) * vwall) / 32768.0f) - ((R16(mldiff - 2) * viir) / 32768.0f) + R16(mldiff - 2));
    int16_t mrdiffv = SAT(rin + ((R16(dldiff) * vwall) / 32768.0f) - ((R16(mrdiff - 2) * viir) / 32768.0f) + R16(mrdiff - 2));
    W16(mldiff, mldiffv);
    W16(mrdiff, mrdiffv);

    // early echo (comb filter with input from buffer)
    int16_t l = SAT((vcomb1 * R16(mlcomb1) / 32768.0f) + (vcomb2 * R16(mlcomb2) / 32768.0f) + (vcomb3 * R16(mlcomb3) / 32768.0f) + (vcomb4 * R16(mlcomb4) / 32768.0f));
    int16_t r = SAT((vcomb1 * R16(mrcomb1) / 32768.0f) + (vcomb2 * R16(mrcomb2) / 32768.0f) + (vcomb3 * R16(mrcomb3) / 32768.0f) + (vcomb4 * R16(mrcomb4) / 32768.0f));

    // late reverb apf1 (all pass filter 1 with input from comb)
    l = SAT(l - SAT((vapf1 * R16(mlapf1 - dapf1)) / 32768.0f));
    r = SAT(r - SAT((vapf1 * R16(mrapf1 - dapf1)) / 32768.0f));

    W16(mlapf1, l);
    W16(mrapf1, r);
    
    l = SAT((l * vapf1 / 32768.0f) + R16(mlapf1 - dapf1));
    r = SAT((r * vapf1 / 32768.0f) + R16(mrapf1 - dapf1));

    // late reverb apf2 (all pass filter 2 with input from apf1)
    l = SAT(l - SAT((vapf2 * R16(mlapf2 - dapf2)) / 32768.0f));
    r = SAT(r - SAT((vapf2 * R16(mrapf2 - dapf2)) / 32768.0f));
    
    W16(mlapf2, l);
    W16(mrapf2, r);

    l = SAT((l * vapf2 / 32768.0f) + R16(mlapf2 - dapf2));
    r = SAT((r * vapf2 / 32768.0f) + R16(mrapf2 - dapf2));

    // output to mixer (output volume multiplied with input from apf2)
    *outl = SAT(l * vlout / 32768.0f);
    *outr = SAT(r * vrout / 32768.0f);

    if (g_psx_audio_diag_enabled &&
        (PSX_AUDIO_DIAG_RAILED(*outl) || PSX_AUDIO_DIAG_RAILED(*outr)))
        g_psx_audio_diag.revout_railed++;

    spu->revbaddr = MAX(mbase, (spu->revbaddr + 2) & 0x7fffe);
}

#undef R16
#undef W16

/*
    Main Volume Left / Right (1F801D80h / 1F801D82h).

    These are in the Volume/Sweep format — the SAME one the voice volumes use — and NOT the
    plain signed-16 format the reverb volumes use. In volume mode (bit15 clear) bits 14-0 hold
    Volume/2, so the gain is value/4000h and 3FFFh is unity.

    This used to be `* (float)spu->mainlvol / 32767.0f`, i.e. the register read as an unsigned
    magnitude against a plain signed-16 full scale. With the 3FFFh that games actually write
    that is 16383/32767 = 0.49999 — exactly HALF the intended level, a flat 6.02 dB loss on
    everything the SPU produces.

    Three independent confirmations, not a reading of the spec alone:
      * MEASURED. In a 30 s MGS codec capture the ratio of the pre-clamp dry accumulator to the
        host mix output was 2.0000 in all 49 snapshots (min 2.0002, max 2.0013), with mainvol
        pinned at 3FFFh, dry_clip=0 and mix wrap=0 throughout. Exactly one halving, right here.
      * INTERNALLY INCONSISTENT. spu_kon() already encodes the same /4000h for the identically
        formatted VOICE volume registers, as "/32767.0f * 2.0f". The x2 is simply missing here.
      * CONSISTENT WITH THE REVERB PATH. spu_get_reverb_sample() divides vLOUT/vRIN/etc by
        32768, which is correct for those registers because they ARE plain signed 16-bit. The
        two formats want different divisors and this one had the wrong one.

    Also decodes the sign properly. Bits 14-0 are a SIGNED 15-bit field, so a negative main
    volume (phase inversion) previously read as a large positive unsigned value and became a
    gain of up to +2.0 with the wrong polarity. Sign-extending bit 14 is the same idiom the XA
    decoder uses for its 4-bit nibbles. Sweep mode (bit15 set) remains unimplemented, but the
    masked magnitude keeps the gain bounded to +/-1.0 instead of the >2x a raw unsigned read
    produced, so this is strictly safer there too.

    Cannot overflow the int16 result: the caller clamps to int16 BEFORE this scale is applied
    and |gain| <= 1.0, so the product stays inside int16 whatever the mix does.
*/
static inline float spu_volume_gain(uint16_t reg) {
    int16_t v = (int16_t)(uint16_t)(reg << 1) >> 1;

    return (float)v / 16384.0f;
}

uint32_t psx_spu_get_sample(psx_spu_t* spu) {
    spu->even_cycle ^= 1;

    int left = 0;
    int right = 0;
    int revl = 0;
    int revr = 0;
    /* `audio_diag` only. Counted unconditionally because one increment on a path that already
       runs the Gaussian interpolator is cheaper than the branch that would skip it. */
    int voices = 0;

    spu->koff = 0;
    spu->kon = 0;

    /* Accumulated per generated sample, not per frame: the host divides voice_samples by
       samples to get the mean number of voices that were actually sounding. Sampling once
       a frame would miss everything that keys on and off inside it. */
    PSX_PERF_INC(spu_samples);

    for (int v = 0; v < VOICE_COUNT; v++) {
        if (!spu->data[v].playing)
            continue;

        ++voices;

        PSX_PERF_INC(spu_voice_samples);

        spu_handle_adsr(spu, v);

        uint32_t sample_index = spu->data[v].counter >> 12;

        if (sample_index > 27) {
            sample_index -= 28;

            spu->data[v].counter &= 0xfff;
            spu->data[v].counter |= sample_index << 12;

            /*
                Loop Start (flag bit 2) copies the current address into the voice's REPEAT
                ADDRESS REGISTER, 1F801C0Eh. Writing spu->voice[v].adraddr rather than a private
                copy is what makes the register the single source of truth — see the loop-end
                cases below for why that matters. data[v].repeat_addr is kept in step purely so
                the save-state payload in this file stays byte-compatible; nothing reads it now.
            */
            if ((spu->data[v].block_flags & 4) && !spu->data[v].ignore_loop_addr) {
                spu->voice[v].adraddr = (uint16_t)(spu->data[v].current_addr >> 3);
                spu->data[v].repeat_addr = spu->data[v].current_addr;
            }

            /*
                The repeat address is read LIVE from 1F801C0Eh, for exactly the same reason the
                voice volume now is: it used to be a snapshot taken at key-on (spu_kon) and only
                ever refreshed by a Loop Start flag, so a game writing 1F801C0Eh during playback
                was ignored. Redirecting the repeat address mid-stream is how double-buffered
                streaming works — the game refills the half that just finished and points the
                loop at the other one — so a streaming voice would jump back to whatever address
                was current at a key-on long past, replay audio it had already played, and then
                die the moment it wandered onto a block flagged end-without-repeat.

                That is precisely the reported symptom: MGS codec speech plays for a few seconds,
                repeats itself, then stops for good.
            */
            const uint32_t repeat_addr = spu_wrap_addr((uint32_t)spu->voice[v].adraddr << 3);

            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.voice_flags[v] = (uint8_t)spu->data[v].block_flags;
            }

            switch (spu->data[v].block_flags & 3) {
                case 0: case 2: {
                    /* The two inline IRQ checks that used to live here moved into
                       spu_read_block(), which every branch of this switch calls on the way out —
                       so the loop-end jumps below are covered too, and a block is no longer
                       tested twice per advance. */
                    spu->data[v].current_addr = spu_wrap_addr(spu->data[v].current_addr + 0x10);
                } break;

                case 1: {
                    /* Loop End with Loop Repeat clear. Flag bit 0 means "set ENDX and jump to
                       the repeat address" on its own; bit 1 only decides whether the voice is
                       ALSO forced into release. ENDX was missing here and set only in case 3,
                       so a stream ending on an unrepeated block never raised the flag the game
                       polls at 1F801D9Ch to learn that a buffer half had been consumed — it
                       would simply never refill. */
                    spu->endx |= 1 << v;
                    spu->data[v].current_addr = repeat_addr;
                    spu->data[v].playing = 0;
                    spu->voice[v].envcvol = 0;

                    if (g_psx_audio_diag_enabled)
                        g_psx_audio_diag.voice_stop[v]++;

                    adsr_load_release(spu, v);
                } break;

                case 3: {
                    spu->endx |= 1 << v;
                    spu->data[v].current_addr = repeat_addr;

                    if (g_psx_audio_diag_enabled)
                        g_psx_audio_diag.voice_loopend[v]++;
                } break;
            }

            spu_read_block(spu, v);
        }

        //  Fetch ADPCM sample
        if (spu->data[v].prev_sample_index != sample_index) {
            spu->data[v].s[3] = spu->data[v].s[2];
            spu->data[v].s[2] = spu->data[v].s[1];
            spu->data[v].s[1] = spu->data[v].s[0];
        }

        spu->data[v].s[0] = spu->data[v].buf[sample_index];

        // Apply 4-point Gaussian interpolation
        uint8_t gauss_index = (spu->data[v].counter >> 4) & 0xff;
        int16_t g0 = g_spu_gauss_table[0x0ff - gauss_index];
        int16_t g1 = g_spu_gauss_table[0x1ff - gauss_index];
        int16_t g2 = g_spu_gauss_table[0x100 + gauss_index];
        int16_t g3 = g_spu_gauss_table[0x000 + gauss_index];
        int16_t out = spu->data[v].s[0];

        // out = interpolate_hermite(
        //     spu->data[v].s[3],
        //     spu->data[v].s[2],
        //     spu->data[v].s[1],
        //     spu->data[v].s[0],
        //     (spu->data[v].counter & 0xfff) / 4096.0f
        // );

        out  = (g0 * spu->data[v].s[3]) >> 15;
        out += (g1 * spu->data[v].s[2]) >> 15;
        out += (g2 * spu->data[v].s[1]) >> 15;
        out += (g3 * spu->data[v].s[0]) >> 15;

        float adsr_vol = (float)spu->voice[v].envcvol / 32767.0f;

        /*
            Voice volume is read LIVE from 1F801C00h+N*10h / 1F801C02h+N*10h.

            It used to come from spu->data[v].lvol/rvol, which are written in exactly ONE place
            — spu_kon() — so the mixer's per-voice gain was a snapshot taken at key-on and never
            updated again. Nothing in spu_handle_write() refreshes them; a write to a voice
            volume register updates spu->voice[v].volumel and the mixer carried on with the old
            cached float. On hardware these registers take effect immediately (in sweep mode
            they ramp continuously), which is exactly why games write them during playback.

            That is silent for one-shot sounds, because those are keyed on AFTER their volume is
            set, so the snapshot happens to be right. It is fatal for a STREAMING voice, which is
            keyed on once and then left running for minutes while the game drives its level
            through the registers: the pair carrying MGS's codec speech had been sounding for
            over 30 s (env=7fff, phase=SUSTAIN, unchanged across all 60 snapshots of a capture)
            with the registers reading 3fff/0000 and 0000/3fff, while the mixer used whatever
            those registers held at a key-on far in the past.

            spu_volume_gain() is the same Volume/Sweep decode the main volume uses — the two
            registers share a format — so this also fixes the sign: the old expression read the
            register as unsigned, turning a negative (phase-inverted) voice volume into a large
            positive gain instead of an inverted one.

            data[].lvol/rvol are still written by spu_kon() and still saved: psx/state.c has them
            in the save-state payload, and changing that would break existing states for a field
            the mixer no longer consults.
        */
        float samplel = (out * spu_volume_gain(spu->voice[v].volumel)) * adsr_vol;
        float sampler = (out * spu_volume_gain(spu->voice[v].volumer)) * adsr_vol;

        left += samplel;
        right += sampler;

        /* `audio_diag`: what this voice actually contributed, post-decode and pre-mix. The one
           number that separates "decode produced silence" from "the audio exists and something
           downstream dropped it". */
        if (g_psx_audio_diag_enabled) {
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.voice_peak_l[v], (int32_t)samplel);
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.voice_peak_r[v], (int32_t)sampler);
        }

        if (spu->eon & (1 << v)) {
            revl += samplel;
            revr += sampler;
        }

        uint16_t step = spu->voice[v].adsampr;

        /* To-do: Do pitch modulation here */

        spu->data[v].prev_sample_index = spu->data[v].counter >> 12;
        spu->data[v].counter += step;
    }

    int16_t clamprl = CLAMP(revl, INT16_MIN, INT16_MAX);
    int16_t clamprr = CLAMP(revr, INT16_MIN, INT16_MAX);
    int16_t clampsl = CLAMP(left, INT16_MIN, INT16_MAX);
    int16_t clampsr = CLAMP(right, INT16_MIN, INT16_MAX);

    /* `audio_diag`. Peaks are taken from the PRE-clamp accumulators on purpose — how far past
       full scale the mix runs is the whole question, and the clamp below destroys that. */
    if (g_psx_audio_diag_enabled) {
        g_psx_audio_diag.spu_samples++;
        g_psx_audio_diag.spu_voices_sum += (uint64_t)voices;

        if ((uint32_t)voices > g_psx_audio_diag.spu_voices_peak)
            g_psx_audio_diag.spu_voices_peak = (uint32_t)voices;

        PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.dry_peak_l, left);
        PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.dry_peak_r, right);
        PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.revin_peak_l, revl);
        PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.revin_peak_r, revr);

        if ((left != clampsl) || (right != clampsr))
            g_psx_audio_diag.dry_clip++;

        if ((revl != clamprl) || (revr != clamprr))
            g_psx_audio_diag.revin_clip++;

        g_psx_audio_diag.mainvol_l = (int32_t)spu->mainlvol;
        g_psx_audio_diag.mainvol_r = (int32_t)spu->mainrvol;

        if ((uint32_t)spu->mainlvol < g_psx_audio_diag.mainvol_min_l)
            g_psx_audio_diag.mainvol_min_l = spu->mainlvol;

        if ((uint32_t)spu->mainrvol < g_psx_audio_diag.mainvol_min_r)
            g_psx_audio_diag.mainvol_min_r = spu->mainrvol;

        if ((spu->spucnt & 0x4000) == 0)
            g_psx_audio_diag.spu_silent++;
    }

    if ((spu->spucnt & 0x4000) == 0)
        return 0;

    uint16_t clampl;
    uint16_t clampr;

    if ((spu->spucnt & 0x0080) && !spu->reverb_disabled) {
        if (spu->even_cycle)
            spu_get_reverb_sample(spu, clamprl, clamprr, &spu->lrsl, &spu->lrsr);

        /* `audio_diag`: does the dry+wet sum clip, and how loud is the wet return on its own?
           Symptom "a grating tone that outlives the shot that caused it" lives or dies here. */
        if (g_psx_audio_diag_enabled) {
            const int sum_l = clampsl + spu->lrsl;
            const int sum_r = clampsr + spu->lrsr;

            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.revout_peak_l, spu->lrsl);
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.revout_peak_r, spu->lrsr);

            if ((sum_l > INT16_MAX) || (sum_l < INT16_MIN) ||
                (sum_r > INT16_MAX) || (sum_r < INT16_MIN))
                g_psx_audio_diag.revsum_clip++;
        }

        clampl = CLAMP((clampsl + spu->lrsl), INT16_MIN, INT16_MAX) * spu_volume_gain(spu->mainlvol);
        clampr = CLAMP((clampsr + spu->lrsr), INT16_MIN, INT16_MAX) * spu_volume_gain(spu->mainrvol);
    } else {
        clampl = CLAMP(clampsl, INT16_MIN, INT16_MAX) * spu_volume_gain(spu->mainlvol);
        clampr = CLAMP(clampsr, INT16_MIN, INT16_MAX) * spu_volume_gain(spu->mainrvol);
    }

    return clampl | (((uint32_t)clampr) << 16);
}

/*
    ===========================================================================================
    SPU sample generation on the CPU's timeline.
    ===========================================================================================

    THE BUG THIS EXISTS TO FIX (measured, not inferred — /tmp/mgs7.txt, MGS codec call):

    psx_spu_get_sample() used to be driven exclusively by the front-end, in one burst of a
    whole frame's worth of samples taken AFTER psx_update() had already finished the frame.
    The audio itself was fine. The interrupts were not: every Sound RAM IRQ that
    spu_check_irq() raised inside that burst landed while the CPU was stopped, so all of
    them collapsed into the single pending IC_SPU bit the CPU saw at the frame boundary.

    The SPU interrupt rate was therefore hard-capped at the VIDEO frame rate, whatever the
    voices were actually doing. That is not a rounding error, it is a different clock.

    MGS streams codec speech through a textbook double-buffered SPU ring and clocks the
    refill off that interrupt:

      - voice 23 is a SILENT metronome (vol=0000/0000) over a 0x200-byte ring at 44100 Hz,
        so it wraps 49.2 times a second;
      - the game alternates SPUIRQA between the ring's two halves (0x1010 / 0x1110 in the
        capture, 28/30 split across 58 snapshots), i.e. 2 interrupts per wrap = 98.4/s;
      - every 32nd interrupt it uploads the next 4096 bytes into each of the two speech
        voices (21 and 22), which is 256 ADPCM blocks = 7168 samples = 0.325 s at their
        pitch of 0x800 (22050 Hz). 32 interrupts is 16 wraps is 0.325 s. The two agree to
        four figures: the design wants 98.4 interrupts per second and nothing else.

    It was getting 57.9 (1678 raises over 29 s), pinned to the 59.29 Hz frame rate exactly as
    the burst model predicts. 57.9/98.4 = 0.588. And the delivered data rate was 7409 bytes/s
    per voice against the 12600 bytes/s those voices consume: 0.588. The interrupt deficit and
    the audio deficit are the same number to three digits, in all three captures.

    The missing 41% is not silence — the ring simply wraps onto sound RAM the game has not
    got round to overwriting yet, so 0.325 s of speech plays a second time before the refill
    lands and the sentence continues. "you'll have to take have to take".

    THE FIX: charge the SPU the same device cycles as every other peripheral, from
    psx_update(), and let it spend a per-frame sample budget across the frame instead of all
    at once. The CPU then runs between samples, services each interrupt, and re-arms SPUIRQA
    before the metronome has run past the next target.

    WHY A BUDGET RATHER THAN A FREE-RUNNING CLOCK: the front-end pulls exactly
    mix_rate/frame_rate samples per emulated frame and resamples afterwards for fast-forward.
    Generating on a free-running divider would make the count per frame depend on how many
    cycles the GPU happened to take to reach vblank, and any mismatch would accumulate into
    underruns or a growing backlog. The budget makes the per-frame total bit-for-bit what it
    was before this change; only WHEN inside the frame each sample is produced has moved.
    Frames that reach vblank in fewer cycles than the budget covers leave a shortfall, which
    the front-end generates inline exactly as it always did.

    768 is exact, not a rounding: PSX_CPU_CPS is 33868800 = 44100 * 768. No accumulator drift.
*/
#define SPU_CYCLES_PER_SAMPLE 768

void psx_spu_begin_frame(psx_spu_t* spu, int sample_budget) {
    /* A frame always drains exactly what it granted, so anything still here belongs to a frame
       whose pull never happened — the audio device closed under it, or a state load landed in
       the middle of it. Those samples describe a timeline the host has already moved past, and
       carrying them forward would offset every later frame by their count, permanently. The
       divider remainder is NOT reset with them: it is sub-sample phase, it is still valid, and
       throwing it away every frame would cost one generated sample in most of them. */
    spu->gen_head = 0;
    spu->gen_tail = 0;
    spu->gen_count = 0;

    if (sample_budget < 0)
        sample_budget = 0;

    /* Clamped rather than trusted. The ring is the only thing standing between a bogus
       frame-rate value and a write past the end of it. */
    if (sample_budget > PSX_SPU_SAMPLE_RING)
        sample_budget = PSX_SPU_SAMPLE_RING;

    spu->gen_budget = sample_budget;
}

void psx_spu_tick(psx_spu_t* spu, uint32_t cycles) {
    /* Zero budget is the normal state for every psx_update() the front-end drives outside a
       real frame — runahead's re-simulated frames, the EXE boot spin, rewind. Those must not
       generate audio, and returning before touching the divider means they do not perturb the
       phase of the frame that follows either. */
    if (spu->gen_budget <= 0)
        return;

    spu->gen_cycles += cycles;

    while (spu->gen_cycles >= SPU_CYCLES_PER_SAMPLE) {
        spu->gen_cycles -= SPU_CYCLES_PER_SAMPLE;

        if (spu->gen_budget <= 0)
            break;

        if (spu->gen_count >= PSX_SPU_SAMPLE_RING)
            break;

        spu->gen_ring[spu->gen_head] = psx_spu_get_sample(spu);
        spu->gen_head = (spu->gen_head + 1) & (PSX_SPU_SAMPLE_RING - 1);
        spu->gen_count++;
        spu->gen_budget--;

        if (g_psx_audio_diag_enabled)
            g_psx_audio_diag.spu_gen_ticked++;
    }
}

int psx_spu_pop_sample(psx_spu_t* spu, uint32_t* out) {
    if (spu->gen_count <= 0)
        return 0;

    *out = spu->gen_ring[spu->gen_tail];
    spu->gen_tail = (spu->gen_tail + 1) & (PSX_SPU_SAMPLE_RING - 1);
    spu->gen_count--;

    return 1;
}

void psx_spu_flush_samples(psx_spu_t* spu) {
    spu->gen_head = 0;
    spu->gen_tail = 0;
    spu->gen_count = 0;
    spu->gen_budget = 0;
    spu->gen_cycles = 0;
}

int counter = 0;

/* One CD sector is 2352 bytes = 588 stereo frames, and psx/dev/cdrom/cdrom.h sizes the only
   buffer ever passed in here — psx_cdrom_t::cdda_buf — as exactly int16_t[CD_SECTOR_SIZE >> 1],
   i.e. 1176 samples. Kept as a named constant rather than derived because spu.c must not
   include the CD-ROM headers. */
#define SPU_CDDA_SECTOR_FRAMES 588

void psx_spu_update_cdda_buffer(psx_spu_t* spu, void* buf) {
    int16_t* ptr = buf;
    int16_t* ram = (int16_t*)spu->ram;

    /* Bounded read. This loop ran 0x400 = 1024 frames unconditionally, taking 2048 int16 out
       of a 1176-int16 buffer: 872 samples, 1744 bytes, past the end of cdda_buf every single
       call. What it actually copied into SPU RAM as "CD audio" was the psx_cdrom_t fields that
       follow cdda_buf in the struct, and then the head of the raw XA sector buffer.

       This is a memory-safety fix only. It is NOT claimed to resolve any reported symptom, and
       the SEPARATE question of whether the destination regions are right is deliberately left
       alone: the hardware capture buffers are 1024 BYTES each at 0x000 and 0x400, while the
       int16 indexing below spans bytes 0x000-0x7FF and 0x800-0xFFF. Changing that without
       measurement is exactly the guess this investigation is trying not to make. */
    for (int i = 0; i < SPU_CDDA_SECTOR_FRAMES; i++) {
        ram[i + 0x000] = *ptr++;
        ram[i + 0x400] = *ptr++;
    }

    // Little bit of lowpass/smoothing
    for (int i = 0; i < 0x400; i += 8) {
        int l = 0, r = 0;

        for (int j = 0; j < 8; j++) {
            l += ram[i + j];
            r += ram[i + j + 0x400];
        }

        ram[i + 0x000] = l / 8;
        ram[i + 0x400] = r / 8;
    }

    // Simulate capture IRQ
    if (spu->ramdtc & 0xc) {
        if (spu->irq9addr <= 0x1ff) {
            if (!counter) {
                psx_ic_irq(spu->ic, IC_SPU);
            }

            counter++;
            counter &= 0x1;
        }
    }
}

#undef CLAMP
#undef MAX
