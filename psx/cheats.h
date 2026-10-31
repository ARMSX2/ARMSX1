/*
    ARMSX PS1 core — cheats and patches.

    ============================================================================
    WHY GAMESHARK CODES AND NOT PNACH
    ============================================================================

    The sibling PS2 project applies patches as PNACH: `patch=1,EE,<addr>,<width>,<value>`,
    keyed by <SERIAL>_<CRC>.pnach, with an EE/IOP split and a 32 MB address space. None of
    that shape exists here. The PS1 has ONE processor, 2 MB of main RAM and no ELF CRC that
    anybody publishes patches against, so bending PNACH onto it would mean inventing a
    dialect that no existing file is written in and that no other tool can read.

    Every PS1 cheat that has ever been published — GameShark, Action Replay, Xplorer, the
    code lists that ship with every emulator on every platform — is a pair of hex words:

        800AB3C4 0063        type 0x80, address 0x0AB3C4, value 0x0063

    That is the interchange format for this machine, so that is what this reads. A user can
    paste a code straight off a twenty-year-old FAQ and it works. Nothing is invented.

    The container is the equally long-standing `.cht` layout: `[Name]` groups a run of code
    lines, `#` starts a comment. WHICH groups are switched on is deliberately NOT stored in
    the file — see "enablement" below.

    ============================================================================
    CODE TYPES
    ============================================================================

    Applied once per emulated frame, in file order, per armed cheat:

        30aaaaaa 00vv   write 8  — RAM[aaaaaa] = vv
        80aaaaaa vvvv   write 16 — RAM[aaaaaa] = vvvv
        5000ccii vvvv   slide    — repeat the NEXT write `cc` times, stepping the address
                                   by `ii` bytes and the value by `vvvv` each time
        D0aaaaaa vvvv   execute the next line only if RAM16[aaaaaa] == vvvv
        D1aaaaaa vvvv   ... only if !=
        D2aaaaaa vvvv   ... only if <
        D3aaaaaa vvvv   ... only if >
        E0aaaaaa 00vv   execute the next line only if RAM8[aaaaaa] == vv
        E1aaaaaa 00vv   ... only if !=
        E2aaaaaa 00vv   ... only if <
        E3aaaaaa 00vv   ... only if >

    Any other type is REFUSED AT PARSE TIME with one log line naming the type, rather than
    being silently dropped or guessed at. A cheat that contains a type this build does not
    implement still loads and is still listed — its unsupported lines are simply inert, and
    psx_cheats_unsupported_types() reports the set so the UI can say so out loud instead of
    letting a user wonder why a code "does nothing".

    A conditional governs exactly ONE following line, and both the conditional and the slide
    state are reset at every cheat boundary — so concatenating two cheats into one program
    can never let the tail of one reach into the head of the next.

    ============================================================================
    ENABLEMENT LIVES IN SETTINGS, NOT IN THE FILE
    ============================================================================

    A `.cht` file is a CATALOGUE. Which entries are armed is a per-game setting
    (`[cheats] enabled_codes` in settings.toml, resolved through the app's per-game layer),
    for one reason: the PS2 project stored enablement by NAME in a layer that could end up
    GLOBAL, so "Infinite Health" armed on one game silently armed the same-named group in
    every other game the user owned. It shipped a one-time repair function for exactly that.

    Here the armed list is per-game and the names it holds are only ever matched against the
    ONE file loaded for the game that is running, so a stray global list can arm nothing.

    ============================================================================
    THREADING
    ============================================================================

    The CATALOGUE (names, descriptions, code text) belongs to whoever is calling — the UI
    thread through JNI, and the emulation thread once at boot. It is guarded by a spin lock
    held only across pointer swaps, never across a file read.

    The PROGRAM — the flat word list the emulator walks — is published by an atomic handoff:
    the arming thread builds a fresh immutable program and parks it, and psx_cheats_apply()
    (emulation thread) adopts it at a frame boundary and frees the one it replaces. Nothing
    the emulator is walking can be freed under it.

    COST WHEN NOTHING IS ARMED: psx_cheats_apply() is one predictable branch on a plain
    pointer plus one atomic load, per FRAME. Not per instruction — this deliberately does
    not sit anywhere near psx_cpu_cycle().
*/

