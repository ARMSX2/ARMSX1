/*
    ARMSX — profile-guided optimisation (PGO) runtime seam.

    See frontend/pgo.c for the whole story. The one thing that matters HERE:

    ★ These declarations are UNCONDITIONAL on purpose. There is no #ifdef and there are no
      inline stubs, and that is a correctness requirement, not a style choice.

      A PGO=use build compiles against a profile recorded by a PGO=generate build. LLVM keys
      every function's profile on a hash of its CFG, so any translation unit whose CONTROL FLOW
      differs between the two builds gets "function control flow change detected (hash mismatch)"
      and its profile is thrown away. If this header had declared no-op stubs under #ifdef, every
      caller of armsx_pgo_flush() would have a different CFG in the two modes and would silently
      lose its profile — in frontend/main.cpp, which is most of the emulator.

      So callers compile identically in all three modes; only frontend/pgo.c changes, and that
      one object is excluded from PGO by the Makefile precisely because it does.
*/

#ifndef ARMSX_PGO_H
#define ARMSX_PGO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors the Makefile's PGO= switch. */
#define ARMSX_PGO_MODE_OFF      0
#define ARMSX_PGO_MODE_GENERATE 1
#define ARMSX_PGO_MODE_USE      2

/* Which of the three the binary was built as. */
int armsx_pgo_mode(void);

/* Provenance, for the staleness guard. All return a stable string, never NULL.

   build_commit  git HEAD when this binary was compiled.
   source_stamp  hash over psx/ + frontend/ sources at compile time. Catches uncommitted edits,
                 which a commit hash cannot.
   profile_*     for a PGO=use build: what the profile it was compiled against was recorded from.
                 Empty in the other two modes. */
const char* armsx_pgo_build_commit(void);
const char* armsx_pgo_source_stamp(void);
const char* armsx_pgo_profile_commit(void);
const char* armsx_pgo_profile_stamp(void);

/* Non-zero when a PGO=use build's profile was recorded from exactly this source tree. Always
   non-zero in the other two modes (nothing to be stale against). */
int armsx_pgo_profile_is_fresh(void);

/* Where the instrumented build writes its counters. "" when not an instrumented build, or
   before armsx_pgo_set_output_dir() has run. */
const char* armsx_pgo_output_path(void);

/* Point the profile runtime at an app-writable directory; creates <dir>/pgo/ and drops a
   provenance sidecar there. Idempotent. No-op unless this is an instrumented build.

   Called from psxe_cfg_set_pref_path() (frontend/config.c), which is the one place both Android
   entry paths already funnel the app's private files dir through. */
void armsx_pgo_set_output_dir(const char* dir);

/* Write the counters out and reset them, so the next flush contributes only its own delta and
   the runtime's %m merge accumulates instead of double-counting. Returns non-zero on success.
   No-op returning 0 unless this is an instrumented build.

   [reason] is only for the log. */
int armsx_pgo_flush(const char* reason);

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_PGO_H */
