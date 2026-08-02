/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Multitap (SCPH-1070) emulator — four controllers on one
    controller port.

    ============================================================================
    HOW IT ANSWERS
    ============================================================================

    A multitap sits in a controller port and answers the pad poll itself, with a
    LONGER packet that carries all four sub-controllers at once:

        byte  0        FFh        hi-Z, the reply to the 01h address byte
        byte  1        80h        multitap ID low  (a plain pad answers 41h/73h)
        byte  2        5Ah        multitap ID high
        bytes 3..34    4 x 8      slots A, B, C, D, eight bytes each:

                                    id_lo id_hi(5Ah) sw_lo sw_hi a0 a1 a2 a3

                                  A digital pad has no ADC bytes, so its four
                                  trailing bytes are FFh — the block is always
                                  eight bytes so the packet has a fixed length.

    That 80h is how a game tells a tap from a pad. Which is also the catch worth
    knowing about: a game that does NOT understand multitaps sees an ID it does
    not recognise and reports NO CONTROLLER AT ALL. Real hardware has exactly the
    same problem, which is why a real SCPH-1070 has a physical 1-player/multitap
    switch — and why this is default-off and opt-in per user rather than
    something the front-end can turn on for them.

    ============================================================================
    WHAT IS NOT HERE
    ============================================================================

    MEMORY CARDS. A real tap also multiplexes four memory cards behind one port,
    addressed with 81h plus a slot byte. That path does not fall out of this
    implementation: psx/dev/pad.c owns the card destination (DEST_MCD) and holds
    exactly one psx_mcd_t per physical slot, so per-sub-slot cards would mean
    reworking the SIO destination handling, not adding a case here. Slots B/C/D
    therefore have no card of their own; slot A keeps the port's card as usual.
*/

#ifndef PSXI_MULTITAP_H
#define PSXI_MULTITAP_H

#include <stdint.h>

#include "../dev/input.h"
#include "../state.h"
#include "sda.h"

#define PSXI_MULTITAP_SLOTS 4
/* 3 header bytes + 4 slots x 8 bytes. */
#define PSXI_MULTITAP_PACKET_SIZE (3 + (PSXI_MULTITAP_SLOTS * 8))

#define PSXI_MULTITAP_ID_LO 0x80
#define PSXI_MULTITAP_ID_HI 0x5a

typedef struct {
    /* By VALUE, not by pointer, and deliberately: psx_input_destroy() does a
       plain free(input->udata), so anything the tap owned through a pointer
       would leak the moment a port is detached. */
    psxi_sda_t sub[PSXI_MULTITAP_SLOTS];

    uint8_t buf[PSXI_MULTITAP_PACKET_SIZE];
    /* Byte the next read returns. 0 = idle, and the packet is (re)built from the
       four sub-pads at that point, so every poll reports current buttons. */
    int pos;
    int tx_ready;
    /* Last command byte the host wrote (42h = read). Kept for save states and
       for the log line when something unsupported comes through. */
    uint8_t last_cmd;
} psxi_multitap_t;

psxi_multitap_t* psxi_multitap_create(void);
/* All four sub-pads start as digital DualShocks. */
void psxi_multitap_init(psxi_multitap_t*);
/* Same meaning as psxi_sda_set_analog_mode(), applied to every sub-pad — the
   front-end's [input] analog_mode_default is a property of "the pad", and a tap
   is four of them. */
void psxi_multitap_set_analog_mode(psxi_multitap_t*, int enabled);
void psxi_multitap_init_input(psxi_multitap_t*, psx_input_t*);

/* Per-player input. `slot` is 0..3 (A..D); anything else is dropped rather than
   folded onto player 1, so a routing bug shows up as a dead player instead of
   two players sharing a pad. */
void psxi_multitap_button_press(psxi_multitap_t*, int slot, uint32_t data);
void psxi_multitap_button_release(psxi_multitap_t*, int slot, uint32_t data);
void psxi_multitap_analog_change(psxi_multitap_t*, int slot, uint32_t axis, uint16_t data);

/* Transfer position plus the four sub-pads, saved inside the PAD section under
   PSX_INPUT_KIND_MULTITAP. */
void psxi_multitap_save_state(psxi_multitap_t*, psx_state_writer_t*);
int psxi_multitap_load_state(psxi_multitap_t*, psx_state_reader_t*);
void psxi_multitap_destroy(psxi_multitap_t*);

#endif
