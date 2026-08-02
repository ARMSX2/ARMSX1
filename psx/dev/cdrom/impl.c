#include "cdrom.h"

#define BTOI(b) btoi_table[b]
#define ITOB(b) itob_table[b]

#define VALID_BCD(bcd) \
    (((bcd & 0xf0) <= 0x90) && ((bcd & 0xf) <= 9))

#define VALID_MSF(m, s, f) \
    (VALID_BCD(m) && VALID_BCD(s) && VALID_BCD(f) && (f < 0x75) && (s < 0x60))

static const uint8_t btoi_table[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
    0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
    0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41,
    0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43,
    0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d,
    0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61,
    0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
    0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73,
    0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d,
    0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91,
    0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d,
    0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
};

static const uint8_t itob_table[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0xa0, 0xa1, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xb0, 0xb1,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xe0, 0xe1, 0xe2, 0xe3,
    0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xf0, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
};

static const uint8_t cdrom_cd_getid[] = {
    0x08, 0x40, 0x00, 0x00, // No Disk
    0x02, 0x00, 0x20, 0x00, // Licensed
    0x0a, 0x90, 0x00, 0x00, // Audio Disk
    0x0a, 0x80, 0x00, 0x00  // Unlicensed
};

static const uint8_t cdrom_version_id[] = {
    0x94, 0x09, 0x19, 0x01, // DTL-H2000 (date unknown)
    0x94, 0x09, 0x19, 0xc0,
    0x94, 0x11, 0x18, 0xc0,
    0x95, 0x05, 0x16, 0xc1,
    0x95, 0x07, 0x24, 0xc1,
    0x95, 0x07, 0x24, 0xd1,
    0x96, 0x08, 0x15, 0xc2,
    0x96, 0x08, 0x18, 0xc1,
    0x96, 0x09, 0x12, 0xc2,
    0x97, 0x01, 0x10, 0xc2,
    0x97, 0x08, 0x14, 0xc2,
    0x98, 0x06, 0x10, 0xc3,
    0x99, 0x02, 0x01, 0xc3,
    0xa1, 0x03, 0x06, 0xc3
};

static const char cdrom_region_letter[] = {
    'I', 'E', 'A'
};

void cdrom_pause(psx_cdrom_t* cdrom) {
    cdrom->prev_state = CD_STATE_IDLE;
    cdrom->state = CD_STATE_IDLE;
    cdrom->pending_command = 0;
    cdrom->busy = 0;
    cdrom->cdda_playing = 0;
    cdrom->xa_playing = 0;
    cdrom->read_ongoing = 0;
}

void cdrom_restore_state(psx_cdrom_t* cdrom) {
    cdrom->state = CD_STATE_IDLE;

    if (cdrom->prev_state == CD_STATE_PLAY ||
        cdrom->prev_state == CD_STATE_READ)
        cdrom->state = cdrom->prev_state;

    cdrom->pending_command = 0;
}

