#include <assert.h>
#include <string.h>
#include "../psx/dev/cdrom/cdrom.h"

void cdrom_cmd_getstat(psx_cdrom_t* cdrom);

int main(void) {
    psx_cdrom_t drive;
    memset(&drive, 0, sizeof(drive));
    drive.disc = (psx_disc_t*)&drive; // Status only; no image IO in this probe.
    drive.response = queue_create();
    queue_init(drive.response, 32);
    drive.shell_open_latched = 1;
    assert(cdrom_get_stat(&drive) & CD_STAT_SHELLOPEN);
    cdrom_cmd_getstat(&drive);
    assert(queue_pop(drive.response) & CD_STAT_SHELLOPEN);
    assert(!(cdrom_get_stat(&drive) & CD_STAT_SHELLOPEN));
    drive.disc = NULL;
    cdrom_cmd_getstat(&drive);
    assert(queue_pop(drive.response) & CD_STAT_SHELLOPEN);
    assert(cdrom_get_stat(&drive) & CD_STAT_SHELLOPEN);
    queue_destroy(drive.response);
    return 0;
}
