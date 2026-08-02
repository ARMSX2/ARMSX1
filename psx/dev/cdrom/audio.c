#include <stdlib.h>
#include <string.h>

#include "cdrom.h"
#include "../spu.h"
#include "../../perf.h"

#define ITOB(b) itob_table[b]

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

static const int pos_adpcm_table[] = {
    0, +60, +115, +98, +122
};

static const int neg_adpcm_table[] = {
    0,   0,  -52, -55,  -60
};

/*
    CD audio volume (the ATV matrix, 1F801802h/1F801803h index 2 and 3).

    80h is UNITY, not "half of FFh": the hardware applies these as (sample * volume) >> 7, so
    the divisor is 128 and values above 80h are legal gains up to just under 2x. Dividing by
    255 made 80h come out as 0.502 and cost a flat 6.02 dB on everything arriving from the
    disc — the same class of error as the SPU main volume, and corroborated by this file's own
    reset defaults of 80h/00h/80h/00h, which are meant to be "unity, no cross-feed".

    Not measured on this game: all three MGS captures show XA and CDDA completely inactive, so
    the CD path never carried a sample. It is fixed anyway because leaving it would put the CD
    path 6 dB BELOW the SPU once the main-volume fix lands, turning a uniform error into a
    balance error between the two halves of the same mix.

    Because unity is now reachable and cross-feed can legitimately sum two terms, the stores
    have to saturate: they are float-to-int16 conversions, which are undefined out of range.
*/
static inline int16_t cdrom_vol_clamp(float v) {
    if (v > 32767.0f)
        return (int16_t)32767;

    if (v < -32768.0f)
        return (int16_t)-32768;

    return (int16_t)v;
}

#define CD_VOL_UNITY 128.0f

void cdrom_resample_xa_buf(psx_cdrom_t* cdrom, int16_t* dst, int16_t* src, int stereo, int16_t ls) {
    int f18khz = ((cdrom->xa_buf[0x13] >> 2) & 1) == 1;
    int sample_count = stereo ? XA_STEREO_SAMPLES : XA_MONO_SAMPLES;
    int resample_count = stereo ? XA_STEREO_RESAMPLE_SIZE : XA_MONO_RESAMPLE_SIZE;

    resample_count *= f18khz + 1;

    // Nearest neighbor
    // for (int i = 0; i < sample_count; i++)
    //     for (int k = 0; k < 7; k++)
    //         cdrom->xa_upsample_buf[(i*7)+k] = src[i];

    /* Linear Upsampling.
       `(k+1)/8` was INTEGER division of two ints: for k = 0..6 the numerator is 1..7, so the
       quotient was 0 every single time and the whole weighted term vanished. Every one of the
       seven interpolated points therefore came out as plain `a` — the interpolator degenerated
       into a zero-order hold of the PREVIOUS input sample, which is the commented-out nearest
       neighbour above with an extra sample of lag. Multiplying before dividing is the fix, and
       it keeps the author's 1/8..7/8 phases exactly as written rather than re-deriving them.

       Cannot overflow: (k+1)/8 is in (0,1), so the result is a convex combination of a and b
       and stays inside their range, hence inside int16. */
    int16_t a = ls;
    int16_t b = src[0];

    for (int k = 0; k < 7; k++)
        cdrom->xa_upsample_buf[k] = a + (((k+1) * (b - a)) / 8);

    for (int i = 1; i < sample_count; i++) {
        a = b;
        b = src[i];

        for (int k = 0; k < 7; k++)
            cdrom->xa_upsample_buf[(i*7)+k] =
                a + (((k+1) * (b - a)) / 8);
    }

    int m = f18khz ? 3 : 6;

    for (int i = 0; i < resample_count; i++)
        dst[i] = cdrom->xa_upsample_buf[i*m];

    cdrom->xa_remaining_samples = resample_count;
}

