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

psx_cdrom_t* psx_cdrom_create(void) {
    return malloc(sizeof(psx_cdrom_t));
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
    cdrom->ifr = n;
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

    cdrom_set_int(cdrom, 1);
    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    // Read sector into our data FIFO
    psx_disc_read(cdrom->disc, cdrom->lba, cdrom->data->buf);

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

void psx_cdrom_update(psx_cdrom_t* cdrom, int cycles) {
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
            cdrom_handle_resp1(cdrom);

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
                cdrom->delay = CD_DELAY_ONGOING_READ;
            }
        } break;

        case CD_STATE_TX_RESP2: {
            cdrom_cmd_table[cdrom->pending_command](cdrom);

            // Switching to read mode after executing a command
            // has a 500ms penalty
            if (cdrom->state == CD_STATE_READ) {
                cdrom_process_setloc(cdrom);

                cdrom->state = CD_STATE_READ;
                cdrom->prev_state = CD_STATE_READ;
                cdrom->delay = CD_DELAY_ONGOING_READ;
            }
        } break;

        case CD_STATE_READ: {
            cdrom_handle_read(cdrom);
        } break;
    }

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

    printf("cdrom: %-16s (%02x) params: ", cdrom_cmd_names[data], data);

    if (queue_is_empty(cdrom->parameters)) {
        puts("(none)");

        return;
    }

    for (int i = 0; i < cdrom->parameters->write_index; i++)
        printf("%02x ", cdrom->parameters->buf[i]);

    putchar('\n');

    return;
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

    return PSX_STATE_OK;
}
