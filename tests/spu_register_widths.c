/*
    SPU REGISTER ACCESS WIDTH — 8, 16 and 32 bit against one 16-bit-wide register file.

    WHY THIS FILE EXISTS

    psx/dev/spu.c implemented 16- and 32-bit register access and nothing else. An 8-bit read
    logged "Unhandled 8-bit SPU read at offset ..." at FATAL and returned 0; an 8-bit write
    logged and was thrown away. Xenogears reads voice ADSR registers a byte at a time —
    offsets 58h, 98h, 118h, 138h, i.e. 1F801C08h+N*10h for voices 5, 9, 17 and 19 — several
    times a second. It was polling envelope configuration and being told zero, forever, and
    filling the log at 1.1 MB per few minutes while it did.

    A width bug is invisible to every other kind of test. The emulator does not crash, audio
    does not obviously stop, and the 16-bit path that everything else exercises stays
    correct. It only shows up if something asks, in as many words, "what does a byte read of
    this register return" — which is what this file is.

    THE INVARIANTS

      1. The register file is 16 bits wide. A byte access resolves to the containing
         halfword: low byte at an even offset, high byte at an odd one. There is no
         byte-addressable storage behind it, so nothing else is possible.
      2. A byte WRITE is a read-modify-write of that halfword, and must still reach the
         registers that have behaviour attached (KON, KOFF, the transfer address).
      3. Access outside the register file — the window is 400h wide, the file is 200h —
         must read 0 and must not write. Past the end of the mirror lies the rest of
         psx_spu_t: the transfer address, the transfer FIFO, and the 24 voice decoder
         contexts. That is the part with teeth, and it is invisible from the outside, so
         the cases below reach into the struct and check those fields directly.

    Pure rule: one psx_spu_t, no bus, no CPU, no interrupt controller, no audio device.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../psx/dev/spu.h"

static int g_failed;

static void check(const char* name, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "SPU_WIDTH failed case=%s got=%08x want=%08x\n", name, got, want);
        g_failed = 1;

        return;
    }

    printf("SPU_WIDTH passed case=%s\n", name);
}

/* Quieter variant for the cases that assert the same rule across all 24 voices — one line
   per voice register would be 192 lines of noise. Reports once, at the end of the sweep. */
static int g_sweep_failed;

static void check_quiet(const char* what, uint32_t offset, uint32_t got, uint32_t want) {
    if (got == want)
        return;

    fprintf(stderr, "SPU_WIDTH sweep %s offset=%03x got=%08x want=%08x\n",
            what, offset, got, want);
    g_sweep_failed = 1;
}

static void sweep_result(const char* name) {
    check(name, g_sweep_failed ? 1u : 0u, 0u);
    g_sweep_failed = 0;
}

static psx_spu_t* fresh(void) {
    psx_spu_t* spu = psx_spu_create();

    psx_spu_init(spu, NULL);

    return spu;
}

/* Voice N register R lives at N*10h + R*2. Offset 0Ch of voice 0 is excluded from the
   round-trip sweep on purpose: psx_spu_write16() has always refused that one write (envcvol
   is the ADSR level the mixer owns, not something the CPU sets), and case
   voice0-envcvol-write-is-refused below pins that behaviour explicitly instead. */
#define VOICE_REG(v, r) (((uint32_t)(v) * 0x10u) + ((uint32_t)(r) * 2u))

