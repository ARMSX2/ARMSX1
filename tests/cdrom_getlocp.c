/*
    Host gate for what the CD-ROM drive REPORTS about itself: CdlGetlocP's answer
    (psx/dev/cdrom/impl.c) and the response FIFO it comes back through
    (psx/dev/cdrom/cdrom.c).

    What this is defending.

    CdlGetlocP is the one command a game can sit in a loop on. It answers "where
    is the head", and a game that has just asked the drive to go somewhere polls
    it until the answer says the drive got there. If the answer never changes,
    the loop never ends — and the failure does not look like a CD bug from the
    outside. The emulator runs at full speed, the frame the game was drawing
    keeps being drawn, and the only symptom is that nothing responds, because
    the game's main loop never reaches its input code. Spyro the Dragon hangs
    exactly that way at its memory-card screen.

    Measured against the code this gate was written for, driving the real MMIO
    interface against a synthetic disc:

        Setloc 00:20:00 (LBA 1500) + SeekP, then CdlGetlocP
          -> absolute 00:19:50   (LBA 1475 - the 25-sector seek undershoot)
          -> absolute 00:19:50   60 emulated frames later
          -> absolute 00:19:50   600 emulated frames later

    The undershoot was subtracted inside CdlGetlocP itself, so it was not a
    landing error that the drive then corrected, it was a permanent offset. No
    poll of that drive can ever see it arrive.

    So the two properties worth more than the rest of this file:

      * The reported position MOVES. Parked with the motor on, it has to keep
        changing, because a real disc keeps spinning under a head that keeps
        reading sub-Q.
      * It moves to somewhere USEFUL. A seek that lands short has to close the
        gap, and a position that is merely parked must not wander off the sector
        the transport is sitting on.

    Both are pure rules over the device, so this needs no BIOS, no CPU and no
    real game disc - just the 2352-byte-sector image built below.

    The rest is encoding. Every field of the response is BCD on hardware, and
    the values are picked so that BCD and binary disagree (seconds 12, frames
    37): a gate that only ever looks at 00:00:00 cannot tell the two apart.

    The response FIFO cases are here rather than in their own file because they
    are the other half of the same answer: a correct position is worthless if
    the bytes carrying it are the tail of an older response. That is not
    hypothetical either - before the fix, four commands whose responses were
    left partly unread filled the 32-byte buffer, after which every further
    response byte was dropped and CdlGetlocP answered with track 03h, index 55h
    from three commands back.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/dev/cdrom/cdrom.h"
#include "psx/dev/ic.h"

#define FIXTURE_SECTORS 1000
#define XA_STREAM_START 700   /* absolute LBA where the interleaved XA stream begins */
#define XA_INTERLEAVE   10    /* 8 ADPCM channels, then a Form2 video and a Form1 data */
#define FRAME_CYCLES    564480 /* one NTSC frame of CPU cycles */

static int g_failures = 0;

static psx_cdrom_t* g_cdrom;
static psx_ic_t g_ic;

static char g_bin_path[512];
static char g_cue_path[512];

static void check(int condition, const char* case_name, const char* what) {
    if (!condition) {
        printf("CDROM_GETLOCP failed case=%s check=%s\n", case_name, what);
        g_failures++;
    }
}

static void check_u32(uint32_t got, uint32_t want, const char* case_name, const char* what) {
    if (got != want) {
        printf("CDROM_GETLOCP failed case=%s check=%s got=%u want=%u\n",
               case_name, what, got, want);
        g_failures++;
    }
}

/* ---------------------------------------------------------------- fixture */

/* One MODE2/2352 track. Sector 16 of the track carries the ISO primary volume
   descriptor, because psx_cdrom_open() refuses anything it cannot identify as a
   licensed PlayStation disc. */