void cdrom_cmd_getstat(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_setloc(psx_cdrom_t* cdrom) {
    int m = queue_pop(cdrom->parameters);
    int s = queue_pop(cdrom->parameters);
    int f = queue_pop(cdrom->parameters);

    if (!VALID_MSF(m, s, f)) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE,
            CD_ERR_INVALID_SUBFUNCTION
        );

        return;
    }

    cdrom_set_int(cdrom, 3);
    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->pending_lba = (BTOI(m) * 4500) + (BTOI(s) * 75) + BTOI(f);

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_play(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom_process_setloc(cdrom);

    int track = -1;

    if (!queue_is_empty(cdrom->parameters)) {
        int track = BTOI(queue_pop(cdrom->parameters));

        if (track)
            cdrom->lba = psx_disc_get_track_lba(cdrom->disc, track);
    } else {
        track = psx_disc_get_track_number(cdrom->disc, cdrom->lba);
    }

    int mm = cdrom->lba / (60 * 75);
    int ss = (cdrom->lba % (60 * 75)) / 75;
    int ff = (cdrom->lba % (60 * 75)) % 75;

    printf("play song at lba=%08x %02u:%02u:%02u track=%d\n", cdrom->lba, mm, ss, ff, track);

    cdrom->prev_state = CD_STATE_PLAY;
    cdrom->state = CD_STATE_PLAY;
    cdrom->pending_command = 0;
    cdrom->cdda_playing = 1;
    cdrom->cdda_remaining_samples = 0;
    cdrom->cdda_sample_index = 0;
    cdrom->cdda_prev_track = track;

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_forward(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->lba += 35;

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_backward(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->lba -= 35;

    if (cdrom->lba < 150)
        cdrom->lba = 150;

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_readn(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));
    
    cdrom_process_setloc(cdrom);

    // Preload sector
    int ts = psx_disc_read(cdrom->disc, cdrom->lba, cdrom->data->buf);

    if (cdrom->mode & MODE_XA_ADPCM) {
        cdrom->xa_playing = 1;
        cdrom->xa_remaining_samples = 0;
        cdrom->xa_sample_index = 0;
        cdrom->xa_lba = cdrom->lba;
    }

    cdrom->state = CD_STATE_READ;
    cdrom->prev_state = CD_STATE_READ;
    cdrom->delay = CD_DELAY_START_READ;
    cdrom->read_ongoing = 1;
}

void cdrom_cmd_motoron(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_FR;
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        cdrom_set_int(cdrom, 2);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        // Doesn't pause!
        cdrom_restore_state(cdrom);
    }
}

void cdrom_cmd_stop(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_FR;
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        cdrom_set_int(cdrom, 2);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        // Reset to 00:02:00
        cdrom->pending_lba = 150;

        cdrom_process_setloc(cdrom);

        cdrom_pause(cdrom);
    }
}

void cdrom_cmd_pause(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        // Pausing at 1x takes 70ms, 2x takes 35ms
        cdrom->delay = cdrom_get_pause_delay(cdrom);
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        cdrom_set_int(cdrom, 2);

        queue_push(cdrom->response, CD_STAT_SPINDLE);

        cdrom_pause(cdrom);
    }
}

void cdrom_cmd_init(psx_cdrom_t* cdrom) {
    // Init sends the "same" thing twice. On real hardware 
    // it would probably send something different, but that's
    // not really important.
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_1MS;
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        cdrom_set_int(cdrom, 2);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom_pause(cdrom);
    }
}

