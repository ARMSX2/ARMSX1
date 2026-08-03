/* ------------------------------------------------------------------------------------
   GTE against the documented rules, independently recomputed.

   WHY THIS EXISTS

   Every CPU case in tests/cpu_differential.c is DIFFERENTIAL: the interpreter against the
   cached interpreter, both this project's own code, both written from the same
   understanding. A wrong rule shared by both reads green forever -- the structural
   blindness that already bit GPU_PARITY (three rasterizers, one shared wrong formula),
   the blend rule, the shift matrix and GPUINFO. The GTE -- the unit that decides which
   polygons a game even submits, because NCLIP winding and screen coordinates come out of
   it -- had NO gate of either kind.

   So this file re-implements the psx-spx pseudocode for the geometry pipeline INSIDE the
   test (the UNR reciprocal table is rebuilt from its documented closed form, never copied
   from psx/cpu.c) and compares the real emulator against it:

     * unr-table-formula      table entries vs the documented generator formula
     * divide-exhaustive      gte_divide over the FULL 2^32 (H x SZ3) domain, threaded,
                              including the h >= 2*SZ3 overflow path and FLAG.17
     * rtps-divide-domain     RTPS through the instruction path: every SZ3 value times a
                              boundary set of H, both sf values; MAC0 = q via DQA=1/DQB=0
     * rtps-project-clamp     IR1/IR2/IR3 saturation (lm both ways), SX2/SY2 saturation
                              and MAC0 flags, OFX/OFY extremes, DQA/DQB depth queue and
                              IR0, exact clamp-boundary vectors for every bound
     * rtps-mac44             per-step 44-bit sign-expansion in the RTPS dot products,
                              exact +/-2^43 edge vectors
     * rtpt-fifo              RTPT: SXY/SZ FIFO end state, flag accumulation across the
                              three vertices, sf=0 IR3/SZ3 quirk
     * nclip-matrix           NCLIP over a dense 6-coordinate boundary lattice plus exact
                              +/-2^31 MAC0 overflow vectors and near-zero windings
     * avsz-matrix            AVSZ3/AVSZ4: full ZSF3/ZSF4 sweep, SZ boundary combos, OTZ
                              saturation edges, MAC0 overflow crossings
     * mvmva-matrix           MVMVA: all mx/v/cv/sf/lm combos including the documented
                              buggy CV=FC path (translation column checked-then-dropped)
     * dispatch-parity        the same vectors through all dispatch layers: direct
                              psx_cpu_execute, the decode-cache handler, and a fetched
                              psx_cpu_cycle in interpreter AND cached modes (the GTE
                              sf/lm/mx/v/cv latch decode is textually duplicated in three
                              places; the differential gate only ever encoded sf=0/lm=0)
     * pgxp-inert             a sweep with PGXP ENABLED: the hooks woven through the GTE
                              register paths must not perturb one architectural bit
     * regfile-*              COP2 register access semantics: SXYP FIFO push, sign/width
                              rules, FLAG write mask and the derived bit 31, IRGB
                              distribution, LZCS/LZCR, read-only registers, and the
                              MTC2/MFC2/CTC2/CFC2/LWC2/SWC2 instruction wrappers

   Reference convention: "spec" below means psx-spx pseudocode as cross-confirmed by
   hardware-verified emulators (per-step sign-expansion, the stage-3 sum feeding SZ3/IR3
   before final truncation, saturation-check-on-shifted-value rules). Where psx-spx is
   silent the reference states its source in a comment.

   MUTATION PROTOCOL (a gate that cannot fail is not a gate): temporarily perturb, one at
   a time, in psx/cpu.c: (1) one g_psx_gte_unr_table entry, (2) one clamp bound (e.g.
   0x3ff in gte_clamp_sxy), (3) one shift (e.g. s_mac3 >> 12 for SZ3), (4) one flag bit.
   `make test-gte` must go red for each. Revert byte-identically afterwards. psx/cpu.c is
   a prerequisite of the test binary, so the rebuild is automatic.

   The test #include's psx/cpu.c (unity-style) to reach the static internals directly:
   the exhaustive divide sweep is ~4.3e9 calls, affordable only in-TU. The Makefile
   filters psx/cpu.c out of the linked core sources for this binary.
   ------------------------------------------------------------------------------------ */

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psx/psx.h"
#include "psx/pgxp.h"

#include "psx/cpu.c"

/* cpu.c leaves its instruction-field helper macros defined; drop the collision-prone
   short names so the test namespace stays sane. */
#undef OP
#undef S
#undef T
#undef D
#undef IMM5
#undef CMT
#undef SOP
#undef IMM26
#undef IMM16
#undef IMM16S

#define GM_TEST_PC 0x80001000u
#define GM_TEST_OFFSET 0x1000u

/* ==================================================================================
   Independent reference model (psx-spx pseudocode re-implemented, no calls into the
   emulator's own helpers).
   ================================================================================== */

/* Well-defined arithmetic shift right (SAR) for int64: floor division by 2^n. */
static int64_t ref_sar64(int64_t v, unsigned n) {
    if (v >= 0)
        return (int64_t)((uint64_t)v >> n);

    return ~(int64_t)((uint64_t)(~v) >> n);
}

/* Well-defined wrap of an int64 to the low 32 bits, two's complement. */
static int32_t ref_wrap32(int64_t v) {
    uint32_t u = (uint32_t)((uint64_t)v & 0xffffffffull);

    if (u & 0x80000000u)
        return (int32_t)((int64_t)u - 0x100000000ll);

    return (int32_t)u;
}

/* 44-bit sign expansion: keep the low 44 bits, extend bit 43. */
static int64_t ref_sext44(int64_t v) {
    uint64_t m = (uint64_t)v & 0xfffffffffffull;

    if (m & 0x80000000000ull)
        return (int64_t)(m - 0x100000000000ull);

    return (int64_t)m;
}

static uint32_t ref_pack16(int16_t lo, int16_t hi) {
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}

static uint32_t ref_sext16(uint32_t raw16) {
    return (uint32_t)(int32_t)(int16_t)(uint16_t)raw16;
}

/* FLAG bit helpers. Bit numbers straight from the psx-spx FLAG table:
   30/29/28 MAC1..3 positive 44-bit overflow, 27/26/25 negative;
   24/23/22 IR1..3 saturated; 21/20/19 colour FIFO; 18 SZ3/OTZ; 17 divide overflow;
   16 MAC0 positive 32-bit overflow, 15 negative; 14 SX2, 13 SY2, 12 IR0. */
static void ref_flag_mac123(uint32_t* flag, int i, int64_t v) {
    if (v > 0x7ffffffffffll)
        *flag |= 0x40000000u >> (i - 1);
    else if (v < -0x80000000000ll)
        *flag |= 0x08000000u >> (i - 1);
}

static void ref_flag_mac0(uint32_t* flag, int64_t v) {
    if (v > 0x7fffffffll)
        *flag |= 1u << 16;
    else if (v < -0x80000000ll)
        *flag |= 1u << 15;
}

static int32_t ref_sat_ir123(uint32_t* flag, int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;

    if (v < lo) { *flag |= 0x01000000u >> (i - 1); return lo; }
    if (v > 0x7fff) { *flag |= 0x01000000u >> (i - 1); return 0x7fff; }

    return v;
}

/* FLAG as a read returns it: bits 0..11 always zero, bit 31 = OR of bits 30..23 and
   18..13 (mask 0x7f800000 | 0x0007e000 = 0x7f87e000). */
static uint32_t ref_flag_read(uint32_t stored) {
    uint32_t v = stored & 0x7ffff000u;

    if (v & 0x7f87e000u)
        v |= 0x80000000u;

    return v;
}

/* UNR reciprocal table rebuilt from the documented closed form -- deliberately NOT read
   from g_psx_gte_unr_table, so a wrong table entry there is a detectable divergence. */
static uint8_t g_ref_unr[257];

