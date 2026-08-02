#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

#include "../psx/dev/cdrom/chd.c"

static int check(int condition, const char* message) {
    if (condition)
        return 0;
    fprintf(stderr, "CHD_LOGIC failed: %s\n", message);
    return 1;
}

int main(void) {
    int failed = 0;
    chd_track_mode_t track_mode;
    chd_subchannel_mode_t subchannel_mode;

    failed |= check(chd_parse_track_mode("AUDIO", &track_mode) &&
                    track_mode == CHD_TRACK_MODE_AUDIO, "AUDIO mode");
    failed |= check(chd_parse_track_mode("MODE2_RAW", &track_mode) &&
                    track_mode == CHD_TRACK_MODE_DATA, "MODE2_RAW mode");
    failed |= check(!chd_parse_track_mode("UNKNOWN", &track_mode), "unknown track mode");
    failed |= check(chd_parse_subchannel_mode("RW", &subchannel_mode) &&
                    subchannel_mode == CHD_SUBCHANNEL_COOKED, "cooked subchannel");
    failed |= check(chd_parse_subchannel_mode("RW_RAW", &subchannel_mode) &&
                    subchannel_mode == CHD_SUBCHANNEL_RAW, "raw subchannel");
    failed |= check(!chd_parse_subchannel_mode("BOGUS", &subchannel_mode),
                    "unknown subchannel mode");

    uint8_t raw[96] = {0};
    uint8_t cooked[96] = {0};
    raw[0] = 0x40;
    raw[8] = 0x40;
    chd_deinterleave_subchannel(raw, cooked);
    failed |= check(cooked[12] == 0x80 && cooked[13] == 0x80,
                    "raw P-W to cooked Q deinterleave");

    chd_track_t tracks[2] = {
        {
            .track_number = 1,
            .start_lba = 150,
            .pregap_frames = 150,
            .data_frames = 10,
            .postgap_frames = 2,
            .mode = CHD_TRACK_MODE_DATA,
        },
        {
            .track_number = 2,
            .start_lba = 162,
            .data_frames = 10,
            .mode = CHD_TRACK_MODE_AUDIO,
        },
    };
    chd_t image = {.tracks = tracks, .track_count = 2, .disc_lba_count = 172};
    int gap = 0;
    failed |= check(chd_find_track(&image, 0, &gap) == 0 && gap, "track 1 pregap");
    failed |= check(chd_find_track(&image, 150, &gap) == 0 && !gap, "track 1 data");
    failed |= check(chd_find_track(&image, 160, &gap) == 0 && gap, "track 1 postgap");
    failed |= check(chd_find_track(&image, 162, &gap) == 1 && !gap, "track 2 data");
    failed |= check(chd_find_track(&image, 172, &gap) < 0, "lead-out");

    if (failed)
        return 1;
    puts("CHD_LOGIC all cases passed");
    return 0;
}