void cdrom_cmd_mute(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->mute = 1;

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_demute(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->mute = 0;

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_setfilter(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom->xa_file = queue_pop(cdrom->parameters);
    cdrom->xa_channel = queue_pop(cdrom->parameters);

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_setmode(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    int prev_speed = cdrom->mode & MODE_SPEED;

    cdrom->mode = queue_pop(cdrom->parameters);

    // Big speed switch delay
    if (prev_speed != (cdrom->mode & MODE_SPEED))
        cdrom->pending_speed_switch_delay = 650 * CD_DELAY_1MS;

    /*
        Setmode has NO transport effect. It used to call cdrom_pause(), which clears
        xa_playing, cdda_playing, read_ongoing, busy AND prev_state — so an in-progress read
        was not merely stopped, it became unrecoverable, because prev_state is the only thing
        cdrom_restore_state() can resume from.

        This silenced XA-ADPCM audio in every game that reconfigures the drive mid-stream,
        which is the normal way to do it. Captured in Silent Hill 1's opening cutscene:

            cmd 0e CdlSetmode params=[80] mode_before=e0 xa=1 read=1 state=3   <- the kill
            cmd 0e CdlSetmode params=[a0] mode_before=80 xa=0 read=0 state=0   <- the aftermath

        mode_before=e0 has bit6 (XA-ADPCM) set and the drive was actively streaming; one
        Setmode later everything is zero. Same symptom in Metal Gear Solid's codec calls.

        cdrom_restore_state() is what the other 18 non-transport commands use — including
        Setfilter, the immediately adjacent opcode. It is sufficient on its own precisely
        because it CLEARS nothing: xa_playing / read_ongoing / cdda_playing simply keep their
        values, and state is restored from prev_state, after which cdrom_handle_resp1()'s
        caller sees CD_STATE_READ and reschedules the read with CD_DELAY_ONGOING_READ.

        Spec: `Command 0Eh - Setmode(mode)` returns INT3(stat) and nothing else; DuckStation's
        CommandSetmode sets the mode bits and acks. The genuine speed-change cost is already
        handled above by pending_speed_switch_delay.
    */
    cdrom_restore_state(cdrom);
}

void cdrom_cmd_getparam(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));
    queue_push(cdrom->response, cdrom->mode);
    queue_push(cdrom->response, 0);
    queue_push(cdrom->response, cdrom->xa_file);
    queue_push(cdrom->response, cdrom->xa_channel);

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_getlocl(psx_cdrom_t* cdrom) {
    // GetlocL only works while reading. This is
    // Somewhat of a hack, but should still work
    // if (queue_is_empty(cdrom->data) && !cdrom->fake_getlocl_data) {
    //     printf("getlocl error\n");
    //     cdrom_error(cdrom,
    //         CD_STAT_SPINDLE,
    //         CD_ERR_NO_DISC
    //     );

    //     return;
    // }

    cdrom->fake_getlocl_data = 0;

    // printf("getlocl: lba=%u %02x:%02x:%02x mode=%02x file=%02x channel=%02x sm=%02x ci=%02x\n",
    //     cdrom->lba,
    //     cdrom->data->buf[0x0c],
    //     cdrom->data->buf[0x0d],
    //     cdrom->data->buf[0x0e],
    //     cdrom->data->buf[0x0f],
    //     cdrom->data->buf[0x10],
    //     cdrom->data->buf[0x11],
    //     cdrom->data->buf[0x12],
    //     cdrom->data->buf[0x13]
    // );

    cdrom_set_int(cdrom, 3);
    queue_push(cdrom->response, cdrom->data->buf[0x0c]);
    queue_push(cdrom->response, cdrom->data->buf[0x0d]);
    queue_push(cdrom->response, cdrom->data->buf[0x0e]);
    queue_push(cdrom->response, cdrom->data->buf[0x0f]);
    queue_push(cdrom->response, cdrom->data->buf[0x10]);
    queue_push(cdrom->response, cdrom->data->buf[0x11]);
    queue_push(cdrom->response, cdrom->data->buf[0x12]);
    queue_push(cdrom->response, cdrom->data->buf[0x13]);

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_getlocp(psx_cdrom_t* cdrom) {
    int lba = cdrom->lba;
    int track = psx_disc_get_track_number(cdrom->disc, lba);
    int track_lba = psx_disc_get_track_lba(cdrom->disc, track);

    if (!cdrom->seek_precision)
        lba -= 25;

    uint8_t subq[12];
    if (lba >= 0 && psx_disc_read_subchannel_q(cdrom->disc, (uint32_t)lba, subq)) {
        cdrom_set_int(cdrom, 3);
        queue_push(cdrom->response, subq[1]);
        queue_push(cdrom->response, subq[2]);
        queue_push(cdrom->response, subq[3]);
        queue_push(cdrom->response, subq[4]);
        queue_push(cdrom->response, subq[5]);
        queue_push(cdrom->response, subq[7]);
        queue_push(cdrom->response, subq[8]);
        queue_push(cdrom->response, subq[9]);
        cdrom_restore_state(cdrom);
        return;
    }

    int32_t diff = lba - track_lba;

    if (diff < 0)
        diff = -diff;

    int rmm = diff / (60 * 75);
    int rss = (diff % (60 * 75)) / 75;
    int rff = (diff % (60 * 75)) % 75;
    int amm = lba / (60 * 75);
    int ass = (lba % (60 * 75)) / 75;
    int aff = (lba % (60 * 75)) % 75;

    int index = psx_disc_query(cdrom->disc, lba) != TS_PREGAP;

    // printf("getlocp: track %u (%02x) relative: %02u:%02u:%02u absolute: %02u:%02u:%02u pregap=%u\n",
    //     track, ITOB(track),
    //     rmm, rss, rff,
    //     amm, ass, aff,
    //     index
    // );

    cdrom_set_int(cdrom, 3);
    queue_push(cdrom->response, ITOB(track));
    queue_push(cdrom->response, ITOB(index));
    queue_push(cdrom->response, ITOB(rmm));
    queue_push(cdrom->response, ITOB(rss));
    queue_push(cdrom->response, ITOB(rff));
    queue_push(cdrom->response, ITOB(amm));
    queue_push(cdrom->response, ITOB(ass));
    queue_push(cdrom->response, ITOB(aff));

    cdrom_restore_state(cdrom);
}

// To-do: Implement SetSession errors
void cdrom_cmd_setsession(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_FR;
        cdrom->state = CD_STATE_TX_RESP2;
        cdrom->busy = 1;
    } else {
        cdrom_set_int(cdrom, 2);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom_pause(cdrom);
    }
}

void cdrom_cmd_gettn(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    int tn = psx_disc_get_track_count(cdrom->disc);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));
    queue_push(cdrom->response, 1);
    queue_push(cdrom->response, ITOB(tn));

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_gettd(psx_cdrom_t* cdrom) {
    int bcd = queue_pop(cdrom->parameters);

    // Check BCD
    if (!VALID_BCD(bcd)) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE,
            CD_ERR_INVALID_SUBFUNCTION
        );

        return;    
    }

    int track = BTOI(bcd);
    int f = psx_disc_get_track_lba(cdrom->disc, track);

    if (f == TS_FAR) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE,
            CD_ERR_INVALID_SUBFUNCTION
        );

        return;
    }

    cdrom_set_int(cdrom, 3);

    int mm = f / (60 * 75);
    int ss = (f % (60 * 75)) / 75;

    // printf("gettd: track %u lba=%08x (%u) %02u:%02u:%02u\n",
    //     track,
    //     f, f,
    //     mm, ss, ff
    // );

    queue_push(cdrom->response, cdrom_get_stat(cdrom));
    queue_push(cdrom->response, ITOB(mm));
    queue_push(cdrom->response, ITOB(ss));

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_seekl(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_1MS;
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        int ts = psx_disc_query(cdrom->disc, cdrom->pending_lba);

        if (ts == TS_FAR) {
            cdrom_error(cdrom,
                CD_STAT_SPINDLE | CD_STAT_SEEKERROR,
                CD_ERR_INVALID_SUBFUNCTION
            );

            return;
        }

        if (ts == TS_AUDIO) {
            cdrom_error(cdrom,
                CD_STAT_SPINDLE | CD_STAT_SEEKERROR,
                CD_ERR_SEEK_FAILED
            );

            return;
        }

        // If everything is right, then seek is successful
        cdrom_process_setloc(cdrom);
        cdrom->seek_precision = 1;

        cdrom_set_int(cdrom, 2);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom_restore_state(cdrom);
    }
}

