/*
    ARMSX — Skip BIOS (fast boot).

    Boots the game directly instead of sitting through the console's startup animation.

    HOW, AND WHY NOT FROM RESET
    ---------------------------
    The BIOS is allowed to run normally from reset. It sets up the kernel — exception vectors,
    the A/B/C function tables, TTY, the CD-ROM subsystem — and games depend on all of it. Only
    once it reaches the SHELL entry point, the address where it would begin the intro animation
    and the memory-card browser, do we step in: read the game's executable off the disc, install
    it, and jump to it. Everything skipped is presentation; everything the game needs has already
    happened.

    Starting the executable straight from reset would be simpler and would break, because the
    kernel would never have been built.

    THE HOOK RUNS IN BOTH EXECUTION MODES
    ------------------------------------
    It is called from psx_cpu_cycle(), which is the single fetch/dispatch point for BOTH
    PSX_CPU_INTERPRETER and PSX_CPU_CACHED_INTERPRETER (the "cached" mode is a per-instruction
    decode cache, not a block recompiler, and it does not have its own fetch loop). A hook placed
    in only one of the two dispatch functions would work in one mode and silently do nothing in
    the other.

    FAILURE IS ALWAYS A NORMAL BOOT
    -------------------------------
    Every unhappy path — no disc, audio disc, missing SYSTEM.CNF, unparsable BOOT= line, missing
    or malformed executable — returns 0 and lets the BIOS carry on into the shell. A fast boot
    that cannot find the game must never leave the machine wedged; the animation is a far better
    outcome than a black screen.
*/

#ifndef PSX_FASTBOOT_H
#define PSX_FASTBOOT_H

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the BIOS transfers control to the shell — as a PHYSICAL address, because the entry can
   be reached through either the cached (0x80030000) or uncached (0xA0030000) mirror. */
#define PSX_FASTBOOT_SHELL_PHYS 0x00030000u

/* User setting. Off leaves the BIOS entirely alone. */
void psx_fastboot_set_enabled(int enabled);
int  psx_fastboot_enabled(void);

/*
    Re-arms for the next boot. Call on reset / disc change: without it a second boot in the same
    session would not fast-boot, because the hook disarms itself after it fires.
*/
void psx_fastboot_rearm(void);

/*
    Called from psx_cpu_cycle() when PC reaches the shell entry. Returns non-zero if it took over
    (CPU state now points at the game), zero to let the BIOS proceed as usual.
*/
int psx_fastboot_try(psx_cpu_t* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FASTBOOT_H */
