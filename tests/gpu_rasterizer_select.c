/*
    RASTERIZER SELECTION — which backend serves the session, not what it draws.

    WHY THIS FILE EXISTS

    tests/gpu_renderer_parity.c has 24 cases and every one of them compares rasterizer
    OUTPUT: software against CPU-backend, 1x against Nx, a formula against its contract.
    None of them asks WHICH rasterizer a given configuration selects.

    That blind spot cost a full evening. frontend/gpu_hw_gl.c gained a real
    framebuffer-fetch implementation of the mask bit, which turned a long-standing
    "decline, use the CPU rasterizer" into "attach" on any device reporting
    GL_EXT_shader_framebuffer_fetch with a trusted driver. The user's Adreno 740 reports
    exactly that, so the GLES rasterizer began serving a session with
    accurate_mask_bit = true — a path no confirmed fix had ever been validated against
    ( mask-from-texel,  modulation truncation and  oversize cull
    all live in the CPU rasterizer). The PS1 BIOS stopped being able to draw its own text,
    and all four gates stayed green through the whole thing, because output parity cannot
    see a selection change.

    So this gate pins SELECTION. It is deliberately a pure-rule test with no GL context, for
    the same reason tests/gpu_profile_rules.c is: the interesting inputs are hardware nobody
    here owns, and a rule that can only be checked by shipping to a user is not a gate.

    THE INVARIANT, IN ONE LINE

    With accurate_mask_bit on and no explicit opt-in, the GLES backend must decline for
    EVERY hardware combination — including one that reports flawless support.
*/

#include <stdio.h>
#include <stdlib.h>

#include "../psx/dev/gpu.h"
#include "../frontend/gpu_hw_gl.h"

static int g_failed;

static void check(const char* name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "RASTER_SELECT failed case=%s got=%d want=%d\n", name, got, want);
        g_failed = 1;

        return;
    }

    printf("RASTER_SELECT passed case=%s\n", name);
}

int main(void) {
    /* ---- 1. THE REGRESSION ITSELF ----------------------------------------------------
       Every one of these is a device that CAN run the GL mask path. Without the opt-in all
       of them must still decline, so the CPU rasterizer keeps the session. The first row is
       the user's Adreno 740 exactly as it reported: extension present, driver trusted, not
       ANGLE — the combination that silently took over. */
    check("adreno-740-no-optin-declines",
          armsx_hw_gl_mask_bit_supported(1, 1, 0, 0), 0);
    check("fbfetch-untrusted-no-optin-declines",
          armsx_hw_gl_mask_bit_supported(1, 0, 0, 0), 0);
    check("no-fbfetch-no-optin-declines",
          armsx_hw_gl_mask_bit_supported(0, 1, 0, 0), 0);
    check("angle-no-optin-declines",
          armsx_hw_gl_mask_bit_supported(0, 0, 1, 0), 0);

    /* ---- 2. THE OPT-IN IS REAL --------------------------------------------------------
       A kill switch that cannot be switched back on is indistinguishable from deleting the
       feature, and would quietly strand the GL mask path forever. With the opt-in AND
       capable hardware, the backend must attach. */
    check("optin-with-capable-hardware-attaches",
          armsx_hw_gl_mask_bit_supported(1, 1, 0, 1), 1);

    /* ---- 3. THE OPT-IN IS NOT A BYPASS ------------------------------------------------
       Opting in must not defeat the capability checks underneath it. Framebuffer fetch is
       the only GLES mechanism that gives a fragment shader the destination the mask CHECK
       reads; without a trustworthy one the check silently does nothing. MediaTek Mali
       advertises the extension and returns stale
       destination colour, and ANGLE has been seen to crash the compiler on it. */
    check("optin-without-fbfetch-still-declines",
          armsx_hw_gl_mask_bit_supported(0, 1, 0, 1), 0);
    check("optin-untrusted-driver-still-declines",
          armsx_hw_gl_mask_bit_supported(1, 0, 0, 1), 0);
    check("optin-under-angle-still-declines",
          armsx_hw_gl_mask_bit_supported(1, 1, 1, 1), 0);

    /* ---- 4. EXHAUSTIVE ----------------------------------------------------------------
       All 16 input combinations, so the rule cannot drift into "mostly right". Only one
       combination may return 1; the no-opt-in half of the space must be uniformly 0, which
       is the invariant at the top of this file stated as an assertion. */
    {
        int combos = 0, attaches = 0, optout_attaches = 0;

        for (int fb = 0; fb < 2; fb++) {
            for (int tr = 0; tr < 2; tr++) {
                for (int angle = 0; angle < 2; angle++) {
                    for (int opt = 0; opt < 2; opt++) {
                        const int got = armsx_hw_gl_mask_bit_supported(fb, tr, angle, opt);

                        combos++;

                        if (got) {
                            attaches++;

                            if (!opt)
                                optout_attaches++;
                        }
                    }
                }
            }
        }

        if (combos != 16 || attaches != 1 || optout_attaches != 0) {
            fprintf(stderr,
                    "RASTER_SELECT failed case=exhaustive combos=%d attaches=%d "
                    "without-optin=%d (want 16/1/0)\n",
                    combos, attaches, optout_attaches);
            g_failed = 1;
        } else {
            printf("RASTER_SELECT passed case=exhaustive (16 combinations, 1 attaches)\n");
        }
    }

    /* ---- 5. THE ENVIRONMENT CHAIN -----------------------------------------------------
       "The opt-in exists" and "the opt-in works" are different claims, and the env read is
       the only link between them. A prefix match here would let ARMSX_GL_MASK_BIT=0 enable
       the very path this switch exists to keep off. */
    unsetenv("ARMSX_GL_MASK_BIT");
    check("env-unset-is-not-opted-in", armsx_hw_gl_mask_bit_opt_in(), 0);

    setenv("ARMSX_GL_MASK_BIT", "1", 1);
    check("env-1-is-opted-in", armsx_hw_gl_mask_bit_opt_in(), 1);

    setenv("ARMSX_GL_MASK_BIT", "0", 1);
    check("env-0-is-not-opted-in", armsx_hw_gl_mask_bit_opt_in(), 0);

    setenv("ARMSX_GL_MASK_BIT", "10", 1);
    check("env-10-is-not-opted-in", armsx_hw_gl_mask_bit_opt_in(), 0);

    setenv("ARMSX_GL_MASK_BIT", "", 1);
    check("env-empty-is-not-opted-in", armsx_hw_gl_mask_bit_opt_in(), 0);

    unsetenv("ARMSX_GL_MASK_BIT");

    /* ---- 6. FALLBACK COST -------------------------------------------------------------
       Auto hardware at 1x must not substitute the slow internal-resolution CPU backend
       when GLES declines: it adds no resolution there and is the measured 60-70% path on
       mobile. Higher scales still need it as the portable upscale fallback, while each
       explicit backend token remains exact. */
    check("auto-1x-falls-back-to-original-software",
          armsx_hw_gl_use_cpu_fallback(1, 1), 0);
    check("auto-2x-keeps-portable-upscale-fallback",
          armsx_hw_gl_use_cpu_fallback(1, 2), 1);
    check("explicit-cpu-1x-is-honoured",
          armsx_hw_gl_use_cpu_fallback(2, 1), 1);
    check("explicit-gl-never-substitutes-cpu",
          armsx_hw_gl_use_cpu_fallback(3, 8), 0);
    check("software-mode-never-substitutes-cpu",
          armsx_hw_gl_use_cpu_fallback(0, 8), 0);

    if (g_failed)
        return 1;

    puts("RASTER_SELECT all cases passed");

    return 0;
}