void cdrom_decode_xa_block(psx_cdrom_t* cdrom, int idx, int blk, int nib, int16_t* buf, int16_t* h) {
    int shift  = 12 - (cdrom->xa_buf[idx + 4 + blk * 2 + nib] & 0x0F);
    int filter =      (cdrom->xa_buf[idx + 4 + blk * 2 + nib] & 0x30) >> 4;

    int32_t f0 = pos_adpcm_table[filter];
    int32_t f1 = neg_adpcm_table[filter];

    for (int j = 0; j < 28; j++) {
        uint16_t n = (cdrom->xa_buf[idx + 16 + blk + j * 4] >> (nib * 4)) & 0x0f;

        int16_t t = (int16_t)(n << 12) >> 12;

        /* Same dead saturation as psx/dev/spu.c's spu_read_block() had: clamping an int16_t
           against the int16 limits is unreachable by construction, so the compiler dropped it
           and out-of-range predictions wrapped sign instead of clipping. XA carries the voice
           acting in games that stream it (Silent Hill), so this is the same defect on the other
           audio path. Accumulate in int32, clamp, then narrow. */
        int32_t acc = (t << shift) + (((h[0] * f0) + (h[1] * f1) + 32) / 64);

        int16_t s = (acc < INT16_MIN) ? INT16_MIN
                                      : ((acc > INT16_MAX) ? INT16_MAX : (int16_t)acc);

        h[1] = h[0];
        h[0] = s;

        buf[j] = s;
    }
}

void cdrom_decode_xa_sector(psx_cdrom_t* cdrom, void* buf) {
    int src = 24;

    int16_t left[28];
    int16_t right[28];

    int16_t* left_ptr = cdrom->xa_left_buf;
    int16_t* right_ptr = cdrom->xa_right_buf;
    int16_t* mono_ptr = cdrom->xa_mono_buf;

    for (int i = 0; i < 18; i++) {
        for (int blk = 0; blk < 4; blk++) {
            if (cdrom->xa_buf[0x13] & 1) {
                cdrom_decode_xa_block(cdrom, src, blk, 0, left, cdrom->xa_left_h);
                cdrom_decode_xa_block(cdrom, src, blk, 1, right, cdrom->xa_right_h);

                for (int i = 0; i < 28; i++) {
                    *left_ptr++ = left[i];
                    *right_ptr++ = right[i];
                }
            } else {
                cdrom_decode_xa_block(cdrom, src, blk, 0, left, cdrom->xa_left_h);

                for (int i = 0; i < 28; i++)
                    *mono_ptr++ = left[i];

                cdrom_decode_xa_block(cdrom, src, blk, 1, left, cdrom->xa_left_h);

                for (int i = 0; i < 28; i++)
                    *mono_ptr++ = left[i];
            }
        }

        src += 128;
    }
}

/* `audio_diag`: one entry per sector DECISION — accepted, rejected, walked past or aborted on.
   This is the only place the raw XA framing is visible, and recording the verdict alongside the
   filter that produced it is what makes "the music plays but the dialogue does not" answerable:
   two XA streams share one track, so the question is never "did XA arrive" but "which channel
   arrived, and what did the fetcher decide about it". */
static void cdrom_xa_diag_push(psx_cdrom_t* cdrom, uint32_t lba, int verdict) {
    uint32_t slot;

    if (!g_psx_audio_diag_enabled)
        return;

    slot = g_psx_audio_diag.xa_hdr_head;

    g_psx_audio_diag.xa_hdr[slot].lba = lba;
    g_psx_audio_diag.xa_hdr[slot].file = cdrom->xa_buf[0x10];
    g_psx_audio_diag.xa_hdr[slot].chan = cdrom->xa_buf[0x11];
    g_psx_audio_diag.xa_hdr[slot].submode = cdrom->xa_buf[0x12];
    g_psx_audio_diag.xa_hdr[slot].coding = cdrom->xa_buf[0x13];
    g_psx_audio_diag.xa_hdr[slot].filter_file = (uint8_t)cdrom->xa_file;
    g_psx_audio_diag.xa_hdr[slot].filter_chan = (uint8_t)cdrom->xa_channel;
    g_psx_audio_diag.xa_hdr[slot].filter_on = (cdrom->mode & MODE_XA_FILTER) ? 1u : 0u;
    g_psx_audio_diag.xa_hdr[slot].verdict = (uint8_t)verdict;

    g_psx_audio_diag.xa_hdr_head = (slot + 1u) % PSX_AUDIO_DIAG_XA_HDRS;
    g_psx_audio_diag.xa_hdr_seen++;
}