int main(void) {
    psx_spu_t* spu = fresh();

    /* ---- 1. THE REGRESSION ITSELF -----------------------------------------------------
       Every voice register, every byte half. A 16-bit write goes in; the two byte reads
       must take it apart the way hardware does. This is the case that would have caught
       the Xenogears report. */
    for (int v = 0; v < 24; v++) {
        for (int r = 0; r < 8; r++) {
            const uint32_t off = VOICE_REG(v, r);

            if (off == 0x0c)
                continue;

            /* Distinct per register, and distinct between the halves so a swapped or
               duplicated byte cannot pass. */
            const uint16_t value = (uint16_t)(((0x40u + v) << 8) | (0x80u + r));

            psx_spu_write16(spu, off, value);

            check_quiet("read16", off, psx_spu_read16(spu, off), value);
            check_quiet("read8-lo", off, psx_spu_read8(spu, off), value & 0xffu);
            check_quiet("read8-hi", off + 1u, psx_spu_read8(spu, off + 1u), value >> 8);
        }
    }

    sweep_result("every-voice-register-splits-into-bytes");

    /* The four offsets the user's log actually named, spelled out. 1F801C08h+N*10h is the
       ADSR lower half for voices 5, 9, 17 and 19. */
    {
        static const uint32_t reported[] = { 0x058, 0x098, 0x118, 0x138 };

        for (unsigned i = 0; i < sizeof(reported) / sizeof(reported[0]); i++) {
            const uint32_t off = reported[i];
            const uint16_t value = (uint16_t)(0xa500u | i);

            psx_spu_write16(spu, off, value);

            check_quiet("xenogears-lo", off, psx_spu_read8(spu, off), value & 0xffu);
            check_quiet("xenogears-hi", off, psx_spu_read8(spu, off + 1u), value >> 8);
        }
    }

    sweep_result("reported-adsr-offsets-read-back");

    /* ---- 2. 32-BIT READS WERE ALREADY HANDLED -----------------------------------------
       They were, and they still have to be: a word read is the two halfwords of the pair,
       little-endian. ADSR is the natural case — envctl1/envctl2 are one 32-bit control
       word split across two registers, which is how spu_adsr_cfg() reads them. */
    psx_spu_write16(spu, VOICE_REG(3, 4), 0x1234);
    psx_spu_write16(spu, VOICE_REG(3, 5), 0x89ab);

    check("read32-is-the-halfword-pair", psx_spu_read32(spu, VOICE_REG(3, 4)), 0x89ab1234u);
    check("read32-low-half-agrees", psx_spu_read16(spu, VOICE_REG(3, 4)), 0x1234u);
    check("read32-high-half-agrees", psx_spu_read16(spu, VOICE_REG(3, 5)), 0x89abu);
    check("read32-byte0", psx_spu_read8(spu, VOICE_REG(3, 4) + 0u), 0x34u);
    check("read32-byte1", psx_spu_read8(spu, VOICE_REG(3, 4) + 1u), 0x12u);
    check("read32-byte2", psx_spu_read8(spu, VOICE_REG(3, 4) + 2u), 0xabu);
    check("read32-byte3", psx_spu_read8(spu, VOICE_REG(3, 4) + 3u), 0x89u);

    /* ---- 3. BYTE WRITES COMPOSE ------------------------------------------------------
       Two byte writes build the halfword, in either order, and each one leaves the other
       half alone. A write8 that ignored the existing value would pass the first assertion
       and fail the third. */
    psx_spu_write16(spu, VOICE_REG(7, 0), 0x0000);
    psx_spu_write8(spu, VOICE_REG(7, 0) + 0u, 0xcd);
    psx_spu_write8(spu, VOICE_REG(7, 0) + 1u, 0xab);

    check("byte-writes-compose-lo-then-hi", psx_spu_read16(spu, VOICE_REG(7, 0)), 0xabcdu);

    psx_spu_write16(spu, VOICE_REG(7, 1), 0x0000);
    psx_spu_write8(spu, VOICE_REG(7, 1) + 1u, 0xfe);
    psx_spu_write8(spu, VOICE_REG(7, 1) + 0u, 0x10);

    check("byte-writes-compose-hi-then-lo", psx_spu_read16(spu, VOICE_REG(7, 1)), 0xfe10u);

    psx_spu_write16(spu, VOICE_REG(7, 2), 0x5a5a);
    psx_spu_write8(spu, VOICE_REG(7, 2) + 0u, 0x99);

    check("byte-write-preserves-high-half", psx_spu_read16(spu, VOICE_REG(7, 2)), 0x5a99u);

    psx_spu_write16(spu, VOICE_REG(7, 3), 0x5a5a);
    psx_spu_write8(spu, VOICE_REG(7, 3) + 1u, 0x99);

    check("byte-write-preserves-low-half", psx_spu_read16(spu, VOICE_REG(7, 3)), 0x995au);

    /* The one voice register psx_spu_write16() has always refused. Pinned so the width
       work above cannot be blamed for it later, and so a byte write cannot sneak in
       through the back door of the read-modify-write path. */
    psx_spu_write16(spu, 0x0c, 0x0000);
    psx_spu_write16(spu, 0x0c, 0x7fff);

    check("voice0-envcvol-write-is-refused", psx_spu_read16(spu, 0x0c), 0x0000u);

    psx_spu_write8(spu, 0x0c, 0x33);

    check("voice0-envcvol-byte-write-is-refused", psx_spu_read16(spu, 0x0c), 0x0000u);

    psx_spu_destroy(spu);

    /* ---- 4. BYTE WRITES STILL REACH THE REGISTERS WITH BEHAVIOUR ----------------------
       A merged halfword that only ever landed in the mirror would leave KON, KOFF and the
       transfer address dead to byte access — which is the state this change found them in.
       KON and KOFF are write-only and never enter the mirror, so a byte write must key
       exactly the voices that byte names, not whatever the neighbouring byte last held. */
    spu = fresh();

    psx_spu_write8(spu, 0x188, 0x02);           /* KON low byte  -> voice 1  */

    check("byte-kon-low-starts-voice", (uint32_t)spu->data[1].playing, 1u);
    check("byte-kon-low-leaves-others", (uint32_t)spu->data[0].playing, 0u);
    check("byte-kon-low-clears-endx", (spu->endx >> 1) & 1u, 0u);

    psx_spu_write8(spu, 0x189, 0x01);           /* KON high byte -> voice 8  */

    check("byte-kon-high-starts-voice", (uint32_t)spu->data[8].playing, 1u);
    check("byte-kon-high-is-not-voice0", (uint32_t)spu->data[0].playing, 0u);

    psx_spu_write8(spu, 0x18a, 0x02);           /* KON word high -> voice 17 */

    check("byte-kon-upper-word-starts-voice", (uint32_t)spu->data[17].playing, 1u);

    /* KOFF drives the envelope into release. adsr_load_release() sets the ENDX bit for the
       voice, which is the observable the register file exposes. */
    spu->endx = 0;
    psx_spu_write8(spu, 0x18c, 0x02);           /* KOFF low byte -> voice 1  */

    check("byte-koff-releases-voice", (spu->endx >> 1) & 1u, 1u);
    check("byte-koff-leaves-others", (spu->endx >> 8) & 1u, 0u);

    /* 1F801DA6h recomputes the sound-RAM transfer address (value << 3) rather than just
       storing the halfword, so a pair of byte writes has to end up at the same place a
       single 16-bit write would. This is the register the MGS sound-RAM upload path rides
       on; a byte write that produced a different address would be much worse than a
       dropped one. */
    psx_spu_write16(spu, 0x1a6, 0x0000);
    psx_spu_write8(spu, 0x1a6, 0x34);
    psx_spu_write8(spu, 0x1a7, 0x12);

    check("byte-writes-set-transfer-address", spu->taddr, 0x1234u << 3);

    psx_spu_destroy(spu);

    /* ---- 5. THE WINDOW IS TWICE THE REGISTER FILE ------------------------------------
       1F801E00h upwards has no register behind it. Reads must be 0, writes must not land,
       and "must not land" is the whole point: the bytes past the end of the mirror are
       psx_spu_t's own working state. The three fields checked here are the ones a stray
       write used to hit first — offset 200h IS taddr, and it climbs into the FIFO and the
       voice decoder contexts from there. */
    spu = fresh();

    psx_spu_write16(spu, 0x1a6, 0x0abc);        /* a transfer in progress          */
    psx_spu_write16(spu, 0x1a8, 0x1111);        /* three halfwords queued in the   */
    psx_spu_write16(spu, 0x1a8, 0x2222);        /* FIFO, so tfifo_index is not 0   */
    psx_spu_write16(spu, 0x1a8, 0x3333);        /* and a stray write would show    */

    psx_spu_write16(spu, VOICE_REG(0, 3), 0x0200);
    psx_spu_write8(spu, 0x188, 0x01);           /* voice 0 playing, decoder primed */

    const uint32_t taddr_before = spu->taddr;
    const uint32_t fifo_index_before = spu->tfifo_index;
    const uint32_t voice0_addr_before = spu->data[0].current_addr;

    check("setup-fifo-index-is-nonzero", fifo_index_before, 3u);
    check("setup-voice-addr-is-nonzero", voice0_addr_before, 0x0200u << 3);

    check("beyond-file-read8", psx_spu_read8(spu, 0x200), 0u);
    check("beyond-file-read8-odd", psx_spu_read8(spu, 0x3ff), 0u);
    check("beyond-file-read16", psx_spu_read16(spu, 0x200), 0u);
    check("beyond-file-read16-top", psx_spu_read16(spu, 0x3fe), 0u);
    check("beyond-file-read32", psx_spu_read32(spu, 0x200), 0u);
    check("beyond-file-read32-top", psx_spu_read32(spu, 0x3fc), 0u);

    /* A 32-bit read straddling the end of the file is out of bounds as a whole. */
    check("straddling-read32", psx_spu_read32(spu, 0x1fe), 0u);

    psx_spu_write32(spu, 0x200, 0xdeadbeefu);
    psx_spu_write16(spu, 0x204, 0xcafeu);
    psx_spu_write8(spu, 0x208, 0x5a);
    psx_spu_write32(spu, 0x3fc, 0xffffffffu);
    psx_spu_write32(spu, 0x1fe, 0xffffffffu);   /* straddles the end */

    check("beyond-file-write-spares-taddr", spu->taddr, taddr_before);
    check("beyond-file-write-spares-fifo-index", (uint32_t)spu->tfifo_index, fifo_index_before);
    check("beyond-file-write-spares-voice-state",
          spu->data[0].current_addr, voice0_addr_before);
    check("beyond-file-write-is-not-readable", psx_spu_read32(spu, 0x200), 0u);

    /* The last in-range halfword must still work — an off-by-one in the bound would take
       vrin (1F801DFEh) with it. */
    psx_spu_write16(spu, 0x1fe, 0x7ffe);

    check("last-register-still-writable", psx_spu_read16(spu, 0x1fe), 0x7ffeu);
    check("last-register-byte-lo", psx_spu_read8(spu, 0x1fe), 0xfeu);
    check("last-register-byte-hi", psx_spu_read8(spu, 0x1ff), 0x7fu);

    psx_spu_destroy(spu);

    /* ---- 6. THE FIFO PORT IS THE ONE READ WITH A SIDE EFFECT -------------------------
       psx_spu_read16(1F801DA8h) pops a halfword of sound RAM and advances the transfer
       address. A byte read must NOT, or two byte reads of one register would consume two
       different words and drag the transfer along with them. */
    spu = fresh();

    psx_spu_write16(spu, 0x1a6, 0x0100);

    const uint32_t fifo_taddr = spu->taddr;

    psx_spu_read8(spu, 0x1a8);
    psx_spu_read8(spu, 0x1a9);

    check("byte-read-of-fifo-does-not-advance", spu->taddr, fifo_taddr);

    psx_spu_read16(spu, 0x1a8);

    check("halfword-read-of-fifo-still-advances", spu->taddr, fifo_taddr + 2u);

    psx_spu_destroy(spu);

    if (g_failed) {
        fprintf(stderr, "SPU_WIDTH FAILED\n");

        return EXIT_FAILURE;
    }

    printf("SPU_WIDTH all cases passed\n");

    return EXIT_SUCCESS;
}
