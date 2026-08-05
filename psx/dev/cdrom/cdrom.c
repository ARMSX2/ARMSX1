#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "cdrom.h"
#include "../../log.h"
#include "../../perf.h"

typedef void (*cdrom_cmd_func)(psx_cdrom_t* cdrom);

void cdrom_cmd_getstat(psx_cdrom_t* cdrom);
void cdrom_cmd_setloc(psx_cdrom_t* cdrom);
void cdrom_cmd_play(psx_cdrom_t* cdrom);
void cdrom_cmd_forward(psx_cdrom_t* cdrom);
void cdrom_cmd_backward(psx_cdrom_t* cdrom);
void cdrom_cmd_readn(psx_cdrom_t* cdrom);
void cdrom_cmd_motoron(psx_cdrom_t* cdrom);
void cdrom_cmd_stop(psx_cdrom_t* cdrom);
void cdrom_cmd_pause(psx_cdrom_t* cdrom);
void cdrom_cmd_init(psx_cdrom_t* cdrom);
void cdrom_cmd_mute(psx_cdrom_t* cdrom);
void cdrom_cmd_demute(psx_cdrom_t* cdrom);
void cdrom_cmd_setfilter(psx_cdrom_t* cdrom);
void cdrom_cmd_setmode(psx_cdrom_t* cdrom);
void cdrom_cmd_getparam(psx_cdrom_t* cdrom);
void cdrom_cmd_getlocl(psx_cdrom_t* cdrom);
void cdrom_cmd_getlocp(psx_cdrom_t* cdrom);
void cdrom_cmd_setsession(psx_cdrom_t* cdrom);
void cdrom_cmd_gettn(psx_cdrom_t* cdrom);
void cdrom_cmd_gettd(psx_cdrom_t* cdrom);
void cdrom_cmd_seekl(psx_cdrom_t* cdrom);
void cdrom_cmd_seekp(psx_cdrom_t* cdrom);
void cdrom_cmd_test(psx_cdrom_t* cdrom);
void cdrom_cmd_getid(psx_cdrom_t* cdrom);
void cdrom_cmd_reads(psx_cdrom_t* cdrom);
void cdrom_cmd_reset(psx_cdrom_t* cdrom);
void cdrom_cmd_getq(psx_cdrom_t* cdrom);
void cdrom_cmd_readtoc(psx_cdrom_t* cdrom);
void cdrom_cmd_videocd(psx_cdrom_t* cdrom);

cdrom_cmd_func cdrom_cmd_table[] = {
    (cdrom_cmd_func)0,
    cdrom_cmd_getstat,
    cdrom_cmd_setloc,
    cdrom_cmd_play,
    cdrom_cmd_forward,
    cdrom_cmd_backward,
    cdrom_cmd_readn,
    cdrom_cmd_motoron,
    cdrom_cmd_stop,
    cdrom_cmd_pause,
    cdrom_cmd_init,
    cdrom_cmd_mute,
    cdrom_cmd_demute,
    cdrom_cmd_setfilter,
    cdrom_cmd_setmode,
    cdrom_cmd_getparam,
    cdrom_cmd_getlocl,
    cdrom_cmd_getlocp,
    cdrom_cmd_setsession,
    cdrom_cmd_gettn,
    cdrom_cmd_gettd,
    cdrom_cmd_seekl,
    cdrom_cmd_seekp,
    (cdrom_cmd_func)0,
    (cdrom_cmd_func)0,
    cdrom_cmd_test,
    cdrom_cmd_getid,
    cdrom_cmd_reads,
    cdrom_cmd_reset,
    cdrom_cmd_getq,
    cdrom_cmd_readtoc,
    cdrom_cmd_videocd
};

static const char* cdrom_cmd_names[] = {
    "<unimplemented>",
    "CdlGetstat",
    "CdlSetloc",
    "CdlPlay",
    "CdlForward",
    "CdlBackward",
    "CdlReadn",
    "CdlMotoron",
    "CdlStop",
    "CdlPause",
    "CdlInit",
    "CdlMute",
    "CdlDemute",
    "CdlSetfilter",
    "CdlSetmode",
    "CdlGetparam",
    "CdlGetlocl",
    "CdlGetlocp",
    "CdlSetsession",
    "CdlGettn",
    "CdlGettd",
    "CdlSeekl",
    "CdlSeekp",
    "<unimplemented>",
    "<unimplemented>",
    "CdlTest",
    "CdlGetid",
    "CdlReads",
    "CdlReset",
    "CdlGetq",
    "CdlReadtoc",
    "CdlVideocd"
};