static int build_fixture(const char* dir) {
    uint8_t* sector;
    FILE* f;

    snprintf(g_bin_path, sizeof(g_bin_path), "%s/cdrom_getlocp_fixture.bin", dir);
    snprintf(g_cue_path, sizeof(g_cue_path), "%s/cdrom_getlocp_fixture.cue", dir);

    sector = malloc(2352);

    if (!sector)
        return 0;

    f = fopen(g_bin_path, "wb");

    if (!f) {
        free(sector);

        return 0;
    }

    for (int lba = 0; lba < FIXTURE_SECTORS; lba++) {
        memset(sector, 0, 2352);

        for (int i = 1; i < 11; i++)
            sector[i] = 0xff;

        /* From XA_STREAM_START on, an interleaved XA stream: 1 data sector then 7 Form2
           ADPCM sectors for file 1 / channel 4. Reads in every other case stay below it. */
        /* From XA_STREAM_START on, what Spyro actually streams: eight interleaved ADPCM
           channels (audio_diag caught them cycling 00..07 against a filter of 01/04), plus
           the two Form2/Form1 cases that MUST still reach the host.

             chan 0..7  Form2 audio   payload AA  - never delivered when Mode.bit6 is set,
                                                    match or no match
             k == 8     Form2 VIDEO   payload BB  - Form2 but not audio: still the host's
             k == 9     Form1 data    payload DA  - ordinary data */
        if (150 + lba >= XA_STREAM_START) {
            const int k = (150 + lba - XA_STREAM_START) % XA_INTERLEAVE;
            uint8_t mark;

            sector[0x10] = 0x01;

            if (k < 8) {
                sector[0x11] = (uint8_t)k;
                sector[0x12] = 0x64; /* realtime | form2 | audio */
                sector[0x13] = 0x01; /* stereo 37.8 kHz 4-bit */
                mark = 0xAA;
            } else if (k == 8) {
                sector[0x11] = 0x04;
                sector[0x12] = 0x22; /* form2 | video */
                sector[0x13] = 0x00;
                mark = 0xBB;
            } else {
                sector[0x11] = 0x04;
                sector[0x12] = 0x08; /* data */
                sector[0x13] = 0x00;
                mark = 0xDA;
            }

            memcpy(&sector[0x14], &sector[0x10], 4);
            memset(&sector[24], mark, 2048);
        }

        if (lba == 16) {
            sector[24] = 1;
            memcpy(&sector[25], "CD001", 5);
            memcpy(&sector[32], "PLAYSTATION                     ", 32);
        }

        if (fwrite(sector, 1, 2352, f) != 2352) {
            fclose(f);
            free(sector);

            return 0;
        }
    }

    fclose(f);
    free(sector);

    f = fopen(g_cue_path, "w");

    if (!f)
        return 0;

    fprintf(f, "FILE \"cdrom_getlocp_fixture.bin\" BINARY\n"
               "  TRACK 01 MODE2/2352\n"
               "    INDEX 01 00:00:00\n");
    fclose(f);

    return 1;
}

static void destroy_fixture(void) {
    remove(g_bin_path);
    remove(g_cue_path);
}

/* ------------------------------------------------------------ MMIO driver */

static void reg_write(uint32_t index, uint32_t addr, uint8_t value) {
    psx_cdrom_write8(g_cdrom, 0, index);
    psx_cdrom_write8(g_cdrom, addr, value);
}

static uint8_t reg_status(void) {
    return (uint8_t)psx_cdrom_read8(g_cdrom, 0);
}

/* Run the drive until it raises an interrupt. Returns the IFR, 0 on timeout. */
static int spin(void) {
    for (int i = 0; i < 4000000; i++) {
        if (g_cdrom->ifr & 0x1f)
            return g_cdrom->ifr & 0x1f;

        psx_cdrom_update(g_cdrom, 64);
    }

    return 0;
}

/* Read every byte the response FIFO offers, the way a driver that trusts the
   status register's "response available" bit does. */
static int drain(uint8_t* out, int max) {
    int n = 0;

    while ((reg_status() & 0x20) && n < max)
        out[n++] = (uint8_t)psx_cdrom_read8(g_cdrom, 1);

    return n;
}

static void ack(void) {
    reg_write(1, 3, 0x1f);
}

static int command(uint8_t cmd, const uint8_t* params, int nparams,
                   uint8_t* out, int max) {
    for (int i = 0; i < nparams; i++)
        reg_write(0, 2, params[i]);

    reg_write(0, 1, cmd);

    spin();

    int n = drain(out, max);

    ack();

    return n;
}

/* The second (INT2) response of a two-phase command. */
static int command_phase2(uint8_t* out, int max) {
    spin();

    int n = drain(out, max);

    ack();

    return n;
}

static void idle_frames(int frames) {
    for (int i = 0; i < frames; i++)
        psx_cdrom_update(g_cdrom, FRAME_CYCLES);
}