static void ref_unr_init(void) {
    for (int i = 0; i <= 0x100; i++) {
        int v = ((0x40000 / (i + 0x100)) + 1) / 2 - 0x101;

        g_ref_unr[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

/* The UNR divide, psx-spx form. Returns the 1.16 quotient (max 0x1ffff); sets FLAG.17
   on the h >= d*2 overflow path (bit 31 of the STORED word is modelled at read time). */
static uint32_t ref_divide(uint16_t h, uint16_t d, uint32_t* flag) {
    if ((uint32_t)h >= (uint32_t)d * 2u) {
        *flag |= 1u << 17;

        return 0x1ffffu;
    }

    unsigned z = (unsigned)__builtin_clz((uint32_t)d) - 16u; /* d != 0 here */
    uint32_t n = (uint32_t)h << z;
    uint32_t dd = (uint32_t)d << z; /* 0x8000..0xffff */
    uint32_t u = (uint32_t)g_ref_unr[(dd - 0x7fc0u) >> 7] + 0x101u;
    uint32_t d2 = (0x2000080u - dd * u) >> 8;
    uint32_t d3 = (0x80u + d2 * u) >> 8;
    uint64_t q = (((uint64_t)n * d3) + 0x8000u) >> 16;

    return (q > 0x1ffffu) ? 0x1ffffu : (uint32_t)q;
}

/* Full COP2 machine state, reference side. Fields mirror register semantics (widths and
   signedness), not the emulator's structs. */
typedef struct {
    /* data registers */
    int16_t vx[3], vy[3], vz[3];
    uint32_t rgbc;
    uint16_t otz;
    int16_t ir0, ir1, ir2, ir3;
    int16_t sx[3], sy[3];
    uint16_t sz[4];
    uint32_t rgb0, rgb1, rgb2;
    uint32_t res1;
    int32_t mac0, mac1, mac2, mac3;
    uint32_t irgb, lzcs, lzcr; /* poked raw; pass-through for the ops under test */

    /* control registers */
    int16_t rt[3][3];
    int32_t tr[3];
    int16_t l[3][3];
    int32_t bk[3];
    int16_t lr[3][3];
    int32_t fc[3];
    uint32_t ofx, ofy;
    uint32_t h_written; /* raw word as written; the divide uses the low 16 bits */
    int16_t dqa;
    int32_t dqb;
    int16_t zsf3, zsf4;
    uint32_t flag; /* stored form, bit 31 never stored */
} ref_gte_t;

/* One RTPS vertex, psx-spx pseudocode. sf is the SAR amount (0 or 12).

   Load-bearing spec details encoded here:
     - each of the three additions per row is 44-bit sign-expanded with flags BEFORE the
       next addition; the STAGE-3 sum is kept un-expanded and that value (not the MAC3
       register) feeds SZ3 and the IR3 saturation-flag quirk;
     - MAC registers store the sign-expanded value SAR sf, wrapped to 32 bits;
     - IR3: the FLAG.22 check uses (stage3 SAR 12) regardless of sf and of lm, while the
       stored IR3 value clamps the MAC3 register value to (lm ? 0 : -0x8000)..0x7fff;
     - SZ3 = (stage3 SAR 12) wrapped to 32 bits, saturated 0..0xffff with FLAG.18, pushed
       through the FIFO BEFORE the divide reads it;
     - SX2/SY2: MAC0 flags on the full OFX + IR1*q sum, saturation check on (sum SAR 16),
       clamp -0x400..0x3ff with FLAG.14/13;
     - depth queue: MAC0 = DQB + DQA*q with MAC0 flags, register wraps to 32 bits, IR0 =
       (sum SAR 12) saturated 0..0x1000 with FLAG.12. */
static void ref_rtp_vertex(ref_gte_t* g, int vi, int sf, int lm, int dq) {
    int64_t raw3 = 0;
    int32_t macreg[3];

    for (int r = 0; r < 3; r++) {
        int64_t acc = (int64_t)g->tr[r] * 4096;

        acc += (int64_t)g->rt[r][0] * g->vx[vi];
        ref_flag_mac123(&g->flag, r + 1, acc);
        acc = ref_sext44(acc);

        acc += (int64_t)g->rt[r][1] * g->vy[vi];
        ref_flag_mac123(&g->flag, r + 1, acc);
        acc = ref_sext44(acc);

        acc += (int64_t)g->rt[r][2] * g->vz[vi];
        ref_flag_mac123(&g->flag, r + 1, acc);

        if (r == 2)
            raw3 = acc; /* stage-3 sum, NOT sign-expanded */

        macreg[r] = ref_wrap32(ref_sar64(ref_sext44(acc), sf));
    }

    g->mac1 = macreg[0];
    g->mac2 = macreg[1];
    g->mac3 = macreg[2];

    g->ir1 = (int16_t)ref_sat_ir123(&g->flag, 1, macreg[0], lm);
    g->ir2 = (int16_t)ref_sat_ir123(&g->flag, 2, macreg[1], lm);

    {
        int32_t v12 = ref_wrap32(ref_sar64(raw3, 12));
        int32_t vsf = macreg[2];
        int32_t lo = lm ? 0 : -0x8000;

        if (v12 < -0x8000 || v12 > 0x7fff)
            g->flag |= 1u << 22;

        g->ir3 = (int16_t)(vsf < lo ? lo : (vsf > 0x7fff ? 0x7fff : vsf));
    }

    g->sz[0] = g->sz[1];
    g->sz[1] = g->sz[2];
    g->sz[2] = g->sz[3];

    {
        int32_t z = ref_wrap32(ref_sar64(raw3, 12));

        if (z < 0) { g->flag |= 1u << 18; z = 0; }
        else if (z > 0xffff) { g->flag |= 1u << 18; z = 0xffff; }

        g->sz[3] = (uint16_t)z;
    }

    {
        uint32_t q = ref_divide((uint16_t)g->h_written, g->sz[3], &g->flag);

        g->sx[0] = g->sx[1]; g->sy[0] = g->sy[1];
        g->sx[1] = g->sx[2]; g->sy[1] = g->sy[2];

        {
            int64_t xm = (int64_t)(int32_t)g->ofx + (int64_t)g->ir1 * (int64_t)(int32_t)q;
            int64_t xs;

            ref_flag_mac0(&g->flag, xm);
            xs = ref_sar64(xm, 16);

            if (xs < -0x400) { g->flag |= 1u << 14; xs = -0x400; }
            else if (xs > 0x3ff) { g->flag |= 1u << 14; xs = 0x3ff; }

            g->sx[2] = (int16_t)xs;
        }

        {
            int64_t ym = (int64_t)(int32_t)g->ofy + (int64_t)g->ir2 * (int64_t)(int32_t)q;
            int64_t ys;

            ref_flag_mac0(&g->flag, ym);
            ys = ref_sar64(ym, 16);

            if (ys < -0x400) { g->flag |= 1u << 13; ys = -0x400; }
            else if (ys > 0x3ff) { g->flag |= 1u << 13; ys = 0x3ff; }

            g->sy[2] = (int16_t)ys;
        }

        if (dq) {
            int64_t m0 = (int64_t)g->dqb + (int64_t)g->dqa * (int64_t)(int32_t)q;
            int32_t i0;

            ref_flag_mac0(&g->flag, m0);
            g->mac0 = ref_wrap32(m0);

            i0 = ref_wrap32(ref_sar64(m0, 12));

            if (i0 < 0) { g->flag |= 1u << 12; i0 = 0; }
            else if (i0 > 0x1000) { g->flag |= 1u << 12; i0 = 0x1000; }

            g->ir0 = (int16_t)i0;
        }
    }
}

static void ref_nclip(ref_gte_t* g) {
    int64_t v;

    g->flag = 0;

    v = (int64_t)g->sx[0] * g->sy[1] + (int64_t)g->sx[1] * g->sy[2] +
        (int64_t)g->sx[2] * g->sy[0] - (int64_t)g->sx[0] * g->sy[2] -
        (int64_t)g->sx[1] * g->sy[0] - (int64_t)g->sx[2] * g->sy[1];

    ref_flag_mac0(&g->flag, v);
    g->mac0 = ref_wrap32(v);
}

static void ref_avsz(ref_gte_t* g, int four) {
    int64_t sum, v;
    int32_t otz;

    g->flag = 0;

    sum = four ? ((int64_t)g->sz[0] + g->sz[1] + g->sz[2] + g->sz[3])
               : ((int64_t)g->sz[1] + g->sz[2] + g->sz[3]);
    v = (int64_t)(four ? g->zsf4 : g->zsf3) * sum;

    ref_flag_mac0(&g->flag, v);
    g->mac0 = ref_wrap32(v);

    otz = ref_wrap32(ref_sar64(v, 12));

    if (otz < 0) { g->flag |= 1u << 18; otz = 0; }
    else if (otz > 0xffff) { g->flag |= 1u << 18; otz = 0xffff; }

    g->otz = (uint16_t)otz;
}

/* MVMVA. Normal path: MACi = sext44 chain over (CVi<<12 + Mi1*Vx) + Mi2*Vy + Mi3*Vz,
   SAR sf, IR with lm. Buggy CV=FC path (psx-spx): the CVi<<12 + Mi1*Vx part is computed,
   MAC-flag-checked, SAR sf'd and IR-saturation-CHECKED (lm=0, flags only), then DROPPED;
   the retained result is only (Mi2*Vy + Mi3*Vz) through the usual pipeline. */
static void ref_mvmva(ref_gte_t* g, int mx, int v, int cv, int sf, int lm) {
    int16_t M[3][3];
    int16_t Vx, Vy, Vz;
    int32_t C[3];

    g->flag = 0;

    switch (mx) {
        case 0: memcpy(M, g->rt, sizeof M); break;
        case 1: memcpy(M, g->l, sizeof M); break;
        case 2: memcpy(M, g->lr, sizeof M); break;
        default: {
            int32_t rc = (int32_t)(g->rgbc & 0xff);

            M[0][0] = (int16_t)(-(rc << 4));
            M[0][1] = (int16_t)(rc << 4);
            M[0][2] = g->ir0;
            M[1][0] = M[1][1] = M[1][2] = g->rt[0][2]; /* RT13 */
            M[2][0] = M[2][1] = M[2][2] = g->rt[1][1]; /* RT22 */
        } break;
    }

    switch (v) {
        case 0: case 1: case 2:
            Vx = g->vx[v]; Vy = g->vy[v]; Vz = g->vz[v];
            break;

        default:
            Vx = g->ir1; Vy = g->ir2; Vz = g->ir3;
            break;
    }

    switch (cv) {
        case 0: C[0] = g->tr[0]; C[1] = g->tr[1]; C[2] = g->tr[2]; break;
        case 1: C[0] = g->bk[0]; C[1] = g->bk[1]; C[2] = g->bk[2]; break;
        case 2: C[0] = g->fc[0]; C[1] = g->fc[1]; C[2] = g->fc[2]; break;
        default: C[0] = C[1] = C[2] = 0; break;
    }

    if (cv == 2) {
        int32_t kept[3];

        for (int r = 0; r < 3; r++) {
            int64_t acc = (int64_t)M[r][1] * Vy;

            ref_flag_mac123(&g->flag, r + 1, acc);
            acc = ref_sext44(acc);
            acc += (int64_t)M[r][2] * Vz;
            ref_flag_mac123(&g->flag, r + 1, acc);
            kept[r] = ref_wrap32(ref_sar64(ref_sext44(acc), sf));
        }

        for (int r = 0; r < 3; r++) {
            int64_t dropped = (int64_t)C[r] * 4096 + (int64_t)M[r][0] * Vx;
            int32_t dv;

            ref_flag_mac123(&g->flag, r + 1, dropped);
            dv = ref_wrap32(ref_sar64(ref_sext44(dropped), sf));
            (void)ref_sat_ir123(&g->flag, r + 1, dv, 0); /* flag only, value dropped */
        }

        g->mac1 = kept[0];
        g->mac2 = kept[1];
        g->mac3 = kept[2];
    } else {
        int32_t macreg[3];

        for (int r = 0; r < 3; r++) {
            int64_t acc = (int64_t)C[r] * 4096;

            acc += (int64_t)M[r][0] * Vx;
            ref_flag_mac123(&g->flag, r + 1, acc);
            acc = ref_sext44(acc);

            acc += (int64_t)M[r][1] * Vy;
            ref_flag_mac123(&g->flag, r + 1, acc);
            acc = ref_sext44(acc);

            acc += (int64_t)M[r][2] * Vz;
            ref_flag_mac123(&g->flag, r + 1, acc);

            macreg[r] = ref_wrap32(ref_sar64(ref_sext44(acc), sf));
        }

        g->mac1 = macreg[0];
        g->mac2 = macreg[1];
        g->mac3 = macreg[2];
    }

    g->ir1 = (int16_t)ref_sat_ir123(&g->flag, 1, g->mac1, lm);
    g->ir2 = (int16_t)ref_sat_ir123(&g->flag, 2, g->mac2, lm);
    g->ir3 = (int16_t)ref_sat_ir123(&g->flag, 3, g->mac3, lm);
}

/* Reference decoder: the instruction fields as psx-spx documents them.
   sf = bit 19 (SAR 12 when set), lm = bit 10, mx = 18:17, v = 16:15, cv = 14:13. */
static int ref_execute(ref_gte_t* g, uint32_t opcode) {
    int sf = ((opcode >> 19) & 1) ? 12 : 0;
    int lm = (opcode >> 10) & 1;
    int mx = (opcode >> 17) & 3;
    int v = (opcode >> 15) & 3;
    int cv = (opcode >> 13) & 3;

    switch (opcode & 0x3f) {
        case 0x01: /* RTPS */
            g->flag = 0;
            ref_rtp_vertex(g, 0, sf, lm, 1);
            return 1;

        case 0x30: /* RTPT */
            g->flag = 0;
            ref_rtp_vertex(g, 0, sf, lm, 0);
            ref_rtp_vertex(g, 1, sf, lm, 0);
            ref_rtp_vertex(g, 2, sf, lm, 1);
            return 1;

        case 0x06: /* NCLIP */
            ref_nclip(g);
            return 1;

        case 0x2d: /* AVSZ3 */
            ref_avsz(g, 0);
            return 1;

        case 0x2e: /* AVSZ4 */
            ref_avsz(g, 1);
            return 1;

        case 0x12: /* MVMVA */
            ref_mvmva(g, mx, v, cv, sf, lm);
            return 1;
    }

    return 0;
}

/* ==================================================================================
   Harness: apply a reference state to the real CPU through the real register write
   path, execute, read everything back through the real register read path, compare
   against the reference-predicted read values.
   ================================================================================== */

/* Expected value of a COP2 register READ given the reference state. Registers 28/29
   (IRGB/ORGB) are excluded from op-level comparison and get their own regfile case. */
static uint32_t ref_expected_read(const ref_gte_t* g, int r) {
    switch (r) {
        case 0: return ref_pack16(g->vx[0], g->vy[0]);
        case 1: return (uint32_t)(int32_t)g->vz[0];
        case 2: return ref_pack16(g->vx[1], g->vy[1]);
        case 3: return (uint32_t)(int32_t)g->vz[1];
        case 4: return ref_pack16(g->vx[2], g->vy[2]);
        case 5: return (uint32_t)(int32_t)g->vz[2];
        case 6: return g->rgbc;
        case 7: return g->otz;
        case 8: return (uint32_t)(int32_t)g->ir0;
        case 9: return (uint32_t)(int32_t)g->ir1;
        case 10: return (uint32_t)(int32_t)g->ir2;
        case 11: return (uint32_t)(int32_t)g->ir3;
        case 12: return ref_pack16(g->sx[0], g->sy[0]);
        case 13: return ref_pack16(g->sx[1], g->sy[1]);
        case 14: return ref_pack16(g->sx[2], g->sy[2]);
        case 15: return ref_pack16(g->sx[2], g->sy[2]); /* SXYP reads as SXY2 mirror */
        case 16: return g->sz[0];
        case 17: return g->sz[1];
        case 18: return g->sz[2];
        case 19: return g->sz[3];
        case 20: return g->rgb0;
        case 21: return g->rgb1;
        case 22: return g->rgb2;
        case 23: return g->res1;
        case 24: return (uint32_t)g->mac0;
        case 25: return (uint32_t)g->mac1;
        case 26: return (uint32_t)g->mac2;
        case 27: return (uint32_t)g->mac3;
        case 30: return g->lzcs;
        case 31: return g->lzcr;
        case 32: return ref_pack16(g->rt[0][0], g->rt[0][1]);
        case 33: return ref_pack16(g->rt[0][2], g->rt[1][0]);
        case 34: return ref_pack16(g->rt[1][1], g->rt[1][2]);
        case 35: return ref_pack16(g->rt[2][0], g->rt[2][1]);
        case 36: return (uint32_t)(int32_t)g->rt[2][2];
        case 37: return (uint32_t)g->tr[0];
        case 38: return (uint32_t)g->tr[1];
        case 39: return (uint32_t)g->tr[2];
        case 40: return ref_pack16(g->l[0][0], g->l[0][1]);
        case 41: return ref_pack16(g->l[0][2], g->l[1][0]);
        case 42: return ref_pack16(g->l[1][1], g->l[1][2]);
        case 43: return ref_pack16(g->l[2][0], g->l[2][1]);
        case 44: return (uint32_t)(int32_t)g->l[2][2];
        case 45: return (uint32_t)g->bk[0];
        case 46: return (uint32_t)g->bk[1];
        case 47: return (uint32_t)g->bk[2];
        case 48: return ref_pack16(g->lr[0][0], g->lr[0][1]);
        case 49: return ref_pack16(g->lr[0][2], g->lr[1][0]);
        case 50: return ref_pack16(g->lr[1][1], g->lr[1][2]);
        case 51: return ref_pack16(g->lr[2][0], g->lr[2][1]);
        case 52: return (uint32_t)(int32_t)g->lr[2][2];
        case 53: return (uint32_t)g->fc[0];
        case 54: return (uint32_t)g->fc[1];
        case 55: return (uint32_t)g->fc[2];
        case 56: return g->ofx;
        case 57: return g->ofy;
        case 58: return ref_sext16(g->h_written); /* H reads back sign-expanded */
        case 59: return (uint32_t)(int32_t)g->dqa;
        case 60: return (uint32_t)g->dqb;
        case 61: return (uint32_t)(int32_t)g->zsf3;
        case 62: return (uint32_t)(int32_t)g->zsf4;
        case 63: return ref_flag_read(g->flag);
    }

    return 0;
}

/* Canary defaults: distinctive values in every register an op under test must NOT
   touch, so an unexpected write anywhere in the file is a visible mismatch. */
static void ref_init(ref_gte_t* g) {
    memset(g, 0, sizeof *g);

    g->rgbc = 0xb3b2b1b0u;
    g->rgb0 = 0x40414243u;
    g->rgb1 = 0x50515253u;
    g->rgb2 = 0x60616263u;
    g->res1 = 0xcafe0001u;
    g->otz = 0xabcdu;
    g->ir0 = 0x123;
    g->ir1 = -0x456;
    g->ir2 = 0x789;
    g->ir3 = -0xabc;
    g->mac0 = (int32_t)0xd0d1d2d3u;
    g->mac1 = (int32_t)0xd4d5d6d7u;
    g->mac2 = (int32_t)0xd8d9dadbu;
    g->mac3 = (int32_t)0xdcdddedfu;
    g->sx[0] = 0x101; g->sy[0] = -0x102;
    g->sx[1] = 0x203; g->sy[1] = -0x204;
    g->sx[2] = 0x305; g->sy[2] = -0x306;
    g->sz[0] = 0x1111;
    g->sz[1] = 0x2222;
    g->sz[2] = 0x3333;
    g->sz[3] = 0x4444;
    g->irgb = 0x2f0f;
    g->lzcs = 0x00ff00ffu;
    g->lzcr = 8;
    g->flag = 0x7ffff000u; /* proves every op resets FLAG */
}

/* Write the reference state into the real CPU through the real write path. IRGB and
   LZCS/LZCR are poked raw because their write handlers have side effects (IR
   distribution, LZCR recompute) that would disturb the state being staged; those side
   effects get their own regfile cases. */
static void gm_apply(psx_cpu_t* cpu, const ref_gte_t* g) {
    memset(&cpu->cop2_dr, 0, sizeof cpu->cop2_dr);
    memset(&cpu->cop2_cr, 0, sizeof cpu->cop2_cr);

    cpu->load_d = 0;
    cpu->load_v = 0;
    cpu->s_mac0 = 0;
    cpu->s_mac3 = 0;

    gte_write_register(cpu, 0, ref_pack16(g->vx[0], g->vy[0]));
    gte_write_register(cpu, 1, (uint32_t)(int32_t)g->vz[0]);
    gte_write_register(cpu, 2, ref_pack16(g->vx[1], g->vy[1]));
    gte_write_register(cpu, 3, (uint32_t)(int32_t)g->vz[1]);
    gte_write_register(cpu, 4, ref_pack16(g->vx[2], g->vy[2]));
    gte_write_register(cpu, 5, (uint32_t)(int32_t)g->vz[2]);
    gte_write_register(cpu, 6, g->rgbc);
    gte_write_register(cpu, 7, g->otz);
    gte_write_register(cpu, 8, (uint32_t)(int32_t)g->ir0);
    gte_write_register(cpu, 9, (uint32_t)(int32_t)g->ir1);
    gte_write_register(cpu, 10, (uint32_t)(int32_t)g->ir2);
    gte_write_register(cpu, 11, (uint32_t)(int32_t)g->ir3);
    gte_write_register(cpu, 12, ref_pack16(g->sx[0], g->sy[0]));
    gte_write_register(cpu, 13, ref_pack16(g->sx[1], g->sy[1]));
    gte_write_register(cpu, 14, ref_pack16(g->sx[2], g->sy[2]));
    gte_write_register(cpu, 16, g->sz[0]);
    gte_write_register(cpu, 17, g->sz[1]);
    gte_write_register(cpu, 18, g->sz[2]);
    gte_write_register(cpu, 19, g->sz[3]);
    gte_write_register(cpu, 20, g->rgb0);
    gte_write_register(cpu, 21, g->rgb1);
    gte_write_register(cpu, 22, g->rgb2);
    gte_write_register(cpu, 23, g->res1);
    gte_write_register(cpu, 24, (uint32_t)g->mac0);
    gte_write_register(cpu, 25, (uint32_t)g->mac1);
    gte_write_register(cpu, 26, (uint32_t)g->mac2);
    gte_write_register(cpu, 27, (uint32_t)g->mac3);

    gte_write_register(cpu, 32, ref_pack16(g->rt[0][0], g->rt[0][1]));
    gte_write_register(cpu, 33, ref_pack16(g->rt[0][2], g->rt[1][0]));
    gte_write_register(cpu, 34, ref_pack16(g->rt[1][1], g->rt[1][2]));
    gte_write_register(cpu, 35, ref_pack16(g->rt[2][0], g->rt[2][1]));
    gte_write_register(cpu, 36, (uint32_t)(int32_t)g->rt[2][2]);
    gte_write_register(cpu, 37, (uint32_t)g->tr[0]);
    gte_write_register(cpu, 38, (uint32_t)g->tr[1]);
    gte_write_register(cpu, 39, (uint32_t)g->tr[2]);
    gte_write_register(cpu, 40, ref_pack16(g->l[0][0], g->l[0][1]));
    gte_write_register(cpu, 41, ref_pack16(g->l[0][2], g->l[1][0]));
    gte_write_register(cpu, 42, ref_pack16(g->l[1][1], g->l[1][2]));
    gte_write_register(cpu, 43, ref_pack16(g->l[2][0], g->l[2][1]));
    gte_write_register(cpu, 44, (uint32_t)(int32_t)g->l[2][2]);
    gte_write_register(cpu, 45, (uint32_t)g->bk[0]);
    gte_write_register(cpu, 46, (uint32_t)g->bk[1]);
    gte_write_register(cpu, 47, (uint32_t)g->bk[2]);
    gte_write_register(cpu, 48, ref_pack16(g->lr[0][0], g->lr[0][1]));
    gte_write_register(cpu, 49, ref_pack16(g->lr[0][2], g->lr[1][0]));
    gte_write_register(cpu, 50, ref_pack16(g->lr[1][1], g->lr[1][2]));
    gte_write_register(cpu, 51, ref_pack16(g->lr[2][0], g->lr[2][1]));
    gte_write_register(cpu, 52, (uint32_t)(int32_t)g->lr[2][2]);
    gte_write_register(cpu, 53, (uint32_t)g->fc[0]);
    gte_write_register(cpu, 54, (uint32_t)g->fc[1]);
    gte_write_register(cpu, 55, (uint32_t)g->fc[2]);
    gte_write_register(cpu, 56, g->ofx);
    gte_write_register(cpu, 57, g->ofy);
    gte_write_register(cpu, 58, g->h_written);
    gte_write_register(cpu, 59, (uint32_t)(int32_t)g->dqa);
    gte_write_register(cpu, 60, (uint32_t)g->dqb);
    gte_write_register(cpu, 61, (uint32_t)(int32_t)g->zsf3);
    gte_write_register(cpu, 62, (uint32_t)(int32_t)g->zsf4);
    gte_write_register(cpu, 63, g->flag);

    cpu->cop2_dr.irgb = (uint16_t)g->irgb;
    cpu->cop2_dr.lzcs = (int32_t)g->lzcs;
    cpu->cop2_dr.lzcr = (int32_t)g->lzcr;
}

typedef struct {
    unsigned long checked;
    unsigned long failed;
    unsigned long printed;
} gm_stats_t;

#define GM_PRINT_CAP 12

static void gm_print_state(const ref_gte_t* g, uint32_t opcode) {
    fprintf(stderr,
            "  opcode=%08x\n"
            "  v0=(%d,%d,%d) v1=(%d,%d,%d) v2=(%d,%d,%d)\n"
            "  rt=[%d %d %d | %d %d %d | %d %d %d] tr=(%d,%d,%d)\n"
            "  ofx=%08x ofy=%08x h=%08x dqa=%d dqb=%d zsf3=%d zsf4=%d\n"
            "  in.sxy0=(%d,%d) sxy1=(%d,%d) sxy2=(%d,%d) in.sz=[%u %u %u %u]\n"
            "  in.ir=(%d,%d,%d,%d)\n",
            opcode,
            g->vx[0], g->vy[0], g->vz[0], g->vx[1], g->vy[1], g->vz[1],
            g->vx[2], g->vy[2], g->vz[2],
            g->rt[0][0], g->rt[0][1], g->rt[0][2], g->rt[1][0], g->rt[1][1],
            g->rt[1][2], g->rt[2][0], g->rt[2][1], g->rt[2][2],
            g->tr[0], g->tr[1], g->tr[2],
            g->ofx, g->ofy, g->h_written, g->dqa, g->dqb, g->zsf3, g->zsf4,
            g->sx[0], g->sy[0], g->sx[1], g->sy[1], g->sx[2], g->sy[2],
            g->sz[0], g->sz[1], g->sz[2], g->sz[3],
            g->ir0, g->ir1, g->ir2, g->ir3);
}

/* Run one vector through psx_cpu_execute (the interpreter dispatch, opcode already
   latched) and compare the complete COP2 register file against the reference. */
static int gm_run_vec(psx_cpu_t* cpu, const ref_gte_t* pre, uint32_t opcode,
                      const char* casename, gm_stats_t* st) {
    ref_gte_t post = *pre;
    int bad = 0;

    if (!ref_execute(&post, opcode)) {
        fprintf(stderr, "GTE_MATRIX failed case=%s reason=unhandled-funct opcode=%08x\n",
                casename, opcode);
        st->failed++;
        return 0;
    }

    gm_apply(cpu, pre);
    cpu->opcode = opcode;

    if (psx_cpu_execute(cpu) == 0) {
        fprintf(stderr, "GTE_MATRIX failed case=%s reason=dispatch-refused opcode=%08x\n",
                casename, opcode);
        st->failed++;
        return 0;
    }

    for (int r = 0; r < 64; r++) {
        uint32_t got, want;

        if (r == 28 || r == 29) /* IRGB/ORGB read conversion: own regfile case */
            continue;

        got = gte_read_register(cpu, (uint32_t)r);
        want = ref_expected_read(&post, r);

        if (got != want) {
            if (st->printed < GM_PRINT_CAP) {
                if (!bad) {
                    fprintf(stderr, "GTE_MATRIX failed case=%s reason=state-mismatch\n",
                            casename);
                    gm_print_state(pre, opcode);
                }

                fprintf(stderr, "  reg%-2d got=%08x want=%08x\n", r, got, want);
            }

            bad = 1;
        }
    }

    st->checked++;

    if (bad) {
        st->failed++;
        if (st->printed < GM_PRINT_CAP)
            st->printed++;
    }

    return !bad;
}

/* Deterministic PRNG for don't-care-bit fuzz and co-indexed lattice mixing. */
static uint32_t gm_rnd_state = 0x12345678u;

static uint32_t gm_rnd(void) {
    uint32_t x = gm_rnd_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    return gm_rnd_state = x;
}

/* GTE opcode builders. Bits 24:20, 12:11 and 9:6 are don't-care for every op here;
   18:13 (mx/v/cv) additionally don't-care for RTPS/RTPT, and 19 (sf) / 10 (lm) for
   NCLIP/AVSZ. Fuzzing them proves no accidental coupling. */
static uint32_t gm_enc(uint32_t funct, int sf, int lm, uint32_t dc_mask) {
    uint32_t op = 0x4a000000u | funct;

    if (sf)
        op |= 1u << 19;
    if (lm)
        op |= 1u << 10;

    op |= gm_rnd() & dc_mask;

    return op;
}

#define GM_DC_RTP 0x01f7fbc0u   /* 24:20, 18:13, 12:11, 9:6 */
#define GM_DC_ALL 0x01fffbc0u   /* the above plus sf(19) and lm(10) */
#define GM_DC_MVM 0x01f01bc0u   /* 24:20, 12:11, 9:6 (mx/v/cv/sf/lm are live) */

/* ==================================================================================
   Case: unr-table-formula
   ================================================================================== */
static int case_unr_table(void) {
    printf("GTE_MATRIX begin case=unr-table-formula\n");

    if (sizeof g_psx_gte_unr_table != 257) {
        fprintf(stderr, "GTE_MATRIX failed case=unr-table-formula reason=size got=%zu\n",
                sizeof g_psx_gte_unr_table);
        return 0;
    }

    for (int i = 0; i <= 0x100; i++) {
        if (g_psx_gte_unr_table[i] != g_ref_unr[i]) {
            fprintf(stderr,
                    "GTE_MATRIX failed case=unr-table-formula index=%d got=%02x want=%02x\n",
                    i, g_psx_gte_unr_table[i], g_ref_unr[i]);
            return 0;
        }
    }

    printf("GTE_MATRIX passed case=unr-table-formula checked=257\n");
    return 1;
}

/* ==================================================================================
   Case: divide-exhaustive -- the full 2^32 (H, SZ3) domain, threaded.
   ================================================================================== */
typedef struct {
    uint32_t d_lo, d_hi;
    unsigned long checked;
    unsigned long failed;
    uint32_t first_h, first_d, first_got, first_want, first_gotflag, first_wantflag;
    psx_cpu_t* cpu;
} gm_div_job_t;

static void* gm_div_worker(void* p) {
    gm_div_job_t* job = (gm_div_job_t*)p;
    psx_cpu_t* cpu = job->cpu;

    for (uint32_t d = job->d_lo; d < job->d_hi; d++) {
        /* Hoist the reference's divisor-derived values (same math, factored). */
        uint32_t z = 0, d3 = 0;

        if (d) {
            uint32_t dd, u, d2;

            z = (uint32_t)__builtin_clz(d) - 16u;
            dd = d << z;
            u = (uint32_t)g_ref_unr[(dd - 0x7fc0u) >> 7] + 0x101u;
            d2 = (0x2000080u - dd * u) >> 8;
            d3 = (0x80u + d2 * u) >> 8;
        }

        for (uint32_t h = 0; h <= 0xffffu; h++) {
            uint32_t got, gotflag, want, wantflag;

            cpu->cop2_cr.flag = 0;
            got = gte_divide(cpu, (uint16_t)h, (uint16_t)d);
            gotflag = cpu->cop2_cr.flag;

            if (h >= d * 2u) {
                want = 0x1ffffu;
                wantflag = (1u << 31) | (1u << 17);
            } else {
                uint64_t q = (((uint64_t)(h << z) * d3) + 0x8000u) >> 16;

                want = (q > 0x1ffffu) ? 0x1ffffu : (uint32_t)q;
                wantflag = 0;
            }

            job->checked++;

            if (got != want || gotflag != wantflag) {
                if (!job->failed) {
                    job->first_h = h;
                    job->first_d = d;
                    job->first_got = got;
                    job->first_want = want;
                    job->first_gotflag = gotflag;
                    job->first_wantflag = wantflag;
                }

                job->failed++;
            }
        }
    }

    return NULL;
}

static int case_divide_exhaustive(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (ncpu < 1) ? 1 : (ncpu > 10 ? 10 : (int)ncpu);
    pthread_t tids[10];
    gm_div_job_t jobs[10];
    unsigned long checked = 0, failed = 0;
    int ok = 1;

    printf("GTE_MATRIX begin case=divide-exhaustive threads=%d domain=2^32\n", nthreads);
    fflush(stdout);

    for (int t = 0; t < nthreads; t++) {
        memset(&jobs[t], 0, sizeof jobs[t]);
        jobs[t].d_lo = (uint32_t)(((uint64_t)0x10000u * t) / nthreads);
        jobs[t].d_hi = (uint32_t)(((uint64_t)0x10000u * (t + 1)) / nthreads);
        jobs[t].cpu = (psx_cpu_t*)calloc(1, sizeof(psx_cpu_t));

        if (!jobs[t].cpu || pthread_create(&tids[t], NULL, gm_div_worker, &jobs[t])) {
            fprintf(stderr, "GTE_MATRIX failed case=divide-exhaustive reason=thread-setup\n");
            return 0;
        }
    }

    for (int t = 0; t < nthreads; t++) {
        pthread_join(tids[t], NULL);
        checked += jobs[t].checked;
        failed += jobs[t].failed;

        if (jobs[t].failed && ok) {
            fprintf(stderr,
                    "GTE_MATRIX failed case=divide-exhaustive h=%04x sz3=%04x "
                    "got=%05x want=%05x gotflag=%08x wantflag=%08x (thread first fail; "
                    "%lu total in thread)\n",
                    jobs[t].first_h, jobs[t].first_d, jobs[t].first_got,
                    jobs[t].first_want, jobs[t].first_gotflag, jobs[t].first_wantflag,
                    jobs[t].failed);
            ok = 0;
        }

        free(jobs[t].cpu);
    }

    if (ok)
        printf("GTE_MATRIX passed case=divide-exhaustive checked=%lu\n", checked);
    else
        fprintf(stderr, "GTE_MATRIX failed case=divide-exhaustive failures=%lu\n", failed);

    return ok;
}

/* ==================================================================================
   Case: rtps-divide-domain -- RTPS via the instruction path for every SZ3 with a
   boundary set of H values. With RT=0/V=0, MAC3=TRZ (sf=1) or TRZ<<12 (sf=0), so TRZ
   lands the exact SZ3 wanted; DQA=1/DQB=0 makes MAC0 read back the raw quotient.
   ================================================================================== */
static int case_rtps_divide_domain(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};

    printf("GTE_MATRIX begin case=rtps-divide-domain\n");
    fflush(stdout);

    for (uint32_t trz = 0; trz <= 0xffffu; trz++) {
        uint32_t hset[16];
        int nh = 0;

        hset[nh++] = 0;
        hset[nh++] = 1;
        hset[nh++] = 2;
        hset[nh++] = 3;
        if (trz >= 1) hset[nh++] = trz - 1;
        hset[nh++] = trz;
        hset[nh++] = trz + 1 > 0xffffu ? 0xffffu : trz + 1;
        if (2 * trz >= 2) hset[nh++] = (2 * trz - 2 > 0xffffu) ? 0xffffu : 2 * trz - 2;
        if (2 * trz >= 1) hset[nh++] = (2 * trz - 1 > 0xffffu) ? 0xffffu : 2 * trz - 1;
        hset[nh++] = (2 * trz > 0xffffu) ? 0xffffu : 2 * trz;
        hset[nh++] = (2 * trz + 1 > 0xffffu) ? 0xffffu : 2 * trz + 1;
        hset[nh++] = 0x7fff;
        hset[nh++] = 0x8000;
        hset[nh++] = 0xffff;
        /* H write with garbage upper bits: only the low 16 may matter. */
        hset[nh++] = 0xdead0000u | (trz & 0xffffu);

        for (int hi = 0; hi < nh; hi++) {
            for (int sf = 0; sf < 2; sf++) {
                ref_gte_t g;

                ref_init(&g);
                g.tr[2] = (int32_t)trz;
                g.h_written = hset[hi];
                g.dqa = 1;
                g.dqb = 0;

                if (!gm_run_vec(cpu, &g, gm_enc(0x01, sf, 0, GM_DC_RTP),
                                "rtps-divide-domain", &st))
                    if (st.failed > 32)
                        goto out;
            }
        }
    }

    /* SZ3 clamp behaviour: negative and oversized MAC3. */
    {
        static const int32_t trz_special[] = {
            -1, -2, -0x8000, INT32_MIN, 0x10000, 0x10001, 0xfffff,
            0x7fffffff, 0x12345, 0x100000
        };

        for (size_t i = 0; i < sizeof trz_special / sizeof trz_special[0]; i++) {
            static const uint32_t hs[] = {0, 1, 0x100, 0x7fff, 0x8000, 0xffff};

            for (size_t hi = 0; hi < sizeof hs / sizeof hs[0]; hi++) {
                for (int sf = 0; sf < 2; sf++) {
                    ref_gte_t g;

                    ref_init(&g);
                    g.tr[2] = trz_special[i];
                    g.h_written = hs[hi];
                    g.dqa = 1;
                    g.dqb = 0;

                    gm_run_vec(cpu, &g, gm_enc(0x01, sf, 0, GM_DC_RTP),
                               "rtps-divide-domain", &st);
                }
            }
        }
    }

out:
    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=rtps-divide-domain checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=rtps-divide-domain checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: rtps-project-clamp -- IR/SXY/IR0 saturation and MAC0 flags, exact boundary
   vectors for every bound, then a broad co-indexed mix.
   ================================================================================== */
static int case_rtps_project_clamp(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};

    printf("GTE_MATRIX begin case=rtps-project-clamp\n");
    fflush(stdout);

    /* Named exact vectors. (h=0x200, trz=0x200) makes the UNR quotient exactly
       0x10000, so DQA/DQB and OFX/IR products land exact MAC0/IR0/SXY boundaries. */
    {
        typedef struct {
            uint32_t h;
            int32_t trz, trx, try_;
            uint32_t ofx, ofy;
            int16_t dqa;
            int32_t dqb;
        } named_t;

        static const named_t named[] = {
            /* SX2/SY2 exact clamp edges via OFX/OFY alone (IR1=IR2=0). */
            {0x200, 0x200, 0, 0, 0x03ff0000u, 0x03ff0000u, 0, 0},
            {0x200, 0x200, 0, 0, 0x04000000u, 0x04000000u, 0, 0},
            {0x200, 0x200, 0, 0, 0xfc000000u, 0xfc000000u, 0, 0},
            {0x200, 0x200, 0, 0, 0xfbff0000u, 0xfbff0000u, 0, 0},
            {0x200, 0x200, 0, 0, 0x03ffffffu, 0xfc00ffffu, 0, 0},
            /* MAC0 exact 2^31 edges on the depth queue: q=0x10000. */
            {0x200, 0x200, 0, 0, 0, 0, 0x7fff, 0x00010000},  /* +2^31 exactly */
            {0x200, 0x200, 0, 0, 0, 0, 0x7fff, 0x0000ffff},  /* +2^31-1 exactly */
            {0x200, 0x200, 0, 0, 0, 0, -0x8000, 0},          /* -2^31 exactly */
            {0x200, 0x200, 0, 0, 0, 0, -0x8000, -1},         /* -2^31-1 exactly */
            /* IR0 exact edges. */
            {0x200, 0x200, 0, 0, 0, 0, 0, 0x01000000},       /* IR0=0x1000, no flag */
            {0x200, 0x200, 0, 0, 0, 0, 0, 0x01000fff},       /* still 0x1000, no flag */
            {0x200, 0x200, 0, 0, 0, 0, 0, 0x01001000},       /* 0x1001 -> sat+flag */
            {0x200, 0x200, 0, 0, 0, 0, 0, -1},               /* negative -> 0+flag */
            /* IR1/IR2 exact clamp edges via TRX/TRY (sf=1 makes MAC=TR). */
            {0x200, 0x200, 0x7fff, 0x7fff, 0, 0, 1, 0},
            {0x200, 0x200, 0x8000, 0x8000, 0, 0, 1, 0},
            {0x200, 0x200, -0x8000, -0x8000, 0, 0, 1, 0},
            {0x200, 0x200, -0x8001, -0x8001, 0, 0, 1, 0},
            {0x200, 0x200, -1, -2, 0, 0, 1, 0},
            /* SX2 driven through IR1*q: IR1=0x7fff, q=0x10000 -> product 0x7fff0000. */
            {0x200, 0x200, 0x7fff, -0x8000, 0, 0, 0, 0},
            {0x200, 0x200, 0x7fff, -0x8000, 0x7fffffffu, 0x80000000u, 0, 0},
            {0x200, 0x200, 0x7fff, -0x8000, 0x80000000u, 0x7fffffffu, 0, 0},
        };

        for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
            for (int sf = 0; sf < 2; sf++) {
                for (int lm = 0; lm < 2; lm++) {
                    ref_gte_t g;

                    ref_init(&g);
                    g.h_written = named[i].h;
                    g.tr[0] = named[i].trx;
                    g.tr[1] = named[i].try_;
                    g.tr[2] = named[i].trz;
                    g.ofx = named[i].ofx;
                    g.ofy = named[i].ofy;
                    g.dqa = named[i].dqa;
                    g.dqb = named[i].dqb;

                    gm_run_vec(cpu, &g, gm_enc(0x01, sf, lm, GM_DC_RTP),
                               "rtps-project-clamp", &st);
                }
            }
        }
    }

    /* Broad co-indexed mix. */
    {
        static const int32_t trlat[] = {
            0, 1, -1, 2, -2, 0x7ffe, 0x7fff, 0x8000, 0x8001, -0x7fff, -0x8000,
            -0x8001, 0xfffe, 0xffff, 0x10000, 0x12345, -0x12345, 0x7ffffffe,
            0x7fffffff, -0x7fffffff, INT32_MIN, 0x40000000, -0x40000000,
            0x123456, -0x654321
        };
        static const uint32_t olat[] = {
            0, 0x03ff0000u, 0x04000000u, 0xfc000000u, 0xfbff0000u, 0x7fffffffu,
            0x80000000u, 0x12348765u, 0xffff0000u, 0x00010000u
        };
        static const uint32_t hlat[] = {0, 1, 0x100, 0x1ff, 0x200, 0x5678, 0x7fff,
                                        0x8000, 0xffff, 0xabcd0200u};
        static const int32_t zlat[] = {1, 2, 0x57, 0x100, 0x200, 0x3ab, 0x1000,
                                       0x7fff, 0x8000, 0xffff, 0, -1};
        static const int16_t dqalat[] = {0, 1, -1, 0x7fff, -0x8000, 0x123};
        static const int32_t dqblat[] = {0, 1, -1, 0x7fffffff, INT32_MIN, 0x1000000,
                                         -0x1000000};
        const size_t ntr = sizeof trlat / sizeof trlat[0];
        const size_t no = sizeof olat / sizeof olat[0];
        const size_t nh = sizeof hlat / sizeof hlat[0];
        const size_t nz = sizeof zlat / sizeof zlat[0];
        const size_t ndqa = sizeof dqalat / sizeof dqalat[0];
        const size_t ndqb = sizeof dqblat / sizeof dqblat[0];

        for (unsigned long i = 0; i < 1500000ul; i++) {
            ref_gte_t g;

            ref_init(&g);
            g.tr[0] = trlat[(i * 7u) % ntr];
            g.tr[1] = trlat[(i * 11u + 3u) % ntr];
            g.tr[2] = zlat[(i * 5u) % nz];
            g.h_written = hlat[(i * 13u + 1u) % nh];
            g.ofx = olat[(i * 17u) % no];
            g.ofy = olat[(i * 19u + 5u) % no];
            g.dqa = dqalat[(i * 23u) % ndqa];
            g.dqb = dqblat[(i * 29u + 2u) % ndqb];

            if (!gm_run_vec(cpu, &g, gm_enc(0x01, (int)((i >> 1) & 1), (int)(i & 1),
                                            GM_DC_RTP),
                            "rtps-project-clamp", &st))
                if (st.failed > 32)
                    break;
        }
    }

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=rtps-project-clamp checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=rtps-project-clamp checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: rtps-mac44 -- per-step 44-bit sign-expansion in the dot products.
   ================================================================================== */
static int case_rtps_mac44(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};

    printf("GTE_MATRIX begin case=rtps-mac44\n");
    fflush(stdout);

    /* Exact 2^43 edge vectors: TRX<<12 = 0x7FFFFFFF000; +0xFFF hits 2^43-1 (no flag),
       +0x1000 hits 2^43 (flag + wrap that the following products then act on). */
    {
        typedef struct { int32_t tr; int16_t m0, m1, m2; int16_t vx, vy, vz; } edge_t;

        static const edge_t edges[] = {
            {0x7fffffff, 3, 0, 0, 0x555, 0, 0},        /* +2^43-1 exactly */
            {0x7fffffff, 1, 0, 0, 0x1000, 0, 0},       /* +2^43 exactly */
            {INT32_MIN, -1, 0, 0, -1, 0, 0},           /* -2^43+1: near edge, no flag */
            {INT32_MIN, 1, 0, 0, -1, 0, 0},            /* -2^43-1 exactly */
            {INT32_MIN, 0, 0, 0, 0, 0, 0},             /* -2^43 exactly, no flag */
            {0x7fffffff, 1, 0x7fff, 0x7fff, 0x1000, 0x7fff, 0x7fff}, /* wrap then add */
            {0x7fffffff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff},
            {INT32_MIN, -0x8000, -0x8000, -0x8000, 0x7fff, 0x7fff, 0x7fff},
            {0x7fffffff, -0x8000, 0x7fff, -0x8000, 0x7fff, -0x8000, 0x7fff},
        };

        for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
            for (int sf = 0; sf < 2; sf++) {
                for (int lm = 0; lm < 2; lm++) {
                    ref_gte_t g;

                    ref_init(&g);
                    for (int r = 0; r < 3; r++) {
                        g.tr[r] = edges[i].tr;
                        g.rt[r][0] = edges[i].m0;
                        g.rt[r][1] = edges[i].m1;
                        g.rt[r][2] = edges[i].m2;
                    }
                    g.vx[0] = edges[i].vx;
                    g.vy[0] = edges[i].vy;
                    g.vz[0] = edges[i].vz;
                    g.h_written = 0x100;

                    gm_run_vec(cpu, &g, gm_enc(0x01, sf, lm, GM_DC_RTP),
                               "rtps-mac44", &st);
                }
            }
        }
    }

    /* Full cross over per-row extremes: every combination of translation and product
       signs, all three accumulation steps stressed. */
    {
        static const int32_t trs[] = {0x7fffffff, INT32_MIN, 0x7ffff000, -0x7ffff000, 0};
        static const int16_t m0s[] = {0x7fff, -0x8000, 1, -1, 0};
        static const int16_t vxs[] = {0x7fff, -0x8000, 0x4000, 1, 0};
        static const int16_t m1s[] = {0x7fff, -0x8000, 0};
        static const int16_t vys[] = {0x7fff, -0x8000, 1};
        static const int16_t m2s[] = {0x7fff, -0x8000, 0};
        static const int16_t vzs[] = {0x7fff, -0x8000, 1};

        for (size_t a = 0; a < 5; a++)
        for (size_t b = 0; b < 5; b++)
        for (size_t c = 0; c < 5; c++)
        for (size_t d = 0; d < 3; d++)
        for (size_t e = 0; e < 3; e++)
        for (size_t f = 0; f < 3; f++)
        for (size_t h = 0; h < 3; h++) {
            int sf = (int)((a + b + c + d + e + f + h) & 1);
            int lm = (int)((a ^ b ^ c ^ d) & 1);
            ref_gte_t g;

            ref_init(&g);
            for (int r = 0; r < 3; r++) {
                g.tr[r] = trs[(a + r) % 5];
                g.rt[r][0] = m0s[(b + r) % 5];
                g.rt[r][1] = m1s[(d + r) % 3];
                g.rt[r][2] = m2s[(f + r) % 3];
            }
            g.vx[0] = vxs[c];
            g.vy[0] = vys[e];
            g.vz[0] = vzs[h];
            g.h_written = 0x1000;

            if (!gm_run_vec(cpu, &g, gm_enc(0x01, sf, lm, GM_DC_RTP), "rtps-mac44", &st))
                if (st.failed > 32)
                    goto out;
        }
    }

out:
    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=rtps-mac44 checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=rtps-mac44 checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: rtpt-fifo -- three-vertex FIFO end state and flag accumulation.
   ================================================================================== */
static int case_rtpt_fifo(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    static const int16_t vlat[] = {0, 1, -1, 0x7fff, -0x8000, 0x1000, -0x1000, 0x555};
    const size_t nv = sizeof vlat / sizeof vlat[0];

    printf("GTE_MATRIX begin case=rtpt-fifo\n");
    fflush(stdout);

    for (unsigned long i = 0; i < 300000ul; i++) {
        ref_gte_t g;

        ref_init(&g);

        /* Identity-ish rotation (1.0 = 0x1000) with occasional extremes. */
        g.rt[0][0] = (i % 7u) ? 0x1000 : 0x7fff;
        g.rt[1][1] = (i % 5u) ? 0x1000 : -0x8000;
        g.rt[2][2] = (i % 3u) ? 0x1000 : 0x1;
        g.rt[0][1] = (int16_t)((i % 11u) * 0x111u);
        g.rt[1][2] = (int16_t)(-(int32_t)((i % 13u) * 0x99u));

        for (int v = 0; v < 3; v++) {
            g.vx[v] = vlat[(i * 3u + (unsigned long)v) % nv];
            g.vy[v] = vlat[(i * 7u + (unsigned long)v * 2u) % nv];
            g.vz[v] = vlat[(i * 11u + (unsigned long)v * 5u) % nv];
        }

        g.tr[0] = (int32_t)(i % 3u ? 0x100 : -0x77777);
        g.tr[1] = (int32_t)(i % 4u ? -0x100 : 0x66666);
        g.tr[2] = (int32_t)((i * 37u) % 0x18000u); /* SZ3 in and out of range */
        g.h_written = (uint32_t)((i * 101u) & 0xffffu);
        g.ofx = (uint32_t)(i * 0x1111u);
        g.ofy = (uint32_t)(0u - (uint32_t)(i * 0x2222u));
        g.dqa = (int16_t)(i * 5u);
        g.dqb = (int32_t)(i * 0x100u);

        if (!gm_run_vec(cpu, &g, gm_enc(0x30, (int)(i & 1), (int)((i >> 1) & 1),
                                        GM_DC_RTP),
                        "rtpt-fifo", &st))
            if (st.failed > 32)
                break;

        /* And an RTPS on the same state: single push against triple push. */
        if ((i % 9u) == 0) {
            if (!gm_run_vec(cpu, &g, gm_enc(0x01, (int)(i & 1), (int)((i >> 1) & 1),
                                            GM_DC_RTP),
                            "rtpt-fifo", &st))
                if (st.failed > 32)
                    break;
        }
    }

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=rtpt-fifo checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=rtpt-fifo checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: nclip-matrix -- dense boundary lattice plus exact +/-2^31 vectors.
   ================================================================================== */
static int case_nclip_matrix(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    static const int16_t lat[] = {
        -0x8000, -0x7fff, -0x401, -0x400, -0x3ff, -2, -1, 0, 1, 2,
        0x3ff, 0x400, 0x7ffe, 0x7fff
    };
    const size_t n = sizeof lat / sizeof lat[0];

    printf("GTE_MATRIX begin case=nclip-matrix\n");
    fflush(stdout);

    /* Exact MAC0 32-bit overflow edges (worked out by hand; see the winding formula):
       (-0x8000, -0x7ffd, 1) x (-0x7fff, 0x7fff, -0x8000) lands exactly -2^31-1. */
    {
        typedef struct { int16_t x0, x1, x2, y0, y1, y2; } nv_t;

        static const nv_t named[] = {
            {-0x8000, -0x7ffd, 1, -0x7fff, 0x7fff, -0x8000},  /* -2^31-1: flag 15 */
            {-0x8000, -0x7ffe, 1, -0x7fff, 0x7fff, -0x8000},  /* -2^31:   no flag */
            {0x7fff, 0x7ffd, -1, 0x7fff, -0x8000, 0x7fff},    /* positive mirrors */
            {-0x8000, 0x7fff, 0, -0x8000, 0x7fff, -0x8000},
            {0, 0, 0, 0, 0, 0},                               /* degenerate zero */
            {5, 5, 5, -7, -7, -7},                            /* collinear zero */
            {100, 200, 300, 100, 200, 300},                   /* collinear zero */
            {1, 0, 0, 0, 1, 0},                               /* winding +1 */
            {0, 1, 0, 0, 0, 1},                               /* winding +1 */
            {1, 0, 0, 0, 0, 1},                               /* winding -1 */
        };

        for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
            ref_gte_t g;

            ref_init(&g);
            g.sx[0] = named[i].x0; g.sx[1] = named[i].x1; g.sx[2] = named[i].x2;
            g.sy[0] = named[i].y0; g.sy[1] = named[i].y1; g.sy[2] = named[i].y2;

            gm_run_vec(cpu, &g, gm_enc(0x06, 0, 0, GM_DC_ALL), "nclip-matrix", &st);
        }
    }

    for (size_t a = 0; a < n; a++)
    for (size_t b = 0; b < n; b++)
    for (size_t c = 0; c < n; c++)
    for (size_t d = 0; d < n; d++)
    for (size_t e = 0; e < n; e++)
    for (size_t f = 0; f < n; f++) {
        ref_gte_t g;

        ref_init(&g);
        g.sx[0] = lat[a];
        g.sx[1] = lat[b];
        g.sx[2] = lat[c];
        g.sy[0] = lat[d];
        g.sy[1] = lat[e];
        g.sy[2] = lat[f];

        if (!gm_run_vec(cpu, &g, gm_enc(0x06, 0, 0, GM_DC_ALL), "nclip-matrix", &st))
            if (st.failed > 32)
                goto out;
    }

out:
    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=nclip-matrix checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=nclip-matrix checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: avsz-matrix -- full ZSF sweep, SZ boundary combos, OTZ and MAC0 edges.
   ================================================================================== */
static int case_avsz_matrix(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};

    typedef struct { uint16_t s0, s1, s2, s3; } szc_t;

    static const szc_t combos[] = {
        {0, 0, 0, 0},
        {0, 0, 0, 1},
        {0x1234, 1, 1, 1},
        {0xffff, 0xffff, 0xffff, 0xffff},
        {0, 0xffff, 0, 0},
        {0x8000, 0x8000, 0x7fff, 1},
        {0xffff, 0xffff, 0xffff, 2},
        {0x4000, 0x1000, 0x2000, 0x3000},
        {0xabcd, 3, 0xffff, 0xfffe},
        {0x2222, 0xffff, 1, 0},
    };

    printf("GTE_MATRIX begin case=avsz-matrix\n");
    fflush(stdout);

    /* Named boundary vectors: OTZ saturation edge and MAC0 crossings. */
    {
        typedef struct { int16_t zsf; szc_t sz; int four; } av_t;

        static const av_t named[] = {
            {0x1000, {0, 0xffff, 0, 0}, 0},       /* sum=0xffff -> OTZ=0xffff, no flag */
            {0x1000, {0, 0xffff, 1, 0}, 0},       /* sum=0x10000 -> sat + flag 18 */
            {0x1000, {0xffff, 0, 0, 1}, 1},       /* AVSZ4 same edge */
            {0x1000, {0xffff, 1, 0, 0}, 1},
            {0x2aab, {0, 0xffff, 0xffff, 0xffff}, 0}, /* 0x80007fff: flag 16 */
            {0x2aaa, {0, 0xffff, 0xffff, 0xffff}, 0}, /* 0x7ffd0002: no flag  */
            {-0x2aab, {0, 0xffff, 0xffff, 0xffff}, 0}, /* negative mirror: flag 15 */
            {-0x2aaa, {0, 0xffff, 0xffff, 0xffff}, 0},
            {0x7fff, {0xffff, 0xffff, 0xffff, 0xffff}, 1},
            {-0x8000, {0xffff, 0xffff, 0xffff, 0xffff}, 1},
            {-1, {0, 0, 0, 1}, 0},                /* small negative -> OTZ 0 + flag */
        };

        for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
            ref_gte_t g;

            ref_init(&g);
            g.sz[0] = named[i].sz.s0;
            g.sz[1] = named[i].sz.s1;
            g.sz[2] = named[i].sz.s2;
            g.sz[3] = named[i].sz.s3;
            if (named[i].four)
                g.zsf4 = named[i].zsf;
            else
                g.zsf3 = named[i].zsf;

            gm_run_vec(cpu, &g, gm_enc(named[i].four ? 0x2e : 0x2d, 0, 0, GM_DC_ALL),
                       "avsz-matrix", &st);
        }
    }

    for (uint32_t zsf = 0; zsf <= 0xffffu; zsf++) {
        for (size_t ci = 0; ci < sizeof combos / sizeof combos[0]; ci += 2) {
            size_t c = (ci + zsf) % (sizeof combos / sizeof combos[0]);
            ref_gte_t g;

            ref_init(&g);
            g.zsf3 = (int16_t)zsf;
            g.zsf4 = (int16_t)(zsf ^ 0x8000u);
            g.sz[0] = combos[c].s0;
            g.sz[1] = combos[c].s1;
            g.sz[2] = combos[c].s2;
            g.sz[3] = combos[c].s3;

            if (!gm_run_vec(cpu, &g, gm_enc((zsf & 1) ? 0x2e : 0x2d, 0, 0, GM_DC_ALL),
                            "avsz-matrix", &st))
                if (st.failed > 32)
                    goto out;
        }
    }

out:
    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=avsz-matrix checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=avsz-matrix checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: mvmva-matrix -- all mx/v/cv/sf/lm combinations including the CV=FC bug.
   ================================================================================== */
static int case_mvmva_matrix(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    static const int16_t s16lat[] = {0, 1, -1, 0x7fff, -0x8000, 0x1000, -0x1000,
                                     0x123, -0x456};
    static const int32_t s32lat[] = {0, 1, -1, 0x7fffffff, INT32_MIN, 0xfffff,
                                     -0xfffff, 0x12345678};
    const size_t n16 = sizeof s16lat / sizeof s16lat[0];
    const size_t n32 = sizeof s32lat / sizeof s32lat[0];

    printf("GTE_MATRIX begin case=mvmva-matrix\n");
    fflush(stdout);

    for (int mx = 0; mx < 4; mx++)
    for (int v = 0; v < 4; v++)
    for (int cv = 0; cv < 4; cv++)
    for (int sf = 0; sf < 2; sf++)
    for (int lm = 0; lm < 2; lm++) {
        for (unsigned long i = 0; i < 2000ul; i++) {
            ref_gte_t g;
            uint32_t op;

            ref_init(&g);

            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    g.rt[r][c] = s16lat[(i * 3u + (unsigned long)(r * 3 + c)) % n16];
                    g.l[r][c] = s16lat[(i * 5u + (unsigned long)(r * 3 + c) * 2u) % n16];
                    g.lr[r][c] = s16lat[(i * 7u + (unsigned long)(r * 3 + c) * 3u) % n16];
                }

                g.tr[r] = s32lat[(i * 11u + (unsigned long)r) % n32];
                g.bk[r] = s32lat[(i * 13u + (unsigned long)r * 2u) % n32];
                g.fc[r] = s32lat[(i * 17u + (unsigned long)r * 3u) % n32];
                g.vx[r] = s16lat[(i * 19u + (unsigned long)r) % n16];
                g.vy[r] = s16lat[(i * 23u + (unsigned long)r * 2u) % n16];
                g.vz[r] = s16lat[(i * 29u + (unsigned long)r * 3u) % n16];
            }

            g.ir0 = s16lat[(i * 31u) % n16];
            g.ir1 = s16lat[(i * 37u + 1u) % n16];
            g.ir2 = s16lat[(i * 41u + 2u) % n16];
            g.ir3 = s16lat[(i * 43u + 3u) % n16];
            g.rgbc = 0xb3b2b100u | (uint32_t)((i * 47u) & 0xffu);

            op = 0x4a000000u | 0x12u | ((uint32_t)mx << 17) | ((uint32_t)v << 15) |
                 ((uint32_t)cv << 13) | (sf ? (1u << 19) : 0) | (lm ? (1u << 10) : 0) |
                 (gm_rnd() & GM_DC_MVM);

            if (!gm_run_vec(cpu, &g, op, "mvmva-matrix", &st))
                if (st.failed > 32)
                    goto out;
        }
    }