int cdrom_fetch_xa_sector(psx_cdrom_t* cdrom) {
    uint32_t walked = 0;

    while (1) {
        int ts = psx_disc_read(cdrom->disc, cdrom->xa_lba, cdrom->xa_buf);

        if (ts == TS_FAR) {
            /* No sub-header to record: xa_buf still holds the PREVIOUS sector, so pushing one
               here would log a sector that was never read. Counter only. */
            if (g_psx_audio_diag_enabled)
                g_psx_audio_diag.xa_stop_far++;

            return 0;
        }

        if (cdrom->xa_buf[0x12] & 1) {
            /* Submode bit0 is End-Of-Record, not End-Of-File (that is bit7), and this test sits
               AHEAD of both the audio-bit test and the filter test — so a sector merely being
               walked past on the way to the wanted channel can abandon XA playback outright.
               Recorded rather than changed: the verdict log says whether this is what silences
               a codec call, and the entry carries the filter that was being searched for. */
            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.xa_stop_eor++;
                cdrom_xa_diag_push(cdrom, cdrom->xa_lba, PSX_XA_VERDICT_STOP_EOR);
            }

            return 0;
        }

        ++cdrom->xa_lba;

        // Check Audio bit
        if (!(cdrom->xa_buf[0x12] & 4)) {
            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.xa_skip_nonaudio++;
                cdrom_xa_diag_push(cdrom, cdrom->xa_lba - 1u, PSX_XA_VERDICT_SKIP_NONAUDIO);
            }

            ++walked;

            continue;
        }

        // If we get here it means this is a real-time audio sector.
        // If the XA filter is disabled, we're done
        if (!(cdrom->mode & MODE_XA_FILTER)) {
            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.xa_sectors++;
                cdrom_xa_diag_push(cdrom, cdrom->xa_lba - 1u,
                                   PSX_XA_VERDICT_ACCEPT_NOFILTER);

                if (walked > g_psx_audio_diag.xa_walk_peak)
                    g_psx_audio_diag.xa_walk_peak = walked;
            }

            return 1;
        }

        // printf("fetch_xa_sector: lba=%u %02x:%02x:%02x mode=%02x file=%02x channel=%02x sm=%02x ci=%02x xafile=%02x xachannel=%02x\n",
        //     cdrom->xa_lba,
        //     cdrom->xa_buf[0x0c],
        //     cdrom->xa_buf[0x0d],
        //     cdrom->xa_buf[0x0e],
        //     cdrom->xa_buf[0x0f],
        //     cdrom->xa_buf[0x10],
        //     cdrom->xa_buf[0x11],
        //     cdrom->xa_buf[0x12],
        //     cdrom->xa_buf[0x13],
        //     cdrom->xa_file,
        //     cdrom->xa_channel
        // );

        // Else check XA file/channel
        int file_eq = cdrom->xa_buf[0x10] == cdrom->xa_file;
        int channel_eq = cdrom->xa_buf[0x11] == cdrom->xa_channel;

        // If they are equal to our filter values, we're done
        // else keep searching
        if (file_eq && channel_eq) {
            if (g_psx_audio_diag_enabled) {
                g_psx_audio_diag.xa_sectors++;
                cdrom_xa_diag_push(cdrom, cdrom->xa_lba - 1u, PSX_XA_VERDICT_ACCEPT_MATCH);

                if (walked > g_psx_audio_diag.xa_walk_peak)
                    g_psx_audio_diag.xa_walk_peak = walked;
            }

            return 1;
        }

        if (g_psx_audio_diag_enabled) {
            g_psx_audio_diag.xa_filter_rejects++;
            cdrom_xa_diag_push(cdrom, cdrom->xa_lba - 1u, PSX_XA_VERDICT_REJECT_FILTER);
        }

        ++walked;
    }
}