#ifndef PSX_CHEATS_H
#define PSX_CHEATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct psx_t;

/* Longest cheat name / description this accepts, in bytes. Bounded so a corrupt file cannot
   turn into an allocation the size of itself. */
#define PSX_CHEAT_NAME_MAX 192
#define PSX_CHEAT_DESC_MAX 512

/* Guards against a file that is not a cheat file at all. Both are far above any real list
   (the largest community .cht files run to a few hundred entries). */
#define PSX_CHEAT_MAX_ENTRIES 4096
#define PSX_CHEAT_MAX_LINES   65536

/* ---- catalogue (host thread) ------------------------------------------------------- */

/* Parse `path` and REPLACE the catalogue. Returns the number of entries parsed, or -1 when
   the file could not be opened. An empty or comment-only file is 0, not an error.
   Passing NULL/"" clears the catalogue (and returns 0), which is what a game with no cheat
   file wants. Does NOT arm anything: call psx_cheats_arm() after. */
int psx_cheats_load_file(const char* path);

/* The file the catalogue came from, or "" — never NULL. Diagnostics only. */
const char* psx_cheats_source_path(void);

int psx_cheats_count(void);

/*
    ★ The catalogue as ONE malloc()ed string the caller frees. Never NULL ("" when empty).

    Records separated by 0x1E, fields within a record by 0x1F:

        name 0x1F line-count 0x1F unsupported(1/0) 0x1F description

    This — not the borrowing accessors below — is what a HOST reads, because it is built under
    the module's lock and hands back a copy. The accessors return pointers INTO the catalogue,
    which another thread's psx_cheats_load_file() is free to replace; that is fine on the thread
    that owns the load and a use-after-free anywhere else.
*/
char* psx_cheats_describe(void);

/* Entry `index`, or "" / 0 when out of range. OWNING THREAD ONLY — see psx_cheats_describe().
   The returned pointers are valid until the next psx_cheats_load_file()/psx_cheats_shutdown(). */
const char* psx_cheats_name(int index);
const char* psx_cheats_description(int index);
int psx_cheats_line_count(int index);
/* Non-zero when this entry contains at least one code type this build does not implement. */
int psx_cheats_has_unsupported(int index);

/* Space-separated hex list of the code types the loaded file used that are not implemented
   ("" when there are none). Never NULL. Owning thread only, as above. */
const char* psx_cheats_unsupported_types(void);

/* ---- arming ------------------------------------------------------------------------ */

/*
    Arm exactly the catalogue entries named in `names` (`count` entries), and publish the
    result for the emulation thread.

    `master` 0 disarms everything regardless of `names`; so does the hardcore inhibit (see
    below). Names are matched case-insensitively with surrounding space ignored, so a
    hand-edited settings.toml behaves the way it looks.

    Returns the number of entries actually armed. A name with no matching entry is counted
    in *missing (pass NULL if you do not care) rather than being silently ignored — a cheat
    list that no longer matches its file is the single most confusing failure this feature
    has, and the caller can say so.
*/
int psx_cheats_arm(int master, const char* const* names, int count, int* missing);

/* How many entries are currently armed. Safe from any thread. */
int psx_cheats_armed_count(void);

/*
    RetroAchievements hardcore gate.

    Non-zero forces the applied program empty no matter what is armed, and makes
    psx_cheats_arm() refuse. Deliberately a separate switch from `master` so that turning
    hardcore off restores the user's own cheat selection instead of silently wiping it.

    The front-end drives this from armsx_ach_hardcore_active() every frame, so the gate is
    re-evaluated from the authoritative source rather than being set once and trusted.
*/
void psx_cheats_set_inhibited(int inhibited);
int psx_cheats_inhibited(void);

/* ---- emulation thread -------------------------------------------------------------- */

/*
    Apply the armed program to `psx`. Call ONCE per emulated frame, at the frame boundary.

    Returns without touching memory (one branch + one atomic load) when nothing is armed and
    nothing is waiting to be published, which is the state every session that never opens the
    cheats screen stays in forever. Never logs: this is per-frame code.
*/
void psx_cheats_apply(struct psx_t* psx);

/* Drop the catalogue, the armed program and everything they own. */
void psx_cheats_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