out:
    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=mvmva-matrix checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=mvmva-matrix checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: dispatch-parity -- one vector, four dispatch layers, identical COP2 state.
   The GTE latch decode (sf/lm/mx/v/cv) is textually duplicated in the interpreter
   switch, the cached-handler wrappers and the IRQ fast path; cpu_differential only
   ever encoded sf=0/lm=0, so a slip in one copy was invisible until now.
   ================================================================================== */
static int case_dispatch_parity(psx_t* ma, psx_t* mb, psx_t* mc, psx_t* md) {
    gm_stats_t st = {0, 0, 0};

    printf("GTE_MATRIX begin case=dispatch-parity\n");
    fflush(stdout);

    psx_cpu_set_execution_mode(mc->cpu, PSX_CPU_INTERPRETER);
    psx_cpu_set_execution_mode(md->cpu, PSX_CPU_CACHED_INTERPRETER);

    for (unsigned long i = 0; i < 15000ul; i++) {
        ref_gte_t g;
        ref_gte_t post;
        uint32_t op;
        static const uint32_t functs[] = {0x01, 0x30, 0x06, 0x2d, 0x2e, 0x12};
        uint32_t funct = functs[i % 6u];

        ref_init(&g);
        g.tr[0] = (int32_t)(gm_rnd() & 0x1ffffu) - 0x10000;
        g.tr[1] = (int32_t)(gm_rnd() & 0x1ffffu) - 0x10000;
        g.tr[2] = (int32_t)(gm_rnd() & 0x1ffffu);
        g.h_written = gm_rnd() & 0xffffu;
        g.ofx = gm_rnd();
        g.ofy = gm_rnd();
        g.dqa = (int16_t)gm_rnd();
        g.dqb = (int32_t)gm_rnd();
        g.zsf3 = (int16_t)gm_rnd();
        g.zsf4 = (int16_t)gm_rnd();

        for (int r = 0; r < 3; r++) {
            g.rt[r][0] = (int16_t)gm_rnd();
            g.rt[r][1] = (int16_t)gm_rnd();
            g.rt[r][2] = (int16_t)gm_rnd();
            g.l[r][0] = (int16_t)gm_rnd();
            g.l[r][2] = (int16_t)gm_rnd();
            g.lr[r][1] = (int16_t)gm_rnd();
            g.bk[r] = (int32_t)gm_rnd();
            g.fc[r] = (int32_t)gm_rnd();
            g.vx[r] = (int16_t)gm_rnd();
            g.vy[r] = (int16_t)gm_rnd();
            g.vz[r] = (int16_t)gm_rnd();
            g.sx[r] = (int16_t)gm_rnd();
            g.sy[r] = (int16_t)gm_rnd();
        }

        g.sz[0] = (uint16_t)gm_rnd();
        g.sz[1] = (uint16_t)gm_rnd();
        g.sz[2] = (uint16_t)gm_rnd();
        g.sz[3] = (uint16_t)gm_rnd();

        op = 0x4a000000u | funct | (gm_rnd() & (funct == 0x12 ? 0x000fe000u : 0u)) |
             ((gm_rnd() & 1u) << 19) | ((gm_rnd() & 1u) << 10) |
             (gm_rnd() & GM_DC_MVM);

        post = g;
        if (!ref_execute(&post, op))
            continue;

        /* Path A: psx_cpu_execute with the opcode latched. */
        gm_apply(ma->cpu, &g);
        ma->cpu->opcode = op;
        psx_cpu_execute(ma->cpu);

        /* Path B: the decode-cache handler, exactly what the cached interpreter runs. */
        {
            psx_cpu_cached_handler_t handler = psx_cpu_decode(op);

            gm_apply(mb->cpu, &g);
            mb->cpu->opcode = op;

            if (!handler) {
                fprintf(stderr, "GTE_MATRIX failed case=dispatch-parity reason=no-handler "
                                "opcode=%08x\n", op);
                st.failed++;
                break;
            }

            handler(mb->cpu);
        }

        /* Paths C and D: a real fetched cycle in each execution mode. */
        gm_apply(mc->cpu, &g);
        psx_bus_write32(mc->bus, GM_TEST_OFFSET, op);
        mc->cpu->pc = GM_TEST_PC;
        mc->cpu->next_pc = GM_TEST_PC + 4;
        psx_cpu_cycle(mc->cpu);

        gm_apply(md->cpu, &g);
        psx_bus_write32(md->bus, GM_TEST_OFFSET, op);
        md->cpu->pc = GM_TEST_PC;
        md->cpu->next_pc = GM_TEST_PC + 4;
        psx_cpu_cycle(md->cpu);

        if (mc->cpu->pc != GM_TEST_PC + 4 || md->cpu->pc != GM_TEST_PC + 4) {
            fprintf(stderr, "GTE_MATRIX failed case=dispatch-parity reason=unexpected-pc "
                            "opcode=%08x pc_c=%08x pc_d=%08x\n",
                    op, mc->cpu->pc, md->cpu->pc);
            st.failed++;
            break;
        }

        {
            int bad = 0;

            for (int r = 0; r < 64; r++) {
                uint32_t wa, wb, wc, wd, want;

                if (r == 28 || r == 29)
                    continue;

                wa = gte_read_register(ma->cpu, (uint32_t)r);
                wb = gte_read_register(mb->cpu, (uint32_t)r);
                wc = gte_read_register(mc->cpu, (uint32_t)r);
                wd = gte_read_register(md->cpu, (uint32_t)r);
                want = ref_expected_read(&post, r);

                if (wa != want || wb != wa || wc != wa || wd != wa) {
                    if (st.printed < GM_PRINT_CAP) {
                        if (!bad) {
                            fprintf(stderr,
                                    "GTE_MATRIX failed case=dispatch-parity opcode=%08x\n",
                                    op);
                            gm_print_state(&g, op);
                        }

                        fprintf(stderr,
                                "  reg%-2d want=%08x execute=%08x handler=%08x "
                                "cycle_int=%08x cycle_cached=%08x\n",
                                r, want, wa, wb, wc, wd);
                    }

                    bad = 1;
                }
            }

            st.checked++;

            if (bad) {
                st.failed++;
                if (st.printed < GM_PRINT_CAP)
                    st.printed++;
            }
        }

        if (st.failed > 32)
            break;
    }

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=dispatch-parity checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=dispatch-parity checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Case: pgxp-inert -- with PGXP enabled, the GTE hooks must not change one
   architectural bit. (The precision shadow is a side channel; the register file is
   the contract.)
   ================================================================================== */
static int case_pgxp_inert(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};

    printf("GTE_MATRIX begin case=pgxp-inert\n");
    fflush(stdout);

    psx_pgxp_set_enabled(1);

    if (!psx_pgxp_active()) {
        fprintf(stderr, "GTE_MATRIX failed case=pgxp-inert reason=enable-refused\n");
        return 0;
    }

    for (unsigned long i = 0; i < 60000ul; i++) {
        ref_gte_t g;
        static const uint32_t functs[] = {0x01, 0x30, 0x06};
        uint32_t funct = functs[i % 3u];

        ref_init(&g);
        g.tr[0] = (int32_t)(gm_rnd() & 0xffffu) - 0x8000;
        g.tr[1] = (int32_t)(gm_rnd() & 0xffffu) - 0x8000;
        g.tr[2] = (int32_t)(gm_rnd() & 0x1ffffu);
        g.h_written = gm_rnd() & 0xffffu;
        g.ofx = gm_rnd();
        g.ofy = gm_rnd();
        g.dqa = (int16_t)gm_rnd();
        g.dqb = (int32_t)gm_rnd();
        g.rt[0][0] = g.rt[1][1] = g.rt[2][2] = 0x1000;

        for (int r = 0; r < 3; r++) {
            g.vx[r] = (int16_t)gm_rnd();
            g.vy[r] = (int16_t)gm_rnd();
            g.vz[r] = (int16_t)(gm_rnd() & 0x7fffu);
            g.sx[r] = (int16_t)gm_rnd();
            g.sy[r] = (int16_t)gm_rnd();
        }

        if (!gm_run_vec(cpu, &g, gm_enc(funct, (int)(i & 1), (int)((i >> 1) & 1),
                                        funct == 0x06 ? GM_DC_ALL : GM_DC_RTP),
                        "pgxp-inert", &st))
            if (st.failed > 32)
                break;
    }

    psx_pgxp_set_enabled(0);

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=pgxp-inert checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=pgxp-inert checked=%lu\n", st.checked);
    return 1;
}