/* ------------------------------------------------------------- CdlGetlocP */

typedef struct {
    int nbytes;
    uint8_t raw[16];
    uint8_t track, index;
    uint32_t relative_lba;
    uint32_t absolute_lba;
    int bcd_ok;
} getlocp_t;

static int valid_bcd(uint8_t b) {
    return ((b & 0xf0) <= 0x90) && ((b & 0x0f) <= 9);
}

static uint32_t bcd_msf_to_lba(uint8_t m, uint8_t s, uint8_t f) {
    const uint32_t mi = (uint32_t)((m >> 4) * 10 + (m & 0xf));
    const uint32_t si = (uint32_t)((s >> 4) * 10 + (s & 0xf));
    const uint32_t fi = (uint32_t)((f >> 4) * 10 + (f & 0xf));

    return (mi * 60u + si) * 75u + fi;
}

static getlocp_t getlocp(void) {
    getlocp_t out;

    memset(&out, 0, sizeof(out));

    out.nbytes = command(CDL_GETLOCP, NULL, 0, out.raw, (int)sizeof(out.raw));

    if (out.nbytes < 8)
        return out;

    out.bcd_ok = 1;

    for (int i = 0; i < 8; i++) {
        if (!valid_bcd(out.raw[i]))
            out.bcd_ok = 0;
    }

    out.track = out.raw[0];
    out.index = out.raw[1];
    out.relative_lba = bcd_msf_to_lba(out.raw[2], out.raw[3], out.raw[4]);
    out.absolute_lba = bcd_msf_to_lba(out.raw[5], out.raw[6], out.raw[7]);

    return out;
}

static void setloc_lba(uint32_t lba) {
    /* Setloc takes absolute BCD MSF, which is the same space cdrom->lba lives
       in: LBA 150 is 00:02:00. */
    const uint32_t m = lba / 4500u;
    const uint32_t s = (lba % 4500u) / 75u;
    const uint32_t f = lba % 75u;
    uint8_t params[3];
    uint8_t resp[16];

    params[0] = (uint8_t)(((m / 10u) << 4) | (m % 10u));
    params[1] = (uint8_t)(((s / 10u) << 4) | (s % 10u));
    params[2] = (uint8_t)(((f / 10u) << 4) | (f % 10u));

    command(CDL_SETLOC, params, 3, resp, (int)sizeof(resp));
}

/* --------------------------------------------------------------- machine */

static int machine_up(const char* dir) {
    if (!build_fixture(dir)) {
        printf("CDROM_GETLOCP failed case=fixture check=build\n");
        g_failures++;

        return 0;
    }

    memset(&g_ic, 0, sizeof(g_ic));
    g_ic.mask = 0; /* no CPU wired up, so the IRQ line must stay unraised */

    g_cdrom = psx_cdrom_create();
    psx_cdrom_init(g_cdrom, &g_ic);

    if (!psx_cdrom_open(g_cdrom, g_cue_path)) {
        printf("CDROM_GETLOCP failed case=fixture check=open\n");
        g_failures++;

        return 0;
    }

    reg_write(1, 2, 0x1f); /* IER: let every interrupt through */

    return 1;
}

static void machine_down(void) {
    if (g_cdrom)
        psx_cdrom_destroy(g_cdrom);

    g_cdrom = NULL;

    destroy_fixture();
}

/* ----------------------------------------------------------------- cases */