int cdrom_get_xa_samples(psx_cdrom_t* cdrom, void* buf, size_t size) {
    if ((!cdrom->xa_playing) || !(cdrom->mode & MODE_XA_ADPCM)) {
        cdrom->xa_remaining_samples = 0;
        cdrom->xa_sample_index = 0;

        memset(buf, 0, size);

        return 0;
    }

    float ll_vol = (((float)cdrom->vol[0]) / CD_VOL_UNITY);
    float lr_vol = (((float)cdrom->vol[1]) / CD_VOL_UNITY);
    float rr_vol = (((float)cdrom->vol[2]) / CD_VOL_UNITY);
    float rl_vol = (((float)cdrom->vol[3]) / CD_VOL_UNITY);

    int16_t* ptr = (int16_t*)buf;

    for (int i = 0; i < (size >> 2); i++) {
        int stereo = (cdrom->xa_buf[0x13] & 1) == 1;

        if (!cdrom->xa_remaining_samples) {
            if (!cdrom_fetch_xa_sector(cdrom)) {
                cdrom->xa_playing = 0;
                cdrom->xa_remaining_samples = 0;

                /* Returning 0 here abandons the rest of `buf` AND makes the caller fall
                   through to the CDDA path, so a starve is audible twice over. */
                if (g_psx_audio_diag_enabled)
                    g_psx_audio_diag.xa_starved++;

                return 0;
            }

            stereo = (cdrom->xa_buf[0x13] & 1) == 1;

            cdrom_decode_xa_sector(cdrom, buf);

            if (stereo) {
                cdrom_resample_xa_buf(
                    cdrom,
                    cdrom->xa_left_resample_buf,
                    cdrom->xa_left_buf,
                    stereo,
                    cdrom->xa_prev_left_sample
                );

                cdrom_resample_xa_buf(
                    cdrom,
                    cdrom->xa_right_resample_buf,
                    cdrom->xa_right_buf,
                    stereo,
                    cdrom->xa_prev_right_sample
                );
            } else {
                cdrom_resample_xa_buf(
                    cdrom,
                    cdrom->xa_mono_resample_buf,
                    cdrom->xa_mono_buf,
                    stereo,
                    cdrom->xa_prev_left_sample
                );
            }

            cdrom->xa_sample_index = 0;
        }

        if (cdrom->xa_mute || cdrom->mute) {
            *ptr++ = 0;
            *ptr++ = 0;

            --cdrom->xa_remaining_samples;

            continue;
        }

        if (stereo) {
            cdrom->xa_prev_left_sample = cdrom->xa_left_resample_buf[cdrom->xa_sample_index];
            cdrom->xa_prev_right_sample = cdrom->xa_right_resample_buf[cdrom->xa_sample_index++];

            *ptr++ = cdrom_vol_clamp((cdrom->xa_prev_left_sample * ll_vol) +
                                     (cdrom->xa_prev_right_sample * rl_vol));
            *ptr++ = cdrom_vol_clamp((cdrom->xa_prev_left_sample * lr_vol) +
                                     (cdrom->xa_prev_right_sample * rr_vol));

        } else {
            cdrom->xa_prev_left_sample = cdrom->xa_mono_resample_buf[cdrom->xa_sample_index++];

            *ptr++ = cdrom_vol_clamp(cdrom->xa_prev_left_sample * ll_vol);
            *ptr++ = cdrom_vol_clamp(cdrom->xa_prev_left_sample * rr_vol);
        }

        /* `audio_diag`: the level XA actually reaches the host buffer at, after cdrom->vol[].
           "You cannot hear the dialogue" is either a zero here or a stream that never arrives,
           and the two need completely different fixes. */
        if (g_psx_audio_diag_enabled) {
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.xa_peak_l, ptr[-2]);
            PSX_AUDIO_DIAG_PEAK(g_psx_audio_diag.xa_peak_r, ptr[-1]);
        }

        --cdrom->xa_remaining_samples;
    }

    return 1;
}

void cdrom_send_autopause_irq(psx_cdrom_t* cdrom) {
    cdrom_set_int(cdrom, 4);

    queue_push(cdrom->response, cdrom_get_stat(cdrom));

    psx_ic_irq(cdrom->ic, IC_CDROM);
}

int cdrom_reload_cdda_buffer(psx_cdrom_t* cdrom, void* buf, size_t size) {
    int ts = psx_disc_read(cdrom->disc, cdrom->lba, cdrom->cdda_buf);

    // We hit the end of the disc
    if (ts == TS_FAR) {
        cdrom->cdda_remaining_samples = 0;
        cdrom->cdda_sample_index = 0;
        cdrom->state = CD_STATE_IDLE;
        cdrom->cdda_prev_track = 0;
        cdrom->cdda_playing = 0;

        memset(buf, 0, size);

        if (cdrom->mode & MODE_AUTOPAUSE)
            cdrom_send_autopause_irq(cdrom);

        return 0;
    }

    if (g_psx_audio_diag_enabled)
        g_psx_audio_diag.cdda_sectors++;

    cdrom->cdda_remaining_samples = CD_SECTOR_SIZE >> 1;
    cdrom->cdda_sample_index = 0;

    cdrom->pending_lba = ++cdrom->lba;

    return 1;
}