/* ==================================================================================
   Regfile cases: access semantics per psx-spx.
   ================================================================================== */
static int gm_expect_reg(psx_cpu_t* cpu, uint32_t r, uint32_t want, const char* casename,
                         gm_stats_t* st) {
    uint32_t got = gte_read_register(cpu, r);

    st->checked++;

    if (got != want) {
        if (st->printed < GM_PRINT_CAP) {
            fprintf(stderr, "GTE_MATRIX failed case=%s reg=%u got=%08x want=%08x\n",
                    casename, r, got, want);
            st->printed++;
        }

        st->failed++;
        return 0;
    }

    return 1;
}

static int case_regfile_widths(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    ref_gte_t g;

    printf("GTE_MATRIX begin case=regfile-widths\n");

    ref_init(&g);
    gm_apply(cpu, &g);

    /* 16-bit sign-expanding data registers: garbage in the upper half must vanish. */
    {
        static const uint32_t pat[] = {0x00000000u, 0x00007fffu, 0x00008000u,
                                       0x0000ffffu, 0x12345678u, 0xabcd8000u,
                                       0xffff7fffu, 0x80000001u};

        for (size_t i = 0; i < sizeof pat / sizeof pat[0]; i++) {
            uint32_t p = pat[i];

            /* VZ0/VZ1/VZ2, IR0..IR3: int16, read sign-expanded. */
            gte_write_register(cpu, 1, p);
            gm_expect_reg(cpu, 1, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 3, p);
            gm_expect_reg(cpu, 3, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 5, p);
            gm_expect_reg(cpu, 5, ref_sext16(p), "regfile-widths", &st);

            for (uint32_t r = 8; r <= 11; r++) {
                gte_write_register(cpu, r, p);
                gm_expect_reg(cpu, r, ref_sext16(p), "regfile-widths", &st);
            }

            /* OTZ and the SZ FIFO: uint16, read zero-expanded. */
            gte_write_register(cpu, 7, p);
            gm_expect_reg(cpu, 7, p & 0xffffu, "regfile-widths", &st);

            for (uint32_t r = 16; r <= 19; r++) {
                gte_write_register(cpu, r, p);
                gm_expect_reg(cpu, r, p & 0xffffu, "regfile-widths", &st);
            }

            /* Raw 32-bit registers. */
            for (uint32_t r = 24; r <= 27; r++) {
                gte_write_register(cpu, r, p);
                gm_expect_reg(cpu, r, p, "regfile-widths", &st);
            }

            gte_write_register(cpu, 6, p);
            gm_expect_reg(cpu, 6, p, "regfile-widths", &st);
            gte_write_register(cpu, 23, p);
            gm_expect_reg(cpu, 23, p, "regfile-widths", &st);

            /* Packed SXY (raw), and the SXYP read mirror of SXY2. */
            gte_write_register(cpu, 12, p);
            gm_expect_reg(cpu, 12, p, "regfile-widths", &st);
            gte_write_register(cpu, 14, p);
            gm_expect_reg(cpu, 14, p, "regfile-widths", &st);
            gm_expect_reg(cpu, 15, p, "regfile-widths", &st);

            /* Control side: matrix diagonals sign-expand, H sign-expands although it
               is used unsigned, DQA/ZSF sign-expand, the rest are raw. */
            gte_write_register(cpu, 36, p);
            gm_expect_reg(cpu, 36, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 44, p);
            gm_expect_reg(cpu, 44, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 52, p);
            gm_expect_reg(cpu, 52, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 58, p);
            gm_expect_reg(cpu, 58, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 59, p);
            gm_expect_reg(cpu, 59, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 61, p);
            gm_expect_reg(cpu, 61, ref_sext16(p), "regfile-widths", &st);
            gte_write_register(cpu, 62, p);
            gm_expect_reg(cpu, 62, ref_sext16(p), "regfile-widths", &st);

            gte_write_register(cpu, 37, p);
            gm_expect_reg(cpu, 37, p, "regfile-widths", &st);
            gte_write_register(cpu, 56, p);
            gm_expect_reg(cpu, 56, p, "regfile-widths", &st);
            gte_write_register(cpu, 60, p);
            gm_expect_reg(cpu, 60, p, "regfile-widths", &st);
            gte_write_register(cpu, 32, p);
            gm_expect_reg(cpu, 32, p, "regfile-widths", &st);
        }
    }

    /* FLAG: bits 0..11 unwritable, bit 31 derived from mask 0x7f87e000. Probe every
       individual bit. */
    for (int bit = 0; bit < 32; bit++) {
        uint32_t p = 1u << bit;
        uint32_t stored = p & 0x7ffff000u;
        uint32_t want = stored | ((stored & 0x7f87e000u) ? 0x80000000u : 0u);

        gte_write_register(cpu, 63, p);
        gm_expect_reg(cpu, 63, want, "regfile-widths", &st);
    }

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=regfile-widths checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=regfile-widths checked=%lu\n", st.checked);
    return 1;
}

