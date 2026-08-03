#ifndef DISCID_H
#define DISCID_H

#include <stddef.h>

#include "dev/cdrom/disc.h"

/*
    Disc serial identification, off any container psx_disc_open() can open.

    Every PS1 game disc carries a SYSTEM.CNF in the ISO9660 root directory naming its boot
    executable — "BOOT = cdrom:\SLUS_005.94;1" — and that name IS the disc serial. The launcher
    reads it in Kotlin (com.armsx2.core.Ps1DiscId) for the containers Java can seek around in
    directly (.bin/.cue/.iso/.pbp), which is most of them.

    A .chd is the one it cannot: CHD v5 Huffman-compresses its own hunk map, so there is no way
    to reach the filesystem without decompressing, and a hand-rolled decoder that is a bit wrong
    returns a plausible WRONG serial rather than failing. The serial keys the cover, the
    per-game settings and the RetroAchievements identity, so wrong is worse than absent.

    This walks the disc through the SAME vtable the emulated drive reads through, so whatever
    libchdr can boot, this can identify — no second decoder to disagree with the first.

    Geometry is detected rather than assumed: the caller may hand us a CHD (2352-byte sectors,
    ISO sector 0 at the track-1 LBA, usually 150), a raw MODE2 rip (2352, ISO sector 0 at LBA 0),
    a MODE1 rip (user data 8 bytes earlier) or a plain 2048-byte ISO. See psx_discid_from_disc.

    Serials come back in psx-covers' filename form — "SLUS_005.94" -> "SLUS-00594" — byte-identical
    to what Ps1DiscId produces in Kotlin, because the two are alternative routes to the same
    identity and a disagreement would silently split a game's settings in half.
*/

/* Longest serial is "AAAA-DDDDD" plus a terminator; round up for callers' comfort. */
#define PSX_DISCID_MAX 16

/*
    Identifies an ALREADY-OPEN disc. Writes a normalised serial into `out` and returns 1; returns
    0 and leaves `out` an empty string when the disc carries no readable, serial-shaped BOOT line
    (an audio CD, a homebrew booting PSX.EXE, a damaged image).
*/
int psx_discid_from_disc(psx_disc_t* disc, char* out, size_t out_size);

/*
    Opens `path`, identifies it, closes it. Same return contract as psx_discid_from_disc.
    Blocking IO — callers on Android dispatch it off the UI thread.
*/
int psx_discid_from_path(const char* path, char* out, size_t out_size);

#endif