/* Field mapping and BCD, at a position where BCD and binary disagree. */
static void case_encoding(void) {
    const char* name = "encoding";
    const uint32_t target = 1087; /* 00:14:37 absolute, 00:12:37 into track 1 */
    uint8_t resp[16];

    setloc_lba(target);
    command(CDL_SEEKL, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    getlocp_t p = getlocp();

    check(p.nbytes == 8, name, "response is 8 bytes");

    if (p.nbytes != 8)
        return;

    check(p.bcd_ok, name, "every field is valid BCD");
    check_u32(p.track, 0x01, name, "track is BCD 01");
    check_u32(p.index, 0x01, name, "index is 01 outside a pregap");

    /* The values that separate BCD from binary: 14 -> 0x14 not 0x0e,
       37 -> 0x37 not 0x25. A binary encoder passes the LBA checks below and
       fails these. */
    check_u32(p.raw[6], 0x14, name, "absolute seconds are BCD 14h");
    check_u32(p.raw[7], 0x37, name, "absolute frames are BCD 37h");
    check_u32(p.raw[3], 0x12, name, "relative seconds are BCD 12h");

    check_u32(p.absolute_lba, target, name, "absolute MSF is the seek target");
    check_u32(p.relative_lba, target - 150u, name,
              "relative MSF counts from the start of track 1");
}

/*
    THE hang case. A coarse seek lands short of its target; the reported
    position has to close that gap on its own.

    Before the fix this returned target-25 forever, which is a poll loop with no
    exit. The bound is generous on purpose - what is being gated is that it
    arrives at all, not how fast.
*/
static void case_seekp_reaches_target(void) {
    const char* name = "seekp_reaches_target";
    const uint32_t target = 900; /* 00:12:00 absolute */
    uint8_t resp[16];

    setloc_lba(target);
    command(CDL_SEEKP, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    getlocp_t landed = getlocp();

    check(landed.nbytes == 8, name, "response is 8 bytes");

    if (landed.nbytes != 8)
        return;

    check(landed.absolute_lba < target, name,
          "the coarse seek lands short of its target");
    check(landed.absolute_lba + 64u > target, name,
          "and lands within a plausible distance of it");

    /* Poll it the way a game does: once a frame, for two emulated seconds. */
    uint32_t furthest = landed.absolute_lba;
    int arrived = 0;

    for (int frame = 0; frame < 120 && !arrived; frame++) {
        idle_frames(1);

        getlocp_t p = getlocp();

        if (p.nbytes != 8)
            break;

        if (p.absolute_lba > furthest)
            furthest = p.absolute_lba;

        if (p.absolute_lba >= target)
            arrived = 1;
    }

    check(arrived, name, "the head reaches the seek target within 2 seconds");

    if (!arrived) {
        printf("CDROM_GETLOCP   (furthest reported LBA %u, target %u)\n",
               furthest, target);
    }
}

/*
    Parked with the motor on: the position has to keep changing, and has to stay
    near the sector the transport is parked on rather than running away down the
    disc.
*/
static void case_parked_position_moves(void) {
    const char* name = "parked_position_moves";
    const uint32_t target = 600; /* 00:08:00 absolute */
    uint8_t resp[16];

    setloc_lba(target);
    command(CDL_SEEKL, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    const uint32_t transport_lba = g_cdrom->lba;
    uint32_t seen_low = 0xffffffffu;
    uint32_t seen_high = 0;
    int distinct = 0;
    uint32_t previous = 0xffffffffu;

    for (int frame = 0; frame < 240; frame++) {
        idle_frames(1);

        getlocp_t p = getlocp();

        if (p.nbytes != 8) {
            check(0, name, "response is 8 bytes");

            return;
        }

        if (p.absolute_lba != previous) {
            distinct++;
            previous = p.absolute_lba;
        }

        if (p.absolute_lba < seen_low)  seen_low = p.absolute_lba;
        if (p.absolute_lba > seen_high) seen_high = p.absolute_lba;
    }

    check(distinct > 1, name, "the reported position changes while parked");
    check(seen_high - seen_low <= 32u, name,
          "and stays inside a hold window rather than running away");
    check(seen_low + 32u >= transport_lba && seen_high <= transport_lba + 32u, name,
          "the hold window is centred on the parked sector");

    /* The transport position is what a resumed read starts from, so it must NOT
       have drifted with the head. */
    check_u32(g_cdrom->lba, transport_lba, name,
              "the transport position is untouched by the head moving");
}

/*
    A response REPLACES the FIFO. A driver that reads one byte of an eight-byte
    answer - which is what an INT1 sector handler that only wants the status
    byte does - must not push the next command's answer behind the leftovers.
*/
static void case_response_fifo_is_replaced(void) {
    const char* name = "response_fifo_is_replaced";
    uint8_t resp[40];

    /* Six commands in a row, reading exactly one byte of each. */
    for (int round = 0; round < 6; round++) {
        reg_write(0, 1, CDL_GETLOCP);
        spin();

        const uint8_t first = (uint8_t)psx_cdrom_read8(g_cdrom, 1);

        ack();

        check_u32(first, 0x01, name, "the first byte is always this disc's track 01");
    }

    /* And a full read after all that has to be a clean 8-byte answer. */
    const int n = command(CDL_GETLOCP, NULL, 0, resp, (int)sizeof(resp));

    check_u32((uint32_t)n, 8u, name, "a full read gets exactly 8 bytes back");
}

/*
    The same thing from the direction a game actually produces it: a long read
    whose per-sector INT1 status byte is never collected.
*/
static void case_undrained_int1_stream(void) {
    const char* name = "undrained_int1_stream";
    uint8_t resp[40];

    setloc_lba(300);
    command(CDL_READN, NULL, 0, resp, (int)sizeof(resp));

    for (int sector = 0; sector < 40; sector++) {
        spin();
        ack(); /* acknowledged, never read */
    }

    check(!queue_is_full(g_cdrom->response), name,
          "40 uncollected sector statuses do not fill the response FIFO");

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    const int n = command(CDL_GETLOCP, NULL, 0, resp, (int)sizeof(resp));

    check_u32((uint32_t)n, 8u, name, "CdlGetlocP still answers with exactly 8 bytes");

    if (n == 8)
        check_u32(resp[0], 0x01, name, "and the first byte is the track, not a leftover");
}

/*
    A query must not restart a running read's sector clock.

    A command and a running read shared cdrom->delay, so every command write threw away
    whatever the read had counted down and rescheduled it from scratch. The drive's sector
    rate therefore collapsed to the rate the game asked it questions — and a game streaming XA
    asks once a frame, which is the sequence the device log caught Spyro in:

        CdlSetfilter 01 04 / CdlSetloc 25 14 48 / CdlReads / CdlGetlocp x512 ...

    Measured replaying exactly that, one poll per frame for two emulated seconds:

        single speed  0 sectors delivered   (the read delay is 17.3 ms against a
                                             16.7 ms poll period, so no sector EVER
                                             completes - the stream is simply dead)
        double speed  120 sectors           (60/s, against the 150/s the drive owes)

    Single speed is what this gate runs, because zero is unambiguous: no threshold argument,
    no flakiness, the stream either moves or it does not.

    It also pins the thing that was misdiagnosed from a trace line: CdlGetlocP answers INT3
    with eight bytes on EVERY poll, including while a read is running underneath it. The INT1
    in that trace was the read's own sector interrupt, not the command's answer.
*/
static void case_query_does_not_starve_read(void) {
    const char* name = "query_does_not_starve_read";
    const int frames = 60; /* 1.0 s; a 1x drive owes ~75 sectors */
    uint8_t resp[40];
    uint8_t mode[1] = { 0x60 }; /* single speed, XA-ADPCM, whole sector */
    int sectors = 0;
    int answered = 0;
    int malformed = 0;

    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
    setloc_lba(300);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    for (int frame = 0; frame < frames; frame++) {
        /* A frame of emulated time, servicing sector interrupts as a CD ISR would. */
        for (int slice = 0; slice < 64; slice++) {
            psx_cdrom_update(g_cdrom, FRAME_CYCLES / 64);

            if (g_cdrom->ifr & 0x1f) {
                if ((g_cdrom->ifr & 0x1f) == 1)
                    sectors++;

                drain(resp, (int)sizeof(resp));
                ack();
            }
        }

        /* The poll. */
        reg_write(0, 1, CDL_GETLOCP);

        const int raised = spin();
        const int n = drain(resp, (int)sizeof(resp));

        ack();

        if (raised == 3 && n == 8)
            answered++;
        else
            malformed++;
    }

    check(answered == frames, name,
          "CdlGetlocP answers INT3 with 8 bytes on every poll during a read");

    if (malformed) {
        printf("CDROM_GETLOCP   (%d of %d polls were not INT3/8 bytes)\n", malformed, frames);
    }

    check(sectors > 40, name,
          "the read keeps delivering sectors while the game polls once a frame");

    if (sectors <= 40) {
        printf("CDROM_GETLOCP   (%d sectors in %d frames; a 1x drive owes ~75)\n",
               sectors, frames);
    }

    check(g_cdrom->read_ongoing, name, "and the read is still running at the end");

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));
}

/*
    Polling must not change the drive's sector rate in EITHER direction.

    The starvation case above pins the floor. This pins both ends against the drive's own
    unpolled rate, because overshoot is the more dangerous of the two failures: this file's
    CD_DELAY_ONGOING_READ comment records an earlier change that made data arrive faster than
    hardware and looped Xenogears' first FMV, and "games that stream XA audio or FMV expect
    data to arrive at the drive's real rate" is the whole reason CD read speedup is documented
    as a compatibility trade.

    The reference is measured, not asserted: whatever this drive does with nobody polling it is
    the number a polled run has to match. That makes the gate independent of the read delay
    constants, of the 1x/2x mode, and of the host read-speedup divisor - change any of them and
    both halves move together, which is exactly the property being defended.

    Rates are per EMULATED CYCLE, not per loop iteration. Servicing a command advances emulated
    time too, so counting iterations credits a polled run with more disc time than an unpolled
    one and invents an ~9% overshoot that is not in the emulator. That artifact cost a false
    alarm during this investigation; measuring the denominator properly is the fix.
*/
static int measure_sector_rate(int poll, double* seconds) {
    uint8_t resp[40];
    uint8_t mode[1] = { 0x60 }; /* single speed, XA-ADPCM, whole sector */
    long long cycles = 0;
    int sectors = 0;

    /* A fresh transport each time so neither run inherits the other's position. */
    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
    setloc_lba(200);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    for (int frame = 0; frame < 90; frame++) {
        for (int slice = 0; slice < 64; slice++) {
            const int step = FRAME_CYCLES / 64;

            psx_cdrom_update(g_cdrom, step);
            cycles += step;

            if (g_cdrom->ifr & 0x1f) {
                if ((g_cdrom->ifr & 0x1f) == 1)
                    sectors++;

                drain(resp, (int)sizeof(resp));
                ack();
            }
        }

        if (!poll)
            continue;

        reg_write(0, 1, CDL_GETLOCP);

        /* Spin by hand rather than through spin(), so the command's own emulated time lands
           in the denominator. */
        for (int i = 0; i < 4000000 && !(g_cdrom->ifr & 0x1f); i++) {
            psx_cdrom_update(g_cdrom, 64);
            cycles += 64;
        }

        drain(resp, (int)sizeof(resp));
        ack();
    }

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    *seconds = (double)cycles / 33868800.0;

    return sectors;
}

static void case_polling_does_not_change_sector_rate(void) {
    const char* name = "polling_does_not_change_sector_rate";
    double unpolled_s = 0.0, polled_s = 0.0;

    const int unpolled = measure_sector_rate(0, &unpolled_s);
    const int polled = measure_sector_rate(1, &polled_s);

    if (unpolled_s <= 0.0 || polled_s <= 0.0 || unpolled <= 0) {
        check(0, name, "the reference run delivered sectors at all");

        return;
    }

    const double reference = (double)unpolled / unpolled_s;
    const double observed = (double)polled / polled_s;
    const double ratio = observed / reference;

    /* Wide enough that sector-boundary phase and the 8820-cycle stepping cannot trip it,
       narrow enough that the failures it exists for cannot hide: the pre-fix starvation was
       a ratio of 0.00, and a give-back that forgets to charge a whole command service is
       ~1.25 at this poll rate. */
    check(ratio > 0.90 && ratio < 1.10, name,
          "polled sector rate stays within 10% of the unpolled rate");

    if (!(ratio > 0.90 && ratio < 1.10)) {
        printf("CDROM_GETLOCP   (unpolled %.1f/s over %.3fs, polled %.1f/s over %.3fs, "
               "ratio %.3f)\n", reference, unpolled_s, observed, polled_s, ratio);
    }
}

/*
    The parked read must be charged for a SLOW host acknowledgement.

    This is the case a rate measurement cannot see, and it is the one that makes an overshoot
    unbounded. cdrom_write_cmd() sets CD_DELAY_FR for the command, but psx_cdrom_update() then
    refuses to execute it while an interrupt is unacknowledged ("Hold IRQ until IFR is
    acknowledged"), so the command's real cost is CD_DELAY_FR plus however long the host takes
    to acknowledge — and a game that services CD interrupts from its main loop can leave one
    sitting for most of a frame.

    A give-back that subtracts a fixed CD_DELAY_FR therefore hands the read back the entire
    acknowledgement latency as free time. Measured against that version, with the host
    dawdling 8 ms before acknowledging:

        read had 225792 left, host dawdled 270952 before acking
        give-back should have been -95561 (overdue); it was 175391
        -> the sector arrives 8.00 ms early, exactly the dawdle

    At tens of polls a second that compounds without limit, which is the overshoot direction
    this file's CD_DELAY_ONGOING_READ comment records as having looped Xenogears' first FMV.
    A disc does not bank time while the host is busy.
*/
static void case_slow_ack_is_charged_to_the_read(void) {
    const char* name = "slow_ack_is_charged_to_the_read";
    uint8_t resp[40];
    uint8_t mode[1] = { 0x60 };
    const int dawdle = 8 * 33869; /* 8 ms, about half a frame */

    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
    setloc_lba(200);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    /* Let a sector land and deliberately leave it unacknowledged. */
    spin();

    const int remaining = g_cdrom->delay;

    check(remaining > dawdle, name, "the parked read has more left than the host will dawdle");

    /* Poll while that interrupt is still outstanding. */
    reg_write(0, 1, CDL_GETLOCP);

    for (int c = 0; c < dawdle; c += 64)
        psx_cdrom_update(g_cdrom, 64);

    drain(resp, (int)sizeof(resp));
    ack();

    spin();

    const int resumed = g_cdrom->delay;

    drain(resp, (int)sizeof(resp));
    ack();

    /* The read owes at most what it had left minus the time that actually passed. Anything
       above that is disc time the drive was given for free. */
    const int ceiling = remaining - dawdle;

    check(resumed <= ceiling, name,
          "the give-back does not include the time the host spent not acknowledging");

    if (resumed > ceiling) {
        printf("CDROM_GETLOCP   (resumed %d, ceiling %d; sector arrives %d cycles early)\n",
               resumed, ceiling, resumed - ceiling);
    }

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));
}