static int case_regfile_sxyp_push(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    ref_gte_t g;

    printf("GTE_MATRIX begin case=regfile-sxyp-push\n");

    ref_init(&g);
    gm_apply(cpu, &g);

    gte_write_register(cpu, 12, 0x00010002u);
    gte_write_register(cpu, 13, 0x00030004u);
    gte_write_register(cpu, 14, 0x00050006u);

    /* Writing SXYP pushes: SXY0=SXY1, SXY1=SXY2, SXY2=value. Direct writes to
       SXY0..2 must NOT push. */
    gte_write_register(cpu, 15, 0xaaaabbbbu);
    gm_expect_reg(cpu, 12, 0x00030004u, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 13, 0x00050006u, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 14, 0xaaaabbbbu, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 15, 0xaaaabbbbu, "regfile-sxyp-push", &st);

    gte_write_register(cpu, 15, 0xccccddddu);
    gm_expect_reg(cpu, 12, 0x00050006u, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 13, 0xaaaabbbbu, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 14, 0xccccddddu, "regfile-sxyp-push", &st);

    gte_write_register(cpu, 13, 0x11112222u);
    gm_expect_reg(cpu, 12, 0x00050006u, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 13, 0x11112222u, "regfile-sxyp-push", &st);
    gm_expect_reg(cpu, 14, 0xccccddddu, "regfile-sxyp-push", &st);

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=regfile-sxyp-push failures=%lu\n", st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=regfile-sxyp-push checked=%lu\n", st.checked);
    return 1;
}