void cdrom_cmd_seekp(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_1MS;
        cdrom->state = CD_STATE_TX_RESP2;
        cdrom->busy = 1;
    } else {
        cdrom_set_int(cdrom, 2);
        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom_process_setloc(cdrom);
        cdrom->seek_precision = 0;

        cdrom_restore_state(cdrom);
    }
}

void cdrom_cmd_test(psx_cdrom_t* cdrom) {
    int subf = queue_pop(cdrom->parameters);

    // To-do: Handle other subfunctions (hard)
    // assert(subf == 32);
    if (subf == 4) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, CD_STAT_SPINDLE);

        cdrom_restore_state(cdrom);

        return;
    }

    if (subf == 5) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, 0);
        queue_push(cdrom->response, 0);

        cdrom_restore_state(cdrom);

        return;
    }

    if (subf != 32) {
        cdrom_error(cdrom,
            CD_STAT_SPINDLE,
            CD_ERR_INVALID_SUBFUNCTION
        );

        return;
    }

    int v = cdrom->version * 4;

    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_version_id[v+0]);
    queue_push(cdrom->response, cdrom_version_id[v+1]);
    queue_push(cdrom->response, cdrom_version_id[v+2]);
    queue_push(cdrom->response, cdrom_version_id[v+3]);

    cdrom_restore_state(cdrom);
}

