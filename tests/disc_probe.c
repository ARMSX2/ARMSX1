#include <stdio.h>
#include <stdlib.h>

#include "../psx/dev/cdrom/disc.h"

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: disc_probe image\n");
        return 2;
    }

    psx_disc_t* disc = psx_disc_create();
    if (!disc) {
        fprintf(stderr, "DISC_PROBE failed reason=allocation\n");
        return 1;
    }

    const int type = psx_disc_open(disc, argv[1]);
    if (type == CDT_ERROR) {
        fprintf(stderr, "DISC_PROBE failed reason=open\n");
        free(disc);
        return 1;
    }

    unsigned char sector[CD_SECTOR_SIZE];
    const int tracks = psx_disc_get_track_count(disc);
    const int sector_type = psx_disc_read(disc, 150, sector);
    if (tracks < 1 || sector_type == 0 || sector_type == TS_FAR) {
        fprintf(stderr, "DISC_PROBE failed reason=sector-map tracks=%d sector=%d\n",
                tracks, sector_type);
        psx_disc_destroy(disc);
        return 1;
    }

    printf("DISC_PROBE passed disc_type=%d tracks=%d sector150=%d\n",
           type, tracks, sector_type);
    psx_disc_destroy(disc);
    return 0;
}