void cdrom_send_report_irq(psx_cdrom_t* cdrom) {
    if (!(cdrom->mode & MODE_REPORT))
        return;

    cdrom_set_int(cdrom, 1);

    int track = psx_disc_get_track_number(cdrom->disc, cdrom->lba);
    int track_lba = psx_disc_get_track_lba(cdrom->disc, track);

    // Sense a track change
    if ((track != cdrom->cdda_prev_track) && (cdrom->mode & MODE_AUTOPAUSE)) {
        cdrom->cdda_remaining_samples = 0;
        cdrom->cdda_sample_index = 0;
        cdrom->state = CD_STATE_IDLE;
        cdrom->cdda_prev_track = 0;
        cdrom->cdda_playing = 0;

        cdrom_send_autopause_irq(cdrom);

        return;
    } else {
        cdrom->cdda_prev_track = track;
    }

    int relative = (cdrom->cdda_sectors_played & 0x10) != 0;

    int32_t diff = cdrom->lba;

    if (relative)
        diff = cdrom->lba - track_lba;

    if (diff < 0)
        diff = -diff;

    int mm = diff / (60 * 75);
    int ss = (diff % (60 * 75)) / 75;
    int ff = (diff % (60 * 75)) % 75;

    printf("report: track %u %02u:%02u:%02u relative=%d\n",
        track,
        mm, ss, ff,
        relative
    );

    queue_push(cdrom->response, cdrom_get_stat(cdrom));
    queue_push(cdrom->response, ITOB(track));
    queue_push(cdrom->response, 1);
    queue_push(cdrom->response, ITOB(mm));
    queue_push(cdrom->response, ITOB(ss) | (relative ? 0x80 : 0));
    queue_push(cdrom->response, ITOB(ff));
    queue_push(cdrom->response, 0);
    queue_push(cdrom->response, 0);

    psx_ic_irq(cdrom->ic, IC_CDROM);
}

void psx_cdrom_get_audio_samples(psx_cdrom_t* cdrom, void* buf, size_t size) {
    if (!cdrom->disc)
        return;

    if (cdrom_get_xa_samples(cdrom, buf, size))
        return;

    if (!cdrom->cdda_playing) {
        cdrom->cdda_remaining_samples = 0;
        cdrom->cdda_sample_index = 0;

        return;
    }

    int16_t* ptr = buf;

    float ll_vol = (((float)cdrom->vol[0]) / CD_VOL_UNITY);
    float lr_vol = (((float)cdrom->vol[1]) / CD_VOL_UNITY);
    float rr_vol = (((float)cdrom->vol[2]) / CD_VOL_UNITY);
    float rl_vol = (((float)cdrom->vol[3]) / CD_VOL_UNITY);

    for (int i = 0; i < (size >> 1);) {
        if (!cdrom->cdda_remaining_samples) {
            if (!cdrom_reload_cdda_buffer(cdrom, buf, size))
                return;

            ++cdrom->cdda_sectors_played;

            if ((cdrom->cdda_sectors_played & 0xf) == 0) {
                cdrom_send_report_irq(cdrom);

                cdrom->cdda_sectors_played &= 0x3f;
            }
        }

        if (cdrom->mute) {
            ptr[i++] = 0;
            ptr[i++] = 0;

            cdrom->cdda_remaining_samples -= 2;
            cdrom->cdda_sample_index += 2;

            continue;
        }

        int16_t left = cdrom->cdda_buf[cdrom->cdda_sample_index++];
        int16_t right = cdrom->cdda_buf[cdrom->cdda_sample_index++];

        // Apply volume settings to CDDA
        ptr[i++] = cdrom_vol_clamp(left  * ll_vol + right * rl_vol);
        ptr[i++] = cdrom_vol_clamp(right * rr_vol + left  * lr_vol);

        cdrom->cdda_remaining_samples -= 2;
    }
}