static int case_regfile_irgb_lzc(psx_cpu_t* cpu) {
    gm_stats_t st = {0, 0, 0};
    ref_gte_t g;
    unsigned long orgb_stale = 0;

    printf("GTE_MATRIX begin case=regfile-irgb-lzc\n");

    ref_init(&g);
    gm_apply(cpu, &g);

    /* IRGB write distributes each 5-bit field * 0x80 into IR1..IR3. */
    gte_write_register(cpu, 28, 0xffffabcdu);
    gm_expect_reg(cpu, 9, ((0xabcdu >> 0) & 0x1fu) * 0x80u, "regfile-irgb-lzc", &st);
    gm_expect_reg(cpu, 10, ((0xabcdu >> 5) & 0x1fu) * 0x80u, "regfile-irgb-lzc", &st);
    gm_expect_reg(cpu, 11, ((0xabcdu >> 10) & 0x1fu) * 0x80u, "regfile-irgb-lzc", &st);
    gm_expect_reg(cpu, 28, 0xabcdu & 0x7fffu, "regfile-irgb-lzc", &st);

    /* IRGB read recomputes from IR1..IR3 with saturation. */
    gte_write_register(cpu, 9, 0x7fffu);
    gte_write_register(cpu, 10, (uint32_t)(int32_t)-0x1000);
    gte_write_register(cpu, 11, 0x0200u);
    {
        uint32_t want28 = 0x1fu | (0u << 5) | ((0x0200u >> 7) << 10);

        /* ORGB (reg 29) is documented as the same conversion. The implementation
           returns the last stored IRGB word instead (stale until a reg-28 read
           refreshes it). Fix-hunt is stood down by order: REPORT, do not fail. */
        {
            uint32_t got29 = gte_read_register(cpu, 29);

            if (got29 != want28) {
                orgb_stale++;
                fprintf(stderr,
                        "GTE_MATRIX reported-divergence case=regfile-irgb-lzc reg=29 "
                        "got=%08x want=%08x note=ORGB-read-does-not-recompute-"
                        "(psx-spx:-mirror-of-IRGB-conversion);-stale-until-reg28-read;-"
                        "reported-only-per-standing-order,-not-a-gate-failure\n",
                        got29, want28);
            }
        }

        gm_expect_reg(cpu, 28, want28, "regfile-irgb-lzc", &st);
        /* After a reg-28 read the mirror is coherent again in the implementation;
           conversion value is the spec answer either way. */
        gm_expect_reg(cpu, 29, want28, "regfile-irgb-lzc", &st);
    }

    /* LZCS/LZCR: leading zeroes of a positive word, leading ones of a negative one. */
    {
        static const struct { uint32_t v; uint32_t want; } lz[] = {
            {0x00000000u, 32}, {0xffffffffu, 32}, {0x00000001u, 31},
            {0x80000000u, 1},  {0x7fffffffu, 1},  {0xc0000000u, 2},
            {0x3fffffffu, 2},  {0x0000ffffu, 16}, {0xffff0000u, 16},
            {0xa5a5a5a5u, 1},  {0x00ff00ffu, 8},
        };

        for (size_t i = 0; i < sizeof lz / sizeof lz[0]; i++) {
            gte_write_register(cpu, 30, lz[i].v);
            gm_expect_reg(cpu, 30, lz[i].v, "regfile-irgb-lzc", &st);
            gm_expect_reg(cpu, 31, lz[i].want, "regfile-irgb-lzc", &st);
        }

        for (int bit = 0; bit < 32; bit++) {
            uint32_t v = 1u << bit;
            uint32_t want = (v & 0x80000000u) ? 1u : (uint32_t)(31 - bit);

            gte_write_register(cpu, 30, v);
            gm_expect_reg(cpu, 31, want, "regfile-irgb-lzc", &st);

            v = ~(1u << bit);
            want = (v & 0x80000000u)
                       ? (uint32_t)((bit == 31) ? 1 : 31 - bit)
                       : 1u; /* bit31 clear: word starts with a 0 run of length 1 */
            gte_write_register(cpu, 30, v);
            gm_expect_reg(cpu, 31, want, "regfile-irgb-lzc", &st);
        }
    }

    /* Read-only registers: writes to ORGB (29) and LZCR (31) are dropped. */
    {
        uint32_t lzcr_before = gte_read_register(cpu, 31);

        gte_write_register(cpu, 31, 0xdeadbeefu);
        gm_expect_reg(cpu, 31, lzcr_before, "regfile-irgb-lzc", &st);
    }

    if (st.failed) {
        fprintf(stderr, "GTE_MATRIX failed case=regfile-irgb-lzc checked=%lu failures=%lu\n",
                st.checked, st.failed);
        return 0;
    }

    printf("GTE_MATRIX passed case=regfile-irgb-lzc checked=%lu reported_divergences=%lu\n",
           st.checked, orgb_stale);
    return 1;
}