/*
    And the other half of the same promise: a command that CAN touch the transport keeps the
    pacing it always had.

    The give-back is deliberately restricted to queries so that every game working today is
    bit-identical. Setmode mid-stream is the case that matters — it is how a game reconfigures
    a running XA stream, this file already carries a Silent Hill / Metal Gear Solid regression
    note about it, and it must still land on CD_DELAY_ONGOING_READ exactly as before.
*/
static void case_non_query_keeps_old_pacing(void) {
    const char* name = "non_query_keeps_old_pacing";
    uint8_t resp[40];
    uint8_t mode[1] = { 0x60 };

    /* CD_DELAY_ONGOING_READ expands to cdrom_get_read_delay(cdrom), so the macro needs a
       local of that name to read the mode's speed bit and the host speedup out of. */
    psx_cdrom_t* cdrom = g_cdrom;

    setloc_lba(200);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    /* Let the read settle into its steady state. */
    for (int sector = 0; sector < 4; sector++) {
        spin();
        drain(resp, (int)sizeof(resp));
        ack();
    }

    /* A query parks the read on its own clock... */
    command(CDL_GETLOCP, NULL, 0, resp, (int)sizeof(resp));

    check(g_cdrom->delay > 0 && g_cdrom->delay < CD_DELAY_ONGOING_READ, name,
          "a query hands the read back less than a full restart");

    /* ...a Setmode does not. */
    mode[0] = 0x60;
    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));

    check_u32((uint32_t)g_cdrom->delay, (uint32_t)CD_DELAY_ONGOING_READ, name,
              "Setmode during a read still restarts the read delay, as it always did");

    check_u32((uint32_t)g_cdrom->read_delay_pending, 0u, name,
              "and leaves no stash behind for a later read to inherit");

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));
}