void cdrom_cmd_getid(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->state = CD_STATE_TX_RESP2;
        cdrom->delay = CD_DELAY_FR;
    } else {
        cdrom_set_int(cdrom, 2);

        int t = cdrom->disc_type * 4;

        queue_push(cdrom->response, cdrom_cd_getid[t+0]);
        queue_push(cdrom->response, cdrom_cd_getid[t+1]);
        queue_push(cdrom->response, cdrom_cd_getid[t+2]);
        queue_push(cdrom->response, cdrom_cd_getid[t+3]);

        if (cdrom->disc_type == CDT_LICENSED) {
            queue_push(cdrom->response, 'S');
            queue_push(cdrom->response, 'C');
            queue_push(cdrom->response, 'E');
            queue_push(cdrom->response, cdrom_region_letter[cdrom->region]);
        } else {
            queue_push(cdrom->response, 0);
            queue_push(cdrom->response, 0);
            queue_push(cdrom->response, 0);
            queue_push(cdrom->response, 0);
        }

        cdrom_restore_state(cdrom);
    }
}

void cdrom_cmd_reads(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 3);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    cdrom_process_setloc(cdrom);

    // Preload sector
    int ts = psx_disc_read(cdrom->disc, cdrom->lba, cdrom->data->buf);

    if (cdrom->mode & MODE_XA_ADPCM) {
        cdrom->xa_playing = 1;
        cdrom->xa_remaining_samples = 0;
        cdrom->xa_sample_index = 0;
        cdrom->xa_lba = cdrom->lba;

        // printf("reads: xa_lba=%u (%08x) lba=%u (%08x) %02x:%02x:%02x mode=%02x file=%02x channel=%02x sm=%02x ci=%02x\n",
        //     cdrom->xa_lba, cdrom->xa_lba,
        //     cdrom->lba, cdrom->lba,
        //     cdrom->data->buf[0x0c],
        //     cdrom->data->buf[0x0d],
        //     cdrom->data->buf[0x0e],
        //     cdrom->data->buf[0x0f],
        //     cdrom->data->buf[0x10],
        //     cdrom->data->buf[0x11],
        //     cdrom->data->buf[0x12],
        //     cdrom->data->buf[0x13]
        // );
    }

    cdrom->state = CD_STATE_READ;
    cdrom->prev_state = CD_STATE_READ;
    cdrom->delay = CD_DELAY_START_READ;
    cdrom->read_ongoing = 1;
}

void cdrom_cmd_reset(psx_cdrom_t* cdrom) {
    printf("reset\n");
}

void cdrom_cmd_getq(psx_cdrom_t* cdrom) {

}

void cdrom_cmd_readtoc(psx_cdrom_t* cdrom) {
    if (cdrom->state == CD_STATE_TX_RESP1) {
        cdrom_set_int(cdrom, 3);

        queue_push(cdrom->response, cdrom_get_stat(cdrom));

        cdrom->delay = CD_DELAY_1MS * 500;
        cdrom->state = CD_STATE_TX_RESP2;
    } else {
        cdrom_set_int(cdrom, 2);

        queue_push(cdrom->response, CD_STAT_SPINDLE);

        cdrom_pause(cdrom);
    }
}

void cdrom_cmd_videocd(psx_cdrom_t* cdrom) {

}