/* The COP2 move/load/store instruction wrappers through real fetched cycles, both
   execution modes: MTC2 -> MFC2 (with the load delay), CTC2 -> CFC2 (H sign-expansion
   quirk through the real read), LWC2 -> SWC2 round trip through RAM. */
static int case_cop2_moves(psx_t* m) {
    gm_stats_t st = {0, 0, 0};
    int modes[2] = {PSX_CPU_INTERPRETER, PSX_CPU_CACHED_INTERPRETER};

    printf("GTE_MATRIX begin case=cop2-moves\n");

    for (int mi = 0; mi < 2; mi++) {
        ref_gte_t g;
        uint32_t program[10];
        size_t n = 0;

        psx_cpu_set_execution_mode(m->cpu, (psx_cpu_execution_mode_t)modes[mi]);

        ref_init(&g);
        gm_apply(m->cpu, &g);

        /* r1 = source patterns staged directly; the program moves them around. */
        m->cpu->r[1] = 0xabcd8001u; /* -> IR1 (data 9) */
        m->cpu->r[2] = 0x1234f234u; /* -> H (control 58) */
        m->cpu->r[3] = 0x80002000u; /* base address for LWC2/SWC2 */
        m->cpu->r[4] = 0;
        m->cpu->r[5] = 0;

        psx_bus_write32(m->bus, 0x2000u, 0x00058006u); /* memory word for LWC2 -> SXY0 */

        program[n++] = (0x12u << 26) | (0x04u << 21) | (1u << 16) | (9u << 11);  /* mtc2 r1 -> d9 */
        program[n++] = (0x12u << 26) | (0x06u << 21) | (2u << 16) | (26u << 11); /* ctc2 r2 -> c58 */
        program[n++] = (0x12u << 26) | (0x00u << 21) | (4u << 16) | (9u << 11);  /* mfc2 r4 <- d9 */
        program[n++] = 0;                                                        /* load delay */
        program[n++] = (0x12u << 26) | (0x02u << 21) | (5u << 16) | (26u << 11); /* cfc2 r5 <- c58 */
        program[n++] = 0;                                                        /* load delay */
        program[n++] = (0x32u << 26) | (3u << 21) | (12u << 16) | 0x0000u;       /* lwc2 d12, 0(r3) */
        program[n++] = (0x3au << 26) | (3u << 21) | (12u << 16) | 0x0010u;       /* swc2 d12, 16(r3) */
        program[n++] = 0;
        program[n++] = 0;

        for (size_t k = 0; k < n; k++)
            psx_bus_write32(m->bus, GM_TEST_OFFSET + (uint32_t)(k * 4u), program[k]);

        m->cpu->pc = GM_TEST_PC;
        m->cpu->next_pc = GM_TEST_PC + 4;

        for (size_t k = 0; k < n; k++)
            psx_cpu_cycle(m->cpu);

        st.checked++;

        if (gte_read_register(m->cpu, 9) != ref_sext16(0xabcd8001u)) {
            fprintf(stderr, "GTE_MATRIX failed case=cop2-moves mode=%d reason=mtc2 got=%08x\n",
                    modes[mi], gte_read_register(m->cpu, 9));
            st.failed++;
        }

        if (m->cpu->r[4] != ref_sext16(0xabcd8001u)) {
            fprintf(stderr, "GTE_MATRIX failed case=cop2-moves mode=%d reason=mfc2 got=%08x\n",
                    modes[mi], m->cpu->r[4]);
            st.failed++;
        }

        /* H stores the full word; reads come back as the sign-expanded low half. */
        if (m->cpu->r[5] != ref_sext16(0x1234f234u)) {
            fprintf(stderr, "GTE_MATRIX failed case=cop2-moves mode=%d reason=cfc2 got=%08x\n",
                    modes[mi], m->cpu->r[5]);
            st.failed++;
        }

        if (gte_read_register(m->cpu, 12) != 0x00058006u) {
            fprintf(stderr, "GTE_MATRIX failed case=cop2-moves mode=%d reason=lwc2 got=%08x\n",
                    modes[mi], gte_read_register(m->cpu, 12));
            st.failed++;
        }

        if (psx_bus_read32(m->bus, 0x2010u) != 0x00058006u) {
            fprintf(stderr, "GTE_MATRIX failed case=cop2-moves mode=%d reason=swc2 got=%08x\n",
                    modes[mi], psx_bus_read32(m->bus, 0x2010u));
            st.failed++;
        }
    }

    if (st.failed)
        return 0;

    printf("GTE_MATRIX passed case=cop2-moves checked=%lu\n", st.checked);
    return 1;
}

