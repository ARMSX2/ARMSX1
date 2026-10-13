# Tony Hawk USA memory research — 2026-10-14

`Tony Hawk USA - experimental.cht` contains alternative RAM-data cheats, not
translations of the removed ViperByte game-instruction patch sequences. Local
listing, labels and research notes are CC BY-SA 4.0. Game/BIOS/RAM dumps and
screenshots are user-owned local test artifacts, not repository or APK assets.
This is not a novelty claim: a functional address/value independently observed
here may also appear in another person's existing code list. Published cheats
were viewed earlier in this conversation; this was not a clean-room exercise.
No claim is made that the game's program code has been independently authored.

## What was actually derived

The opt-in `TonyHawkMemoryResearchTest` boots the user's CHD after checking its
disc-content serial is `SLUS-00860`. It uses a separate root, cards and states,
backs up/restores frontend settings, and does not load the normal game's cheats.
`NativeApp.getSwanMemorySnapshot` copies RAM on the emulation thread; it exposes
no mutable core pointer or arbitrary memory-write API. Candidate analysis uses
`tools/cheat_memory_research.py`, not external cheat data.

- Visible score observations 0, 100 and 175 narrowed the Warehouse score to RAM
  offset `0x1F2F60`. Writing decimal 999999 as two 16-bit values visibly changed
  the HUD and awarded both score tapes. End-run statistics also showed 999999.
- A first candidate at `0x0D7868` failed to change the special bar and was rejected.
- Before/after snapshots of the game's own paused controller cheat established
  its built-in toggle. That toggle was used only to trace HUD/player-field
  behaviour in the user ROM, not as the replacement code. Game inspection showed
  full-mode timer fields at actor offsets `0x250` and `0x254`, with a refresh value
  of 750. Freezing these fields made the bar full with built-in flag `0x0D172C`
  still zero. They are RAM fields, not instruction modifications. Disabling
  allowed natural decay (750 to 152 after approximately ten gameplay seconds).
- Player 1's observed pointer is held at `0x0D17C8`. The Warehouse allocation
  was `0x801F2638`; score offset is actor+`0x928`. A second freshly allocated
  Warehouse run reproduced the score and full bar together.
- School allocated the same player layout at `0x801EE6CC`, not the Warehouse
  address. Separate profiles are used; each individual write is guarded by BOTH
  halves of the observed player pointer. Unknown allocations and no-player
  menus are skipped. No unguarded writes are permitted in this experimental file.
  The combined School profile was then enabled: the visible score became 999999,
  both score tapes were awarded, the special bar became full, timer fields read
  750, and the built-in flag remained zero. The Warehouse-only profile had left
  School unaffected. In the no-player screen, guarded target bytes were unchanged.

## Single Session follow-up

Reproduced the user's Career-only special-meter report. Single Session mode 3
allocates Warehouse at `0x801F2694` and School at `0x801EE728`, versus Career's
`0x801F2638` and `0x801EE6CC`. The old guards safely skipped the new allocations.
Added separate full-pointer-guarded profiles to both research cheats. Field
offsets are unchanged; unknown pointers remain untouched.

Actual Thor checks with the final file showed score 999999 and full special in
Single Session Warehouse/School (timers 750; built-in flag zero). Career Warehouse
still worked. The special HUD flashes, so dark and full-yellow phases were
inspected alongside timer values. Five profile regression tests and the opt-in
998.224-second device session passed, including VM shutdown, inactive snapshot
and settings restoration. School unlock was earned/saved only in the sandbox's
own card. Updated only the two research groups in Thor's private list; names,
selections and Libretro groups are preserved. Backups remain outside the repo.
No APK rebuild or bundled-database change is needed for this data-only fix.

## Limits

Only this user-supplied USA ROM, Tony Hawk, and the Warehouse/School allocations
in Career and Single Session are in scope. Other levels, Free Skate, skaters,
multiplayer, revisions, regional
games and long play sessions are not validated. A matching serial is not proof
for every pressing. Never advertise these as universal cheats. New selections
must remain off until the user opts in. Score changes persist until the run
resets; special timers can drain after disabling. This is NOT an undo-code system.

The experimental file is deliberately outside `tools/cheat-supplements`, so the
standard bundled database continues using its existing licensed upstream entries
without automatically distributing these limited research candidates. It can
be imported into a private test game's cheat list. No whole author collection
has been copied, and no public-release/licence audit is cleared by this work.