void cdrom_write_stat(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_cmd(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_null(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_parm(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_ier(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_ifr(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_req(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_vol0(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_vol1(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_vol2(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_vol3(psx_cdrom_t* cdrom, uint8_t data);
void cdrom_write_vapp(psx_cdrom_t* cdrom, uint8_t data);

static void cdrom_trace_flush(psx_cdrom_t* cdrom);

psx_cdrom_t* psx_cdrom_create(void) {
    return calloc(1, sizeof(psx_cdrom_t));
}

void psx_cdrom_init(psx_cdrom_t* cdrom, psx_ic_t* ic) {
    memset(cdrom, 0, sizeof(psx_cdrom_t));

    cdrom->io_base = PSX_CDROM_BEGIN;
    cdrom->io_size = PSX_CDROM_SIZE;

    cdrom->data = queue_create();
    cdrom->response = queue_create();
    cdrom->parameters = queue_create();
    cdrom->ic = ic;

    queue_init(cdrom->data, CD_SECTOR_SIZE);
    queue_init(cdrom->response, 32);
    queue_init(cdrom->parameters, 32);

    cdrom->version = CDR_VERSION_C0A;
    cdrom->region = CDR_REGION_AMERICA;

    cdrom->vol[0] = 0x80;
    cdrom->vol[1] = 0x00;
    cdrom->vol[2] = 0x80;
    cdrom->vol[3] = 0x00;

    cdrom->pending_lba = 150;
    cdrom->lba = 150;
    cdrom->report_lba = 150;
    cdrom->report_anchor = 150;
    cdrom->report_accum = 0;
    cdrom->seek_precision = 1;
    cdrom->fake_getlocl_data = 1;
}

void psx_cdrom_reset(psx_cdrom_t* cdrom) {
    queue_clear(cdrom->data);
    queue_clear(cdrom->response);
    queue_clear(cdrom->parameters);

    cdrom->prev_state = CD_STATE_IDLE;
    cdrom->state = CD_STATE_IDLE;
    cdrom->pending_command = 0;
    cdrom->busy = 0;
    cdrom->cdda_playing = 0;
    cdrom->xa_playing = 0;
    cdrom->read_ongoing = 0;

    queue_clear(cdrom->data);
    queue_clear(cdrom->response);
    queue_clear(cdrom->parameters);

    cdrom_anchor_report_position(cdrom, cdrom->lba);

    /* Report the run in progress rather than dropping it: a reset is exactly the boundary
       where the last thing the drive was asked to do is worth knowing. */
    cdrom_trace_flush(cdrom);

    cdrom->trace_valid = 0;
    cdrom->trace_in_command = 0;
    cdrom->trace_pending_int = 0;

    cdrom->vol[0] = 0x80;
    cdrom->vol[1] = 0x00;
    cdrom->vol[2] = 0x80;
    cdrom->vol[3] = 0x00;
}

void psx_cdrom_set_version(psx_cdrom_t* cdrom, int version) {
    cdrom->version = version;
}

void psx_cdrom_set_region(psx_cdrom_t* cdrom, int region) {
    cdrom->region = region;
}

int psx_cdrom_open(psx_cdrom_t* cdrom, const char* path) {
    if (!path)
        return 1;

    cdrom_cmd_reset(cdrom);

    cdrom->disc = psx_disc_create();

    if (!cdrom->disc) {
        log_error("Failed to allocate a disc reader for: %s", path);

        return 0;
    }

    /* Remember the image path: save states record disc identity (never the disc itself). */
    {
        size_t n = strlen(path);
        if (n >= sizeof(cdrom->disc_path)) n = sizeof(cdrom->disc_path) - 1;
        memcpy(cdrom->disc_path, path, n);
        cdrom->disc_path[n] = '\0';
    }

    cdrom->disc_type = psx_disc_open(cdrom->disc, path);

    if (cdrom->disc_type == CDT_ERROR) {
        log_error("Failed to open CD image: %s", path);
        psx_cdrom_close(cdrom);

        return 0;
    }

    if (cdrom->disc_type != CDT_LICENSED) {
        log_error("CD image not recognized as a licensed PSX disc (type=%d). Use a proper BIN/CUE rip.", cdrom->disc_type);
        psx_cdrom_close(cdrom);

        return 0;
    }

    log_info("CD image type: Licensed");

    return 1;
}

void psx_cdrom_close(psx_cdrom_t* cdrom) {
    if (cdrom->disc) {
        psx_disc_destroy(cdrom->disc);

        cdrom->disc = NULL;
    }
}

void cdrom_process_setloc(psx_cdrom_t* cdrom) {
    if (!cdrom->pending_lba)
        return;

    cdrom->lba = cdrom->pending_lba;

    cdrom->pending_lba = 0;
}

void cdrom_set_int(psx_cdrom_t* cdrom, int n) {
    /*
        A response REPLACES the response FIFO; it does not append to it. Hardware's FIFO is a
        fixed window that the controller refills when it produces a new response, and every
        cdrom_set_int() call site in this device pushes its bytes immediately afterwards, so
        this is the one place that has to know.

        Measured before this line existed, with a host probe driving the real MMIO interface:
        a driver that leaves bytes of one response unread — an INT1 sector handler that never
        reads the status byte is the ordinary case — walks the write cursor to the end of the
        32-byte buffer in four commands. From there queue_push() silently drops everything,
        and the next command answers with bytes from three responses ago. The probe watched
        CdlPause reply with 32 bytes of 22h, and CdlGetlocP reply with 26 bytes whose first
        two were the tail of an older answer, so track read back as 03h and index as 55h.

        cdrom_error() already did exactly this reset for its own INT5 path.

        Can this discard a response the game has not read yet? No, and the reason is one line
        up in psx_cdrom_update(): "Hold IRQ until IFR is acknowledged" refuses to produce ANY
        new response while an interrupt is outstanding, so nothing can land on top of a
        response the game has not acknowledged. Once it has acknowledged, hardware replaces
        the FIFO too — and the previous behaviour there was worse than replacing it: during a
        streaming read, a game that acknowledges the sector INT1 without reading its status
        byte used to get CdlGetlocP's eight bytes appended BEHIND that byte, so an eight-byte
        read came back as 22 01 01 00 ... — a position whose track field is the drive status.
    */
    queue_reset(cdrom->response);

    /* Attribute the interrupt to the command being traced, or to the transport running
       underneath it. See cdrom_trace_flush(). */
    if (cdrom->trace_in_command)
        cdrom->trace_pending_int = (uint8_t)(n & 7);
    else
        cdrom->trace_async[n & 7]++;

    cdrom->ifr = n;
}

/*
    The reported (physical) position — what CdlGetlocP answers with.

    `cdrom->lba` is the TRANSPORT position: the sector the next read will fetch. It only moves
    when something moves it (Setloc, a completed seek, a sector read, a CD-DA sector). That is
    not what CdlGetlocP returns. CdlGetlocP returns what the drive's sub-Q decoder last saw,
    and a spinning disc always has the head over some sector — it keeps moving when nothing is
    being read at all.

    Measured before this existed, with a host probe against a synthetic single-track disc:

        Setloc 00:20:00 (LBA 1500) + SeekP, then CdlGetlocP
          -> abs 00:19:50  (LBA 1475: the target minus the 25-sector seek undershoot)
          -> abs 00:19:50  after 60 emulated frames
          -> abs 00:19:50  after a further 600 emulated frames

        ReadN, Pause, then CdlGetlocP
          -> abs 00:03:55 and identical 300 frames later

    A game that seeks and then polls CdlGetlocP until the head arrives can never leave that
    loop. It polls once a frame, forever — which is the failure Spyro the Dragon shows at its
    memory-card screen: full speed, the screen drawn, and CdlGetlocp in armsx.log at frame
    rate with no other command in sight.

    Two rules, both of them what the mechanism does:

      * Behind the transport position, because an imprecise seek landed short: walk forward
        one sector per sector-time until it arrives.
      * Sitting on it (parked or paused, motor on): cycle over CD_REPORT_HOLD_SECTORS. The
        position moves, which is what a poll is watching for, and never runs away.

    `cdrom->lba` is deliberately untouched by any of this, so a ReadN resuming after a Pause
    still resumes on the exact sector it left off on.
*/
static void cdrom_update_report_position(psx_cdrom_t* cdrom, int cycles) {
    int sector;

    /* The transport moved the head itself: Setloc, a seek completing, a sector read, a CD-DA
       sector. Checking `lba` against the anchor rather than re-anchoring at each of those
       call sites is what makes it impossible for a new one to be forgotten. */
    if (cdrom->lba != cdrom->report_anchor) {
        cdrom->report_anchor = cdrom->lba;
        cdrom->report_lba = cdrom->lba;
        cdrom->report_accum = 0;

        return;
    }

    if (!cdrom->disc || cycles <= 0)
        return;

    /* The head's own speed, so the CD-ROM speedup setting (a host-side load-time trade) does
       not make the disc spin faster than the drive can. */
    sector = (cdrom->mode & MODE_SPEED) ? CD_DELAY_READ_DS : CD_DELAY_READ_SS;

    cdrom->report_accum += cycles;

    while (cdrom->report_accum >= sector) {
        uint32_t base, pos;

        cdrom->report_accum -= sector;

        /* More than a hold window short of the target: an imprecise seek landed early and the
           head is reading its way onto it. */
        if ((cdrom->report_lba + CD_REPORT_HOLD_SECTORS) <= cdrom->lba) {
            cdrom->report_lba++;

            continue;
        }

        /* Holding station. The window straddles the target — a drive kicks back to hold, so
           it sits a little behind as often as a little ahead — which also keeps the reported
           position from running far past the sector the transport is parked on. */
        base = (cdrom->lba >= (150u + CD_REPORT_HOLD_OFFSET)) ? (cdrom->lba - CD_REPORT_HOLD_OFFSET)
                                                             : cdrom->lba;
        pos = (cdrom->report_lba >= base) ? (cdrom->report_lba - base) : 0u;

        cdrom->report_lba = base + ((pos + 1u) % CD_REPORT_HOLD_SECTORS);
    }
}

/* Commands that only ASK the drive something: no seek, no transport change, no mode change.
   A read running underneath one of these must not have its sector clock restarted by it. */
static int cdrom_command_is_query(uint8_t cmd) {
    switch (cmd) {
        case CDL_GETSTAT:
        case CDL_GETPARAM:
        case CDL_GETLOCL:
        case CDL_GETLOCP:
        case CDL_GETTN:
        case CDL_GETTD:
        case CDL_TEST:
            return 1;
    }

    return 0;
}

/*
    Charge the parked read for the time that passes while a query is being serviced.

    This is the whole reason the stash is a countdown and not a saved constant. The first cut
    subtracted a fixed CD_DELAY_FR on resume, on the assumption that a command costs exactly
    that. It does not: cdrom_write_cmd() sets CD_DELAY_FR, but psx_cdrom_update() then refuses
    to execute the command for as long as the host leaves an interrupt unacknowledged ("Hold
    IRQ until IFR is acknowledged"), and a game that services CD interrupts from its main loop
    can leave one sitting for most of a frame. Every microsecond of that extra wait was time
    the read was NOT charged for, so the sector came early — and at ~50 polls a second the
    error compounds into a drive running ~50% fast, which is the failure this file's
    CD_DELAY_ONGOING_READ comment records as having looped Xenogears' first FMV.

    A disc does not bank time while the host is busy. Ticking the stash with the same `cycles`
    every other timer here uses makes the give-back exact by construction, with no estimate of
    the command's cost in it at all.
*/
static void cdrom_tick_read_delay(psx_cdrom_t* cdrom, int cycles) {
    if (cdrom->read_delay_pending <= 0 || cycles <= 0)
        return;

    cdrom->read_delay_pending -= cycles;

    /* Floor at 1 rather than 0: 0 is the "nothing parked" sentinel, and a sector that came due
       while the query was being serviced should fire as soon as the read resumes. */
    if (cdrom->read_delay_pending < 1)
        cdrom->read_delay_pending = 1;
}

/* Resume a read that a query interrupted, on the clock it was already on. Falls back to the
   normal start-of-read delay when nothing was interrupted. */
static void cdrom_resume_read_delay(psx_cdrom_t* cdrom) {
    if (cdrom->read_delay_pending > 0) {
        cdrom->delay = cdrom->read_delay_pending;

        return;
    }

    cdrom->delay = CD_DELAY_ONGOING_READ;
}

void cdrom_anchor_report_position(psx_cdrom_t* cdrom, uint32_t lba) {
    cdrom->report_anchor = cdrom->lba;
    cdrom->report_lba = lba;
    cdrom->report_accum = 0;
}

/*
    CD-ROM speedup.

    A PlayStation reads at 150 KB/s (1x) or 300 KB/s (2x), and a real disc's seek takes hundreds
    of milliseconds. Both are modelled here as delays, so "speeding up the drive" is just dividing
    them — the emulated drive then returns data sooner than any real one could, which is where the
    load-time win comes from.

    Divisors, not percentages: 1 leaves the timing exactly as it was, 2 halves it, and so on.

    ⚠ This is a compatibility trade, not a free win. Games that stream XA audio or FMV expect data
    to arrive at the drive's real rate and can stutter, desync their audio, or drop frames when it
    arrives early. Seek speedup is the safer of the two; read speedup is the one that breaks FMV.
    Both default to 1.
*/
static unsigned g_cd_read_speedup = 1;
static unsigned g_cd_seek_speedup = 1;

void psx_cdrom_set_read_speedup(unsigned factor) {
    g_cd_read_speedup = (factor < 1) ? 1 : ((factor > 16) ? 16 : factor);
}

void psx_cdrom_set_seek_speedup(unsigned factor) {
    g_cd_seek_speedup = (factor < 1) ? 1 : ((factor > 16) ? 16 : factor);
}

unsigned psx_cdrom_get_read_speedup(void) { return g_cd_read_speedup; }
unsigned psx_cdrom_get_seek_speedup(void) { return g_cd_seek_speedup; }

/* Never returns 0: a zero delay would make the drive respond within the same cycle it was asked,
   which is not "fast" but "instant", and the state machine treats a zero delay as "already done"
   rather than scheduling a completion. */
static int cd_apply_speedup(int delay, unsigned factor) {
    if (factor <= 1 || delay <= 0)
        return delay;

    delay /= (int)factor;

    return delay > 0 ? delay : 1;
}

int cdrom_get_read_delay(psx_cdrom_t* cdrom) {
    const int base = (cdrom->mode & MODE_SPEED) ? CD_DELAY_READ_DS : CD_DELAY_READ_SS;

    return cd_apply_speedup(base, g_cd_read_speedup);
}

int cdrom_get_pause_delay(psx_cdrom_t* cdrom) {
    if (!cdrom->read_ongoing)
        return 7000;

    return CD_DELAY_1MS * ((cdrom->mode & MODE_SPEED) ? 35 : 70);
}

int cdrom_get_seek_delay(psx_cdrom_t* cdrom, int ts) {
    int delay = cdrom->pending_speed_switch_delay;

    cdrom->pending_speed_switch_delay = 0;

    // Ridiculous delays for seeking to an audio sector
    // or out of the disk
    if (ts == TS_FAR)   delay = 650 * CD_DELAY_1MS;
    if (ts == TS_AUDIO) delay = 4000 * CD_DELAY_1MS;
    // if (ts == TS_PREGAP) delay = 4000 * CD_DELAY_1MS;

    return cd_apply_speedup(delay, g_cd_seek_speedup);
}

void cdrom_error(psx_cdrom_t* cdrom, uint8_t stat, uint8_t err) {
    cdrom->ifr = 5;

    queue_reset(cdrom->parameters);
    queue_reset(cdrom->response);

    if ((stat & CD_STAT_IDERROR) || (stat & CD_STAT_SEEKERROR)) {
        queue_push(cdrom->response, stat);
    } else {
        queue_push(cdrom->response, CD_STAT_ERROR | stat);
    }

    queue_push(cdrom->response, err);

    cdrom->prev_state = CD_STATE_IDLE;
    cdrom->state = CD_STATE_IDLE;
    cdrom->pending_command = 0;
    cdrom->busy = 0;
}

void cdrom_handle_resp1(psx_cdrom_t* cdrom) {
    cdrom->busy = 0;

    // Check for no disc, some commands can be issued with no disc
    // in the drive (e.g. Test, Setmode, Init, etc.)
    // i.e. INT5(11h, 80h)
    if (!cdrom->disc) {
        switch (cdrom->pending_command) {
            case CDL_SETLOC:
            case CDL_PLAY:
            case CDL_FORWARD:
            case CDL_BACKWARD:
            case CDL_READN:
            case CDL_MOTORON:
            case CDL_STOP:
            case CDL_PAUSE:
            case CDL_MUTE:
            case CDL_DEMUTE:
            case CDL_SETFILTER:
            case CDL_GETLOCL:
            case CDL_GETLOCP:
            case CDL_SETSESSION:
            case CDL_GETTN:
            case CDL_GETTD:
            case CDL_SEEKL:
            case CDL_SEEKP:
            case CDL_GETID:
            case CDL_READS:
            case CDL_GETQ: {
                cdrom_error(cdrom, CD_STAT_SHELLOPEN, CD_ERR_NO_DISC);

                return;
            } break;
        }
    }

    // Check for version-specific unsupported commands
    switch (cdrom->pending_command) {
        case CDL_RESET: {
            if (cdrom->version == CDR_VERSION_01) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_INVALID_COMMAND
                );

                return;
            }
        } break;

        case CDL_GETQ:
        case CDL_READTOC: {
            if (cdrom->version < CDR_VERSION_C1A) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_INVALID_COMMAND
                );

                return;
            }
        } break;
    }

    // Check for wrong number of parameters and invalid commands
    // i.e. INT5(03h, 20h), INT5(03h, 40h)
    switch (cdrom->pending_command) {
        case CDL_GETSTAT:
        case CDL_FORWARD:
        case CDL_BACKWARD:
        case CDL_READN:
        case CDL_MOTORON:
        case CDL_STOP:
        case CDL_PAUSE:
        case CDL_INIT:
        case CDL_MUTE:
        case CDL_DEMUTE:
        case CDL_GETPARAM:
        case CDL_GETLOCL:
        case CDL_GETLOCP:
        case CDL_GETTN:
        case CDL_SEEKL:
        case CDL_SEEKP:
        case CDL_GETID:
        case CDL_READS:
        case CDL_RESET:
        case CDL_READTOC: {
            // These commands take no parameters
            if (queue_size(cdrom->parameters)) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        case CDL_SETLOC: {
            // Setloc takes exactly 3 parameters
            if (queue_size(cdrom->parameters) != 3) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        case CDL_PLAY: {
            // Play may take either 0 or 1 parameter
            if (queue_size(cdrom->parameters) > 1) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        case CDL_SETFILTER:
        case CDL_GETQ: {
            // Setfilter and GetQ both take exactly 2 parameters
            if (queue_size(cdrom->parameters) != 2) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        case CDL_SETMODE:
        case CDL_GETTD:
        case CDL_TEST: {
            // Setmode and GetTD both take exactly 1 parameter

            // Test may actually take additional parameters depending
            // on the subfunction, but we only emulate subfunction 20h
            // for now, which takes no extra parameters
            if (queue_size(cdrom->parameters) != 1) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        case CDL_VIDEOCD: {
            // To-do: Check for model
            // VideoCD is only supported on the SCPH-5903
            // Should return invalid command normally

            // VideoCD takes exactly 6 parameters
            if (queue_size(cdrom->parameters) != 6) {
                cdrom_error(cdrom,
                    CD_STAT_SPINDLE,
                    CD_ERR_WRONG_PARAMETER_COUNT
                );

                return;
            }
        } break;

        default: {
            // Invalid command
            cdrom_error(cdrom,
                CD_STAT_SPINDLE,
                CD_ERR_INVALID_COMMAND
            );

            return;
        } break;
    }

    // If everything is alright (i.e. disc present, valid command,
    // correct number of parameters) then send "execute command"
    cdrom_cmd_table[cdrom->pending_command](cdrom);
}

uint8_t cdrom_get_stat(psx_cdrom_t* cdrom) {
    return ((cdrom->cdda_playing) ? CD_STAT_PLAY : 0) |
           ((cdrom->read_ongoing) ? CD_STAT_READ : 0) |
           ((cdrom->mode & 0x10) ? CD_STAT_IDERROR : 0) |
           ((!cdrom->disc) ? CD_STAT_SHELLOPEN : 0) |
           CD_STAT_SPINDLE;
}

void cdrom_handle_read(psx_cdrom_t* cdrom) {
    cdrom_process_setloc(cdrom);

    int ts = psx_disc_query(cdrom->disc, cdrom->lba);

    if (ts == TS_FAR) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE | CD_STAT_SEEKERROR,
            CD_ERR_INVALID_SUBFUNCTION
        );

        return;
    }

    //if (ts == TS_AUDIO || ts == TS_PREGAP) {
    if (ts == TS_AUDIO) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE | CD_STAT_SEEKERROR,
            CD_ERR_SEEK_FAILED
        );

        return;
    }

    /*
        Read into scratch FIRST, because whether this sector belongs to the host at all is a
        property of its subheader, and overwriting the data FIFO before knowing that would
        corrupt a sector the game has not collected yet.

        With Mode.bit6 (XA-ADPCM) set, a Form2 audio sector whose file/channel match the filter
        is consumed by the controller's ADPCM decoder: it goes to the SPU and raises NO data
        interrupt. Only the other sectors are handed to the host.

        Measured against an interleaved fixture (1 data : 7 audio, file 1/chan 4, mode c8 —
        Spyro's own configuration from the device log), before this existed:

            walked 160 sectors:  INT1 raised 160, of which 20 data and 140 XA AUDIO

        Hardware raises about 20. The game's data handler was being fed eight times the
        sectors it expected, seven of every eight containing ADPCM, so it could never parse
        its stream — which is why three rounds of fixing the transport changed nothing. The
        transport was already delivering; the routing was wrong.

        Nothing here touches the audio: psx_cdrom_get_audio_samples() walks its own xa_lba
        independently of this path, so suppressing the interrupt does not suppress the sound.
    */
    {
        static uint8_t sector[CD_SECTOR_SIZE];

        psx_disc_read(cdrom->disc, cdrom->lba, sector);

        if (cdrom->mode & MODE_XA_ADPCM) {
            const uint8_t submode = sector[0x12];

            /* Audio (bit2) AND Form2 (bit5): both, or it is not an ADPCM sector. A Form2
               sector that is not audio — video, or real-time data — is still the host's. */
            if ((submode & 0x04) && (submode & 0x20)) {
                /*
                    Deliberately NOT conditioned on the filter. The filter decides which
                    channel reaches the SPU and which is discarded; it does not decide whether
                    an ADPCM sector counts as data. psx-spx on Setfilter: it "selects which of
                    the interleaved audio channels shall be played (the other channels are
                    ignored)" — ignored, not handed to the host.

                    Getting this wrong left 7 of every 8 sectors still being delivered as
                    data on an 8-channel interleave, which is exactly what Spyro streams:
                    audio_diag showed channels cycling 00..07 against filter 01/04, every one
                    of them audio=1 data=0 form2=1, and INT1 arriving at 131/s — 7/8 of the
                    drive rate. SPU delivery is psx_cdrom_get_audio_samples()'s job, on its
                    own xa_lba walk, and it applies the filter itself.
                */
                cdrom->pending_lba = cdrom->lba + 1;
                cdrom->delay = cdrom_get_read_delay(cdrom);

                return;
            }
        }

        memcpy(cdrom->data->buf, sector, CD_SECTOR_SIZE);
    }

    cdrom_set_int(cdrom, 1);
    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    int size_bit = cdrom->mode & MODE_SECTOR_SIZE;

    cdrom->data->read_index = size_bit ? 12 : 24;
    cdrom->data->write_index = size_bit ? 0x924 : 0x800;
    cdrom->data->write_index += cdrom->data->read_index;

    cdrom->pending_lba = cdrom->lba + 1;
    cdrom->delay = cdrom_get_read_delay(cdrom);

    // printf("size=%x off=%u lba=%d: %02x:%02x:%02x delay=%u\n",
    //     cdrom->data->write_index,
    //     cdrom->data->read_index,
    //     cdrom->lba,
    //     cdrom->data->buf[0xc],
    //     cdrom->data->buf[0xd],
    //     cdrom->data->buf[0xe],
    //     cdrom->data->buf[0xf],
    //     cdrom->delay
    // );
}

/*
    CD command trace.

    This was an unconditional printf on every command write. A game that polls CdlGetlocP once
    a frame — the hang the reported-position block above exists for — turned that into ~60
    lines a second: 9270 bytes in three seconds on the device, 1.7 MB of armsx.log in minutes,
    with every other line in the file buried under it. A capture of the hang said almost
    nothing as a result.

    Runs of the same command with the same parameters now collapse into one line that carries
    the repeat count and the response the drive actually gave, which is the half that was
    missing: the old trace printed at command-WRITE time, so it could not distinguish "the
    game keeps asking" from "the game keeps asking and we keep answering with the same thing".

        cdrom: CdlGetlocp   (11) x512 exec=512 params: (none) -> INT3 01 01 00 17 50 00 19 50
                                | async INT1 x430 | state=3 lba=113598 head=113598

    The first cut of this got the attribution wrong and cost a wrong diagnosis, so it is worth
    being explicit about. The response snapshot used to be taken after EVERY update that
    raised an interrupt, including the per-sector INT1 of a read still running underneath the
    polling. During an XA stream that is 75-150 interrupts a second against ~50 polls, so the
    last one before each flush was almost always a sector interrupt, and a CdlGetlocP run
    printed `-> INT1 22` — the read's data-ready status, attributed to a command that had in
    fact answered INT3 correctly every time.

    So an interrupt is now attributed to the command only if it was raised while that command
    was executing (`trace_in_command`), and everything else is counted separately. The counts
    are the diagnosis on their own: `exec` is how many of the writes reached the command
    implementation at all, and `async INT1` is whether the transport is still delivering
    sectors while the game polls.

    A run still going after CD_TRACE_RUN_FLUSH repeats reports and keeps counting, so a hang
    is never silent; a run ends (and prints) as soon as a different command arrives.
*/
#define CD_TRACE_RUN_FLUSH 512

static void cdrom_trace_hex(char* dst, size_t size, const uint8_t* src, unsigned n) {
    size_t used = 0;

    dst[0] = '\0';

    for (unsigned i = 0; i < n; i++) {
        int written;

        if (used + 4u >= size)
            break;

        written = snprintf(dst + used, size - used, "%02x ", src[i]);

        if (written <= 0)
            break;

        used += (size_t)written;
    }
}

static void cdrom_trace_flush(psx_cdrom_t* cdrom) {
    char params[24];
    char resp[40];
    char async[64];
    size_t used = 0;

    if (!cdrom->trace_valid || !cdrom->trace_repeat)
        return;

    cdrom_trace_hex(params, sizeof(params), cdrom->trace_params, cdrom->trace_nparams);
    cdrom_trace_hex(resp, sizeof(resp), cdrom->trace_resp, cdrom->trace_nresp);

    async[0] = '\0';

    for (unsigned n = 1; n < 8; n++) {
        int written;

        if (!cdrom->trace_async[n] || used + 16u >= sizeof(async))
            continue;

        written = snprintf(async + used, sizeof(async) - used, " INT%u x%u",
                           n, cdrom->trace_async[n]);

        if (written <= 0)
            break;

        used += (size_t)written;
    }

    char answer[56];

    if (cdrom->trace_nresp)
        snprintf(answer, sizeof(answer), "INT%u %s", cdrom->trace_int, resp);
    else
        snprintf(answer, sizeof(answer), "(no response)");

    /*
        exec < repeat means writes that never reached the command implementation; exec == 0
        with a run in the hundreds is a wedged controller. A populated `async` column while the
        poll makes no progress says the transport is still running underneath it; an empty one
        says the polling starved it.

        `mode` and `spd` are here because a sector rate cannot be judged without them, and
        judging one without them has already cost a wrong call: `async INT1` counted against
        wall time looked like the drive running 48% fast, when mode bit7 (speed) and the host's
        read-speedup divisor between them said it was running at exactly the rate it had been
        configured for. Expected sectors/s = (75 << mode.bit7) * spd.
    */
    printf("cdrom: %-14s (%02x) x%u exec=%u params: %s-> %s | async%s | state=%d lba=%u head=%u "
           "mode=%02x spd=%ux\n",
        psx_cdrom_command_name(cdrom->trace_cmd),
        cdrom->trace_cmd,
        cdrom->trace_repeat,
        cdrom->trace_exec,
        cdrom->trace_nparams ? params : "(none) ",
        answer,
        used ? async : " (none)",
        cdrom->state,
        cdrom->lba,
        cdrom->report_lba,
        cdrom->mode,
        psx_cdrom_get_read_speedup());

    cdrom->trace_repeat = 0;
    cdrom->trace_exec = 0;

    memset(cdrom->trace_async, 0, sizeof(cdrom->trace_async));
}

static void cdrom_trace_cmd(psx_cdrom_t* cdrom, uint8_t data) {
    uint8_t params[4] = { 0, 0, 0, 0 };
    int n = queue_size(cdrom->parameters);

    if (n < 0) n = 0;
    if (n > 4) n = 4;

    for (int i = 0; i < n; i++)
        params[i] = cdrom->parameters->buf[cdrom->parameters->read_index + i];

    if (cdrom->trace_valid &&
        cdrom->trace_cmd == data &&
        cdrom->trace_nparams == (uint8_t)n &&
        !memcmp(cdrom->trace_params, params, sizeof(params))) {
        cdrom->trace_repeat++;

        if (cdrom->trace_repeat >= CD_TRACE_RUN_FLUSH)
            cdrom_trace_flush(cdrom);

        return;
    }

    cdrom_trace_flush(cdrom);

    cdrom->trace_valid = 1;
    cdrom->trace_cmd = data;
    cdrom->trace_nparams = (uint8_t)n;
    memcpy(cdrom->trace_params, params, sizeof(params));
    cdrom->trace_nresp = 0;
    cdrom->trace_int = 0;
    cdrom->trace_repeat = 1;
    cdrom->trace_exec = 0;

    memset(cdrom->trace_async, 0, sizeof(cdrom->trace_async));
}

/* Snapshot what the TRACED COMMAND put in the response FIFO, so the run's one line shows what
   the game is actually being told. Only runs if cdrom_set_int() saw the interrupt raised from
   inside the command dispatch — a sector interrupt from a read running underneath the polling
   is counted in trace_async instead, and must never land here. Peeks; never consumes. */
static void cdrom_trace_command_response(psx_cdrom_t* cdrom) {
    int n;

    if (!cdrom->trace_valid || !cdrom->trace_pending_int)
        return;

    n = queue_size(cdrom->response);

    if (n < 0) n = 0;
    if (n > (int)sizeof(cdrom->trace_resp)) n = (int)sizeof(cdrom->trace_resp);

    for (int i = 0; i < n; i++)
        cdrom->trace_resp[i] = cdrom->response->buf[cdrom->response->read_index + i];

    cdrom->trace_nresp = (uint8_t)n;
    cdrom->trace_int = cdrom->trace_pending_int;
    cdrom->trace_pending_int = 0;
    cdrom->trace_exec++;
}

void psx_cdrom_update(psx_cdrom_t* cdrom, int cycles) {
    /* Before every early return below: the disc keeps spinning whether or not the controller
       has anything to do. CdlGetlocP has to see that, and a read parked behind a query has to
       be charged for it. */
    cdrom_update_report_position(cdrom, cycles);
    cdrom_tick_read_delay(cdrom, cycles);

    if (cdrom->delay > 0) {
        cdrom->delay -= cycles;

        if (cdrom->delay > 0)
            return;
    }

    cdrom->delay = 0;

    if (cdrom->state == CD_STATE_IDLE)
        return;

    if (cdrom->state == CD_STATE_PLAY)
        return;

    // Hold IRQ until IFR is acknowledged
    if (cdrom->ifr & 0x1f) {
        cdrom->delay = 2;

        return;
    }

    switch (cdrom->state) {
        case CD_STATE_TX_RESP1: {
            cdrom->trace_in_command = 1;
            cdrom_handle_resp1(cdrom);
            cdrom->trace_in_command = 0;

            switch (cdrom->pending_command) {
                case CDL_READN:
                case CDL_READS:
                    break;
            }

            // Switching to read mode after executing a command
            // has a 500ms penalty
            if (cdrom->state == CD_STATE_READ) {
                cdrom_process_setloc(cdrom);

                cdrom->state = CD_STATE_READ;
                cdrom->prev_state = CD_STATE_READ;

                cdrom_resume_read_delay(cdrom);
            }
        } break;

        case CD_STATE_TX_RESP2: {
            cdrom->trace_in_command = 1;
            cdrom_cmd_table[cdrom->pending_command](cdrom);
            cdrom->trace_in_command = 0;

            // Switching to read mode after executing a command
            // has a 500ms penalty
            if (cdrom->state == CD_STATE_READ) {
                cdrom_process_setloc(cdrom);

                cdrom->state = CD_STATE_READ;
                cdrom->prev_state = CD_STATE_READ;

                cdrom_resume_read_delay(cdrom);
            }
        } break;

        case CD_STATE_READ: {
            cdrom_handle_read(cdrom);
        } break;
    }

    /* One command service is the whole life of the stash. If the command errored out instead
       of returning the drive to CD_STATE_READ, nothing consumed it and it must not survive to
       be applied to some later read. */
    cdrom->read_delay_pending = 0;

    cdrom_trace_command_response(cdrom);

    if ((cdrom->ifr & cdrom->ier) == 0)
        return;

    psx_ic_irq(cdrom->ic, IC_CDROM);
}

uint8_t cdrom_read_status(psx_cdrom_t* cdrom) {
    int data_empty = queue_is_empty(cdrom->data) || !cdrom->data_req;

    return (cdrom->index                        << 0) |
           (cdrom->xa_playing                   << 2) |
           (queue_is_empty(cdrom->parameters)   << 3) |
           ((!queue_is_full(cdrom->parameters)) << 4) |
           ((!queue_is_empty(cdrom->response))  << 5) |
           ((!data_empty)                       << 6) |
           (cdrom->busy                         << 7);
}

uint8_t cdrom_read_data(psx_cdrom_t* cdrom) {
    if (!cdrom->data_req)
        return 0;

    // printf("read=%x write=%x data=%02x |%c|\n",
    //     cdrom->data->read_index,
    //     cdrom->data->write_index,
    //     queue_peek(cdrom->data),
    //     isprint(queue_peek(cdrom->data)) ? queue_peek(cdrom->data) : '.'
    // );

    return queue_pop(cdrom->data);
}

uint32_t psx_cdrom_read8(psx_cdrom_t* cdrom, uint32_t addr) {
    switch (addr) {
        case 0: return cdrom_read_status(cdrom);
        case 1: return queue_pop(cdrom->response);
        case 2: return cdrom_read_data(cdrom);
        case 3: return (cdrom->index & 1) ? (0xe0 | cdrom->ifr) : cdrom->ier;
    }

    return 0;
}

void psx_cdrom_write8(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value) {
    switch ((cdrom->index << 2) | addr) {
        case 0: cdrom_write_stat(cdrom, value); break;
        case 1: cdrom_write_cmd(cdrom, value); break;
        case 2: cdrom_write_parm(cdrom, value); break;
        case 3: cdrom_write_req(cdrom, value); break;
        case 4: cdrom_write_stat(cdrom, value); break;
        case 5: cdrom_write_null(cdrom, value); break;
        case 6: cdrom_write_ier(cdrom, value); break;
        case 7: cdrom_write_ifr(cdrom, value); break;
        case 8: cdrom_write_stat(cdrom, value); break;
        case 9: cdrom_write_null(cdrom, value); break;
        case 10: cdrom_write_vol0(cdrom, value); break;
        case 11: cdrom_write_vol1(cdrom, value); break;
        case 12: cdrom_write_stat(cdrom, value); break;
        case 13: cdrom_write_vol2(cdrom, value); break;
        case 14: cdrom_write_vol3(cdrom, value); break;
        case 15: cdrom_write_vapp(cdrom, value); break;
    }
}

void psx_cdrom_destroy(psx_cdrom_t* cdrom) {
    psx_cdrom_close(cdrom);
    queue_destroy(cdrom->data);
    queue_destroy(cdrom->response);
    queue_destroy(cdrom->parameters);
    free(cdrom);
}

void cdrom_write_stat(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->index = data & 3;
}

/* The name table above is static; the host's audio_diag needs it to print the command log. */
const char* psx_cdrom_command_name(uint8_t cmd) {
    if (cmd >= (sizeof(cdrom_cmd_names) / sizeof(cdrom_cmd_names[0])))
        return "<out-of-range>";

    return cdrom_cmd_names[cmd];
}

void cdrom_write_cmd(psx_cdrom_t* cdrom, uint8_t data) {
    /* `audio_diag`. Captured HERE rather than in the command implementations because this is
       the only point where the parameter FIFO is still intact — the implementations pop it —
       and because the drive state recorded is the state the command is about to act on. */
    if (g_psx_audio_diag_enabled) {
        uint32_t slot = g_psx_audio_diag.cd_cmd_head;
        int n = queue_size(cdrom->parameters);

        if (n < 0)
            n = 0;

        if (n > 4)
            n = 4;

        g_psx_audio_diag.cd_cmd[slot].cmd = data;
        g_psx_audio_diag.cd_cmd[slot].nparams = (uint8_t)n;

        for (int i = 0; i < 4; i++) {
            g_psx_audio_diag.cd_cmd[slot].param[i] =
                (i < n) ? cdrom->parameters->buf[cdrom->parameters->read_index + i] : 0u;
        }

        g_psx_audio_diag.cd_cmd[slot].mode_before = cdrom->mode;
        g_psx_audio_diag.cd_cmd[slot].xa_playing = (uint8_t)(cdrom->xa_playing ? 1 : 0);
        g_psx_audio_diag.cd_cmd[slot].read_ongoing = (uint8_t)(cdrom->read_ongoing ? 1 : 0);
        g_psx_audio_diag.cd_cmd[slot].state = (uint8_t)cdrom->state;

        g_psx_audio_diag.cd_cmd_head = (slot + 1u) % PSX_AUDIO_DIAG_CD_CMDS;
        g_psx_audio_diag.cd_cmd_seen++;
    }

    cdrom->prev_state = cdrom->state;

    /*
        A command and a running read share `cdrom->delay`, so writing a command discards
        however much of the read's sector countdown had elapsed, and the read is then
        rescheduled from scratch. The drive's sector rate therefore collapses to the rate the
        game asks it questions.

        Measured on the host, replaying Spyro's own sequence (Setmode e0, Setfilter 01 04,
        Setloc, ReadS, then CdlGetlocP once a frame the way the device log shows): 120 sector
        interrupts in 120 frames. Exactly one per frame, when a 2x XA stream needs 150 a
        second. At 1x it is worse than throttled — CD_DELAY_ONGOING_READ is 17.3 ms against a
        16.7 ms poll period, so no sector ever completes at all.

        A real drive does not restart its sector clock because the host asked where the head
        is. So remember what the read had left and give it back, minus the time the command
        itself takes.

        Only for commands that cannot touch the transport. Setmode mid-stream is a real
        transport event that this file already has scar tissue about, and every game that
        works today reaches CD_DELAY_ONGOING_READ through one of those — leaving their pacing
        bit-identical is worth more than being thorough here.
    */
    if (cdrom->prev_state == CD_STATE_READ && cdrom_command_is_query(data))
        cdrom->read_delay_pending = (cdrom->delay > 0) ? cdrom->delay : 1;

    cdrom->state = CD_STATE_TX_RESP1;

    cdrom->pending_command = data;

    switch (cdrom->pending_command) {
        case CDL_INIT:
            cdrom->delay = CD_DELAY_INIT_FR;
        break;

        default:
            cdrom->delay = CD_DELAY_FR;
        break;
    }

    if (cdrom->state == CD_STATE_READ)
        cdrom->busy = 1;

    /* Was an unconditional printf per command, and indexed cdrom_cmd_names[] with an
       unchecked command byte on the way. See cdrom_trace_cmd(). */
    cdrom_trace_cmd(cdrom, data);
}

void cdrom_write_null(psx_cdrom_t* cdrom, uint8_t data) {
    /* Ignore writes */
}

void cdrom_write_parm(psx_cdrom_t* cdrom, uint8_t data) {
    queue_push(cdrom->parameters, data);
}

void cdrom_write_ier(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->ier = data;
}

void cdrom_write_ifr(psx_cdrom_t* cdrom, uint8_t data) {
    if (data & 0x40)
        queue_clear(cdrom->parameters);

    // uint8_t prev_ifr = cdrom->ifr & 0x1f;

    cdrom->ifr &= ~(data & 0x1f);

    // If an INT is acknowledged, then the response
    // FIFO is cleared
    // if (((cdrom->ifr & 0x1f) == 0) && (prev_ifr != 0))
    //     queue_clear(cdrom->response);
}

void cdrom_write_req(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->data_req = data & 0x80;
}

void cdrom_write_vol0(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->vol_pending[0] = data;
}

void cdrom_write_vol1(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->vol_pending[1] = data;
}

void cdrom_write_vol2(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->vol_pending[2] = data;
}

void cdrom_write_vol3(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->vol_pending[3] = data;
}

void cdrom_write_vapp(psx_cdrom_t* cdrom, uint8_t data) {
    cdrom->xa_mute = data & 1;
    cdrom->vol[0] = cdrom->vol_pending[0];
    cdrom->vol[1] = cdrom->vol_pending[1];
    cdrom->vol[2] = cdrom->vol_pending[2];
    cdrom->vol[3] = cdrom->vol_pending[3];
}

uint32_t psx_cdrom_read32(psx_cdrom_t* cdrom, uint32_t addr) {
    assert("32-bit CDROM reads are not supported" && 0);

    return 0;
}

uint32_t psx_cdrom_read16(psx_cdrom_t* cdrom, uint32_t addr) {
    assert("16-bit CDROM reads are not supported" && 0);

    // The CDROM controller is connected to the SUB-BUS which is a 16-bit
    // bus, but the output from the controller itself is 8-bit. I think
    // 16-bit accesses are handled as a pair of 8-bit accesses
    return psx_cdrom_read8(cdrom, addr) << 8 | psx_cdrom_read8(cdrom, addr+1);
}

void psx_cdrom_write32(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value) {
    assert("32-bit CDROM writes are not supported" && 0);
}

void psx_cdrom_write16(psx_cdrom_t* cdrom, uint32_t addr, uint32_t value) {
    assert("16-bit CDROM writes are not supported" && 0);
}


/* ---------------------------------------------------------------------------------------
   Save-state support.

   The disc is a host resource and is NEVER serialised — a state would otherwise embed a
   several-hundred-megabyte image. Instead we persist the disc's IDENTITY (path, track count
   and a cheap fingerprint over the table of contents) so psx_load_state can refuse a state
   that belongs to a different game, and only the controller's logical state, so loading
   simply re-seeks the disc that is already open.

   Pointers (disc, ic) and the queue backing buffers are rebuilt from the surrounding
   objects, never restored from the stream.
   --------------------------------------------------------------------------------------- */

const char* psx_cdrom_get_disc_path(psx_cdrom_t* cdrom) {
    if (!cdrom)
        return "";

    return cdrom->disc_path;
}

int psx_cdrom_get_disc_track_count(psx_cdrom_t* cdrom) {
    if (!cdrom || !cdrom->disc)
        return 0;

    return psx_disc_get_track_count(cdrom->disc);
}

uint64_t psx_cdrom_get_disc_fingerprint(psx_cdrom_t* cdrom) {
    /* FNV-1a over the image's basename plus its table of contents. Deliberately does not read
       sector data: this must stay cheap enough to call on every save/load, and the TOC is
       already enough to tell two different games apart. */
    uint64_t hash = 1469598103934665603ULL;
    const char* base;
    const char* p;
    int tracks, i;

    if (!cdrom)
        return 0;

    base = cdrom->disc_path;

    for (p = cdrom->disc_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    for (p = base; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= 1099511628211ULL;
    }

    tracks = psx_cdrom_get_disc_track_count(cdrom);

    hash ^= (uint64_t)(uint32_t)tracks;
    hash *= 1099511628211ULL;

    for (i = 1; i <= tracks; i++) {
        uint32_t lba = (uint32_t)psx_disc_get_track_lba(cdrom->disc, i);

        hash ^= (uint64_t)lba;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void cdrom_save_queue(psx_state_writer_t* w, queue_t* q) {
    /* Serialise a queue by VALUE, not by its ring indices: the buffer is allocated by
       queue_init and its capacity belongs to the running machine, not to the state. */
    int size = q ? queue_size(q) : 0;
    int i;

    psx_sw_i32(w, size);

    if (!q)
        return;

    for (i = 0; i < size; i++) {
        size_t index = (q->read_index + (size_t)i) % (q->size ? q->size : 1);

        psx_sw_u8(w, q->buf ? q->buf[index] : 0);
    }
}

static void cdrom_load_queue(psx_state_reader_t* r, queue_t* q) {
    int size = psx_sr_i32(r);
    int i;

    if (q)
        queue_clear(q);

    for (i = 0; i < size; i++) {
        uint8_t value = psx_sr_u8(r);

        if (q && !queue_is_full(q))
            queue_push(q, value);
    }
}

void psx_cdrom_save_state(psx_cdrom_t* cdrom, psx_state_writer_t* w) {
    psx_sw_i32(w, cdrom->mute);
    psx_sw_u32(w, cdrom->bus_delay);
    psx_sw_i32(w, cdrom->disc_type);
    psx_sw_i32(w, cdrom->version);
    psx_sw_i32(w, cdrom->region);
    psx_sw_i32(w, cdrom->index);
    psx_sw_i32(w, cdrom->pending_speed_switch_delay);
    psx_sw_i32(w, cdrom->seek_precision);
    psx_sw_i32(w, cdrom->fake_getlocl_data);
    psx_sw_u8(w, cdrom->ier);
    psx_sw_u8(w, cdrom->ifr);
    psx_sw_bytes(w, cdrom->vol_pending, sizeof(cdrom->vol_pending));
    psx_sw_bytes(w, cdrom->vol, sizeof(cdrom->vol));
    psx_sw_u8(w, cdrom->mode);
    psx_sw_i32(w, cdrom->data_req);

    cdrom_save_queue(w, cdrom->data);
    cdrom_save_queue(w, cdrom->response);
    cdrom_save_queue(w, cdrom->parameters);

    psx_sw_u8(w, cdrom->pending_command);
    psx_sw_i32(w, cdrom->busy);

    psx_sw_u32(w, cdrom->xa_lba);
    psx_sw_i32(w, cdrom->xa_playing);
    psx_sw_i32(w, cdrom->xa_mute);
    psx_sw_i32(w, cdrom->xa_channel);
    psx_sw_i32(w, cdrom->xa_file);
    psx_sw_i32(w, cdrom->xa_remaining_samples);
    psx_sw_i16_array(w, cdrom->xa_left_h, 2);
    psx_sw_i16_array(w, cdrom->xa_right_h, 2);
    psx_sw_i32(w, cdrom->xa_prev_left_sample);
    psx_sw_i32(w, cdrom->xa_prev_right_sample);
    psx_sw_i32(w, cdrom->xa_sample_index);

    psx_sw_i32(w, cdrom->state);
    psx_sw_i32(w, cdrom->prev_state);
    psx_sw_i32(w, cdrom->int1_pending);
    psx_sw_i32(w, cdrom->int2_pending);
    psx_sw_i32(w, cdrom->delay);
    psx_sw_u32(w, cdrom->pending_lba);
    psx_sw_u32(w, cdrom->lba);

    psx_sw_i16_array(w, cdrom->cdda_buf, CD_SECTOR_SIZE >> 1);
    psx_sw_i32(w, cdrom->cdda_remaining_samples);
    psx_sw_u32(w, cdrom->cdda_sample_index);
    psx_sw_u32(w, cdrom->cdda_sectors_played);
    psx_sw_i32(w, cdrom->cdda_playing);
    psx_sw_i32(w, cdrom->cdda_prev_track);
    psx_sw_i32(w, cdrom->read_ongoing);

    psx_sw_bytes(w, cdrom->xa_buf, CD_SECTOR_SIZE);
    psx_sw_i16_array(w, cdrom->xa_left_buf, XA_STEREO_SAMPLES);
    psx_sw_i16_array(w, cdrom->xa_right_buf, XA_STEREO_SAMPLES);
    psx_sw_i16_array(w, cdrom->xa_mono_buf, XA_MONO_SAMPLES);
    psx_sw_i16_array(w, cdrom->xa_upsample_buf, XA_UPSAMPLE_SIZE);
    psx_sw_i16_array(w, cdrom->xa_left_resample_buf, XA_STEREO_RESAMPLE_MAX_SIZE);
    psx_sw_i16_array(w, cdrom->xa_right_resample_buf, XA_STEREO_RESAMPLE_MAX_SIZE);
    psx_sw_i16_array(w, cdrom->xa_mono_resample_buf, XA_MONO_RESAMPLE_MAX_SIZE);
}

int psx_cdrom_load_state(psx_cdrom_t* cdrom, psx_state_reader_t* r) {
    cdrom->mute = psx_sr_i32(r);
    cdrom->bus_delay = psx_sr_u32(r);
    cdrom->disc_type = psx_sr_i32(r);
    cdrom->version = psx_sr_i32(r);
    cdrom->region = psx_sr_i32(r);
    cdrom->index = psx_sr_i32(r);
    cdrom->pending_speed_switch_delay = psx_sr_i32(r);
    cdrom->seek_precision = psx_sr_i32(r);
    cdrom->fake_getlocl_data = psx_sr_i32(r);
    cdrom->ier = psx_sr_u8(r);
    cdrom->ifr = psx_sr_u8(r);
    psx_sr_bytes(r, cdrom->vol_pending, sizeof(cdrom->vol_pending));
    psx_sr_bytes(r, cdrom->vol, sizeof(cdrom->vol));
    cdrom->mode = psx_sr_u8(r);
    cdrom->data_req = psx_sr_i32(r);

    cdrom_load_queue(r, cdrom->data);
    cdrom_load_queue(r, cdrom->response);
    cdrom_load_queue(r, cdrom->parameters);

    cdrom->pending_command = psx_sr_u8(r);
    cdrom->busy = psx_sr_i32(r);

    cdrom->xa_lba = psx_sr_u32(r);
    cdrom->xa_playing = psx_sr_i32(r);
    cdrom->xa_mute = psx_sr_i32(r);
    cdrom->xa_channel = psx_sr_i32(r);
    cdrom->xa_file = psx_sr_i32(r);
    cdrom->xa_remaining_samples = psx_sr_i32(r);
    psx_sr_i16_array(r, cdrom->xa_left_h, 2);
    psx_sr_i16_array(r, cdrom->xa_right_h, 2);
    cdrom->xa_prev_left_sample = (int16_t)psx_sr_i32(r);
    cdrom->xa_prev_right_sample = (int16_t)psx_sr_i32(r);
    cdrom->xa_sample_index = psx_sr_i32(r);

    cdrom->state = psx_sr_i32(r);
    cdrom->prev_state = psx_sr_i32(r);
    cdrom->int1_pending = psx_sr_i32(r);
    cdrom->int2_pending = psx_sr_i32(r);
    cdrom->delay = psx_sr_i32(r);
    cdrom->pending_lba = psx_sr_u32(r);
    cdrom->lba = psx_sr_u32(r);

    psx_sr_i16_array(r, cdrom->cdda_buf, CD_SECTOR_SIZE >> 1);
    cdrom->cdda_remaining_samples = psx_sr_i32(r);
    cdrom->cdda_sample_index = psx_sr_u32(r);
    cdrom->cdda_sectors_played = psx_sr_u32(r);
    cdrom->cdda_playing = psx_sr_i32(r);
    cdrom->cdda_prev_track = psx_sr_i32(r);
    cdrom->read_ongoing = psx_sr_i32(r);

    psx_sr_bytes(r, cdrom->xa_buf, CD_SECTOR_SIZE);
    psx_sr_i16_array(r, cdrom->xa_left_buf, XA_STEREO_SAMPLES);
    psx_sr_i16_array(r, cdrom->xa_right_buf, XA_STEREO_SAMPLES);
    psx_sr_i16_array(r, cdrom->xa_mono_buf, XA_MONO_SAMPLES);
    psx_sr_i16_array(r, cdrom->xa_upsample_buf, XA_UPSAMPLE_SIZE);
    psx_sr_i16_array(r, cdrom->xa_left_resample_buf, XA_STEREO_RESAMPLE_MAX_SIZE);
    psx_sr_i16_array(r, cdrom->xa_right_resample_buf, XA_STEREO_RESAMPLE_MAX_SIZE);
    psx_sr_i16_array(r, cdrom->xa_mono_resample_buf, XA_MONO_RESAMPLE_MAX_SIZE);

    /* Derived, never serialised: a state written by any build (including every state already
       on a device) restores a head sitting exactly on the transport position, which is where
       a drive that has just been told to seek there would be. */
    cdrom_anchor_report_position(cdrom, cdrom->lba);

    return PSX_STATE_OK;
}