/* ================================================================================== */

static int gm_write_blank_bios(const char* path) {
    FILE* file = fopen(path, "wb");
    uint8_t block[4096] = {0};

    if (!file)
        return 0;

    for (size_t offset = 0; offset < 512u * 1024u; offset += sizeof(block)) {
        if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
            fclose(file);
            return 0;
        }
    }

    return fclose(file) == 0;
}

int main(void) {
    const char* bios_path = "build/tests/gte-blank-bios.bin";
    psx_t* main_m;
    psx_t* pa;
    psx_t* pb;
    psx_t* pc;
    psx_t* pd;
    int ok = 1;

    ref_unr_init();

    if (!gm_write_blank_bios(bios_path)) {
        fprintf(stderr, "GTE_MATRIX failed reason=create-bios path=%s\n", bios_path);
        return 1;
    }

    main_m = psx_create();
    pa = psx_create();
    pb = psx_create();
    pc = psx_create();
    pd = psx_create();

    if (!main_m || !pa || !pb || !pc || !pd ||
        psx_init(main_m, bios_path, NULL) != 0 || psx_init(pa, bios_path, NULL) != 0 ||
        psx_init(pb, bios_path, NULL) != 0 || psx_init(pc, bios_path, NULL) != 0 ||
        psx_init(pd, bios_path, NULL) != 0) {
        fprintf(stderr, "GTE_MATRIX failed reason=machine-init\n");
        return 1;
    }

    /* The widescreen hack scales the IR1 product inside RTPS; the gate tests stock
       hardware behaviour, so it must be off (it defaults off -- assert, don't assume). */
    if (psx_cpu_widescreen_hack() || psx_pgxp_active()) {
        fprintf(stderr, "GTE_MATRIX failed reason=nondefault-globals widescreen=%d pgxp=%d\n",
                psx_cpu_widescreen_hack(), psx_pgxp_active());
        return 1;
    }

    ok = case_unr_table() && ok;
    ok = ok && case_divide_exhaustive();
    ok = ok && case_rtps_divide_domain(main_m->cpu);
    ok = ok && case_rtps_project_clamp(main_m->cpu);
    ok = ok && case_rtps_mac44(main_m->cpu);
    ok = ok && case_rtpt_fifo(main_m->cpu);
    ok = ok && case_nclip_matrix(main_m->cpu);
    ok = ok && case_avsz_matrix(main_m->cpu);
    ok = ok && case_mvmva_matrix(main_m->cpu);
    ok = ok && case_dispatch_parity(pa, pb, pc, pd);
    ok = ok && case_pgxp_inert(main_m->cpu);
    ok = ok && case_regfile_widths(main_m->cpu);
    ok = ok && case_regfile_sxyp_push(main_m->cpu);
    ok = ok && case_regfile_irgb_lzc(main_m->cpu);
    ok = ok && case_cop2_moves(main_m);

    psx_destroy(main_m);
    psx_destroy(pa);
    psx_destroy(pb);
    psx_destroy(pc);
    psx_destroy(pd);

    if (!ok)
        return 1;

    printf("GTE_MATRIX all cases passed\n");
    return 0;
}
