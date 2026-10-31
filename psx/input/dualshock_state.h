#ifndef PSXI_DUALSHOCK_STATE_H
#define PSXI_DUALSHOCK_STATE_H

#include "../dev/pad.h"

void psxi_dualshock_save_state(const psx_pad_t*, psx_state_writer_t*);
int psxi_dualshock_load_state(psx_pad_t*, psx_state_reader_t*, int apply);

#endif