/*
    XA-ADPCM sectors belong to the decoder, not to the host — on EVERY channel.

    With Mode.bit6 set, hardware routes Form2 audio sectors away from the host. The filter
    chooses which channel reaches the SPU and which is discarded; psx-spx on Setfilter: it
    "selects which of the interleaved audio channels shall be played (the other channels are
    ignored)". Ignored, not handed over as data.

    Conditioning the suppression on a filter MATCH is the trap, and it is why a first fix
    looked right and changed nothing. Spyro streams eight interleaved ADPCM channels; suppress
    only the matching one and 7 of every 8 sectors still arrive as data. Measured on device:
    INT1 at 131/s against a 150/s drive — exactly 7/8 — with audio_diag showing every sector
    audio=1 data=0 form2=1, channels cycling 00..07 against filter 01/04.

    The two negatives matter as much as the positive, so the fixture carries them: a Form2
    VIDEO sector and an ordinary Form1 data sector must still be delivered. Suppressing on
    "Form2" alone, or on "Mode.bit6 alone", would eat those and starve a game of real data.

    Payload marks are the assertion, not counts: AA audio, BB Form2 video, DA Form1 data.
*/
static void case_adpcm_sectors_are_not_delivered_as_data(void) {
    const char* name = "adpcm_sectors_are_not_delivered_as_data";
    uint8_t resp[40];
    uint8_t mode[1] = { 0xc8 };        /* 2x + XA-ADPCM + XA filter */
    uint8_t filter[2] = { 0x01, 0x04 };
    int delivered = 0, audio = 0, video = 0, data = 0;

    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
    command(CDL_SETFILTER, filter, 2, resp, (int)sizeof(resp));
    setloc_lba(XA_STREAM_START);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    for (int i = 0; i < 12; i++) {
        if (spin() != 1)
            break;

        delivered++;

        switch (g_cdrom->data->buf[g_cdrom->data->read_index]) {
            case 0xAA: audio++; break;
            case 0xBB: video++; break;
            case 0xDA: data++;  break;
        }

        drain(resp, (int)sizeof(resp));
        ack();
    }

    check(delivered > 0, name, "the read delivers something at all");
    check_u32((uint32_t)audio, 0u, name,
              "no ADPCM sector reaches the host, on ANY channel, matching or not");
    check(video > 0, name, "a Form2 VIDEO sector is still delivered - Form2 is not enough");
    check(data > 0, name, "an ordinary Form1 data sector is still delivered");

    if (audio) {
        printf("CDROM_GETLOCP   (%d of %d delivered sectors were ADPCM)\n", audio, delivered);
    }

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    /*
        And the other direction: with XA-ADPCM DISABLED the controller decodes nothing, so the
        very same sectors are the host's to read. A game that wants the raw ADPCM bytes turns
        Mode.bit6 off to get them, and suppressing unconditionally would hand it nothing.
    */
    mode[0] = 0x88; /* 2x, XA filter on, XA-ADPCM OFF */
    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
    setloc_lba(XA_STREAM_START);
    command(CDL_READS, NULL, 0, resp, (int)sizeof(resp));

    int raw_audio = 0;

    for (int i = 0; i < 12; i++) {
        if (spin() != 1)
            break;

        if (g_cdrom->data->buf[g_cdrom->data->read_index] == 0xAA)
            raw_audio++;

        drain(resp, (int)sizeof(resp));
        ack();
    }

    check(raw_audio > 0, name,
          "with XA-ADPCM off, ADPCM sectors ARE delivered as ordinary data");

    command(CDL_PAUSE, NULL, 0, resp, (int)sizeof(resp));
    command_phase2(resp, (int)sizeof(resp));

    mode[0] = 0x60;
    command(CDL_SETMODE, mode, 1, resp, (int)sizeof(resp));
}

/* The command-name table is indexed by a byte a game chooses. */
static void case_command_name_is_bounded(void) {
    const char* name = "command_name_is_bounded";

    check(psx_cdrom_command_name(0x11) != NULL, name, "a known command names itself");
    check(strcmp(psx_cdrom_command_name(0x11), "CdlGetlocp") == 0, name,
          "11h is CdlGetlocp");
    check(psx_cdrom_command_name(0xff) != NULL, name,
          "an invalid command byte does not index off the end of the table");
}

int main(int argc, const char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";

    if (!machine_up(dir)) {
        machine_down();

        return 1;
    }

    case_encoding();
    case_seekp_reaches_target();
    case_parked_position_moves();
    case_response_fifo_is_replaced();
    case_undrained_int1_stream();
    case_query_does_not_starve_read();
    case_polling_does_not_change_sector_rate();
    case_slow_ack_is_charged_to_the_read();
    case_non_query_keeps_old_pacing();
    case_adpcm_sectors_are_not_delivered_as_data();
    case_command_name_is_bounded();

    machine_down();

    if (g_failures) {
        printf("CDROM_GETLOCP FAILED (%d)\n", g_failures);

        return 1;
    }

    printf("CDROM_GETLOCP OK\n");

    return 0;
}
