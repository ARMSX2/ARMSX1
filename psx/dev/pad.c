#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pad.h"
#include "../log.h"

#define JOY_IRQ_DELAY 512

#define PAD_TRACE_INTERVAL_CYCLES 33868800u
#define PAD_TRACE_COMMANDS_PER_LINE 16

static const char* pad_trace_device_name(uint8_t dev) {
    switch (dev) {
        case DEST_JOY: return "JOY";
        case DEST_MCD: return "MCD";
    }

    return "???";
}

static void pad_trace_flush(psx_pad_t* pad) {
    const int enabled = log_get_level() <= LOG_DEBUG;
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned dev = 0; dev < 2; ++dev) {
            psx_pad_trace_t* trace = &pad->trace[slot][dev];
            if (!trace->transactions)
                continue;
            if (enabled) {
                char commands[512];
                size_t used = 0;
                unsigned shown = 0, omitted = 0;
                uint32_t incomplete = 0, other_total = 0, other_incomplete = 0;
                commands[0] = 0;
                for (unsigned cmd = 0; cmd < 256; ++cmd) {
                    incomplete += trace->incomplete[cmd];
                    if (!trace->commands[cmd])
                        continue;
                    if (shown < PAD_TRACE_COMMANDS_PER_LINE) {
                        used += (size_t)snprintf(commands + used, sizeof(commands) - used,
                            "%s%02x:%u/%u", shown ? " " : "", cmd,
                            trace->commands[cmd], trace->incomplete[cmd]);
                        ++shown;
                    } else {
                        ++omitted;
                        other_total += trace->commands[cmd];
                        other_incomplete += trace->incomplete[cmd];
                    }
                }
                log_debug("sio0: slot%u %s transactions=%u complete=%u incomplete=%u "
                    "bytes=%u..%u commands(total/incomplete)=[%s] "
                    "other_commands=%u other_total=%u other_incomplete=%u",
                    slot, pad_trace_device_name(dev ? DEST_MCD : DEST_JOY),
                    trace->transactions, trace->transactions - incomplete, incomplete,
                    trace->min_bytes, trace->max_bytes, commands,
                    omitted, other_total, other_incomplete);
                if (trace->analog_polls) {
                    const uint8_t* p = trace->last_poll;
                    log_debug("sio0: slot%u analog_polls=%u last_reply="
                        "%02x %02x %02x %02x %02x %02x %02x %02x %02x "
                        "right=%u,%u left=%u,%u rx=%u..%u ry=%u..%u lx=%u..%u ly=%u..%u",
                        slot, trace->analog_polls, p[0], p[1], p[2], p[3], p[4],
                        p[5], p[6], p[7], p[8], p[5], p[6], p[7], p[8],
                        trace->axis_min[0], trace->axis_max[0], trace->axis_min[1], trace->axis_max[1],
                        trace->axis_min[2], trace->axis_max[2], trace->axis_min[3], trace->axis_max[3]);
                }
            }
            memset(trace, 0, sizeof(*trace));
        }
    }
    if (enabled && pad->trace_open) {
        log_debug("sio0: open slot%u %s cmd=%02x bytes=%u stat=%04x ctrl=%04x dest=%02x/%02x",
            pad->trace_open_slot, pad_trace_device_name(pad->trace_open_dev),
            pad->trace_open_cmd, pad->trace_open_bytes, pad->stat, pad->ctrl,
            (unsigned)pad->dest[0] & 0xffu, (unsigned)pad->dest[1] & 0xffu);
    }
    pad->trace_pending = 0;
}

/* A device just took the bus. */
static void pad_trace_begin(psx_pad_t* pad, int slot, uint8_t dev) {
    if (log_get_level() > LOG_DEBUG)
        return;
    pad->trace_open = 1;
    pad->trace_open_slot = (uint8_t)slot;
    pad->trace_open_dev = dev;
    pad->trace_open_cmd = 0;
    pad->trace_open_bytes = 0;
    pad->trace_reply_size = 0;
}

/* `incomplete` means the transaction was still holding the bus when it ended — a deselect
   mid-transfer, or an abort because the device was not plugged in. */
static void pad_trace_end(psx_pad_t* pad, int incomplete) {
    if (!pad->trace_open)
        return;

    pad->trace_open = 0;

    psx_pad_trace_t* trace = &pad->trace[pad->trace_open_slot][pad->trace_open_dev == DEST_MCD];
    if (!trace->transactions || pad->trace_open_bytes < trace->min_bytes)
        trace->min_bytes = pad->trace_open_bytes;
    if (pad->trace_open_bytes > trace->max_bytes)
        trace->max_bytes = pad->trace_open_bytes;
    ++trace->transactions;
    ++trace->commands[pad->trace_open_cmd];
    trace->incomplete[pad->trace_open_cmd] += incomplete != 0;
    if (!incomplete && pad->trace_open_dev == DEST_JOY && pad->trace_open_cmd == 0x42 &&
        pad->trace_reply_size == 9 &&
        (pad->trace_reply[1] == 0x73 || pad->trace_reply[1] == 0xf3)) {
        memcpy(trace->last_poll, pad->trace_reply, sizeof(trace->last_poll));
        for (unsigned axis = 0; axis < 4; ++axis) {
            const uint8_t value = pad->trace_reply[5 + axis];
            if (!trace->analog_polls || value < trace->axis_min[axis])
                trace->axis_min[axis] = value;
            if (!trace->analog_polls || value > trace->axis_max[axis])
                trace->axis_max[axis] = value;
        }
        ++trace->analog_polls;
    }
    pad->trace_pending = 1;
}

uint32_t pad_read_rx(psx_pad_t* pad) {
    int slot = (pad->ctrl >> 13) & 1;

    psx_input_t* joy = pad->joy_slot[slot];
    psx_mcd_t* mcd = pad->mcd_slot[slot];

    if (!pad->dest[slot])
        return 0xffffffff;

    if (!(pad->ctrl & CTRL_JOUT) && !(pad->ctrl & CTRL_RXEN))
        return 0xffffffff;

    switch (pad->dest[slot]) {
        case DEST_JOY: {
            if (!joy) {
                pad->dest[slot] = 0;

                return 0xffffffff;
            }

            uint8_t data = joy->read_func(joy->udata);
            if (pad->trace_open && pad->trace_reply_size < sizeof(pad->trace_reply))
                pad->trace_reply[pad->trace_reply_size++] = data;

            if (!joy->query_fifo_func(joy->udata)) {
                pad->dest[slot] = 0;

                pad_trace_end(pad, 0);
            }

            return data;
        } break;

        case DEST_MCD: {
            if (!mcd) {
                pad->dest[slot] = 0;

                return 0xffffffff;
            }

            uint8_t data = psx_mcd_read(mcd);

            if (!psx_mcd_query(mcd)) {
                pad->dest[slot] = 0;

                pad_trace_end(pad, 0);
            }

            return data;
        } break;
    }

    return 0xffffffff;
}

void pad_write_tx(psx_pad_t* pad, uint16_t data) {
    int slot = (pad->ctrl >> 13) & 1;

    psx_input_t* joy = pad->joy_slot[slot];
    psx_mcd_t* mcd = pad->mcd_slot[slot];

    if (!(pad->ctrl & CTRL_TXEN) || !(pad->ctrl & CTRL_JOUT))
        return;

    if (!pad->dest[slot]) {
        if ((data == DEST_JOY) || (data == DEST_MCD)) {
            pad->dest[slot] = data;

            /* A select while one is still open means the previous transaction never
               deselected — worth seeing, so close it as incomplete rather than losing it. */
            pad_trace_end(pad, 1);
            pad_trace_begin(pad, slot, (uint8_t)data);

            if ((data == DEST_JOY) && !joy) {
                pad_trace_end(pad, 1);

                return;
            }

            if ((data == DEST_MCD) && !mcd) {
                pad_trace_end(pad, 1);

                return;
            }

            if (pad->ctrl & CTRL_ACIE)
                pad->cycles_until_irq = JOY_IRQ_DELAY;
        }
    } else {
        /* A payload byte. The first one after the select is the command (42h pad read,
           52h/57h card read/write, ...), which is what names the transaction. */
        if (pad->trace_open) {
            if (!pad->trace_open_bytes)
                pad->trace_open_cmd = (uint8_t)data;

            pad->trace_open_bytes++;
        }

        switch (pad->dest[slot]) {
            case DEST_JOY: {
                if (!joy) {
                    pad->dest[slot] = 0;

                    pad_trace_end(pad, 1);

                    return;
                }

                joy->write_func(joy->udata, data);

                if (!joy->query_fifo_func(joy->udata)) {
                    pad->dest[slot] = 0;

                    pad_trace_end(pad, 0);
                }
            } break;

            case DEST_MCD: {
                if (!mcd) {
                    pad->dest[slot] = 0;

                    pad_trace_end(pad, 1);

                    return;
                }

                psx_mcd_write(mcd, data);

                if (pad->ctrl & CTRL_ACIE) {
                    if (mcd->state == MCD_R_STATE_TX_MEB ||
                        mcd->state == MCD_W_STATE_TX_MEB ||
                        mcd->state == MCD_S_STATE_TX_DAT3 ||
                        (mcd->state == MCD_R_STATE_TX_LSB && mcd->msb >= 4) ||
                        (mcd->state == MCD_STATE_TX_FLG &&
                         data != 'R' && data != 'W' && data != 'S'))
                        return;
                    pad->irq_bit = 1;
                    pad->cycles_until_irq = 1024;

                    return;
                }

                if (!psx_mcd_query(mcd)) {
                    pad->dest[slot] = 0;

                    pad_trace_end(pad, 0);
                }
            } break;
        }

        if (pad->ctrl & CTRL_ACIE) {
            pad->irq_bit = 1;
            pad->cycles_until_irq = (pad->dest[slot] == DEST_MCD) ? 2048 : JOY_IRQ_DELAY;
        }
    }
}

uint32_t pad_handle_stat_read(psx_pad_t* pad) {
    return pad->stat | 7;
}

static void pad_reset_transfer(psx_pad_t* pad, int slot) {
    pad->dest[slot] = 0;
    if (pad->mcd_slot[slot])
        psx_mcd_reset(pad->mcd_slot[slot]);
    psx_input_t* input = pad->joy_slot[slot];
    if (!input || !input->udata)
        return;
    switch (input->kind) {
        case PSX_INPUT_KIND_SDA: psxi_sda_reset_transfer(input->udata); break;
        case PSX_INPUT_KIND_MULTITAP: psxi_multitap_reset_transfer(input->udata); break;
        case PSX_INPUT_KIND_GUNCON: psxi_guncon_reset_transfer(input->udata); break;
    }
}

void pad_handle_ctrl_write(psx_pad_t* pad, uint32_t value) {
    int slot = pad->ctrl & CTRL_SLOT;

    if (value & CTRL_REST) {
        pad_trace_end(pad, 1);
        pad->ctrl = pad->mode = pad->baud = pad->stat = 0;
        pad_reset_transfer(pad, 0);
        pad_reset_transfer(pad, 1);
        pad->cycles_until_irq = pad->irq_bit = 0;
        return;
    }

    if ((pad->ctrl ^ value) & CTRL_SLOT) {
        pad_trace_end(pad, 1);
        pad_reset_transfer(pad, slot >> 13);
        pad_reset_transfer(pad, (slot >> 13) ^ 1);
    }

    pad->ctrl = value;

    if (!(pad->ctrl & CTRL_JOUT)) {
        pad->ctrl &= ~CTRL_SLOT;

        /* Deselect. A transaction still open here never finished its transfer, which is the
           signature worth catching: the port is being dropped mid-conversation. */
        if (pad->trace_open)
            pad_trace_end(pad, 1);

        /*
            Dropping /SEL ENDS the transaction, so the port must forget which device held it.

            This used to reset the memory card's state machine but leave pad->dest[] set, and
            dest[] is what routes the next byte written to TX. A transaction that ends by
            deselect rather than by running its FIFO dry — which is how a card command
            normally finishes — therefore left the port still believing the card was selected,
            and the game's NEXT device-select byte (01h, "talk to the controller") was fed to
            psx_mcd_write() as a command byte instead of selecting the pad. The controller is
            never addressed again, so its reads stop returning.

            That is precisely the reported symptom: input alive everywhere, dead only on the
            one screen that talks to the memory card, on a bus the pad and the card share.

            Both slots, because CTRL.SLOT has already been cleared above and a stale select on
            the slot that is not being addressed is just as wedged.
        */
        pad_reset_transfer(pad, 0);
        pad_reset_transfer(pad, 1);
    }

    // Reset STAT bits 3, 4, 5, 9
    if (pad->ctrl & CTRL_ACKN) {
        pad->stat &= 0xfdc7;
        pad->ctrl &= ~CTRL_ACKN;
    }
}

psx_pad_t* psx_pad_create(void) {
    return (psx_pad_t*)calloc(1, sizeof(psx_pad_t));
}

void psx_pad_init(psx_pad_t* pad, psx_ic_t* ic) {
    memset(pad, 0, sizeof(psx_pad_t));

    pad->ic = ic;

    pad->io_base = PSX_PAD_BEGIN;
    pad->io_size = PSX_PAD_SIZE;

    pad->joy_slot[0] = NULL;
    pad->joy_slot[1] = NULL;
    pad->mcd_slot[0] = NULL;
    pad->mcd_slot[1] = NULL;
}

uint32_t psx_pad_read32(psx_pad_t* pad, uint32_t offset) {
    uint32_t v = 0;

    switch (offset) {
        case 0: v = pad_read_rx(pad); break;
        case 4: v = pad_handle_stat_read(pad); break;
        case 8: v = pad->mode; break;
        case 10: v = pad->ctrl; break;
        case 14: v = pad->baud; break;
    }

    return v;
}

uint16_t psx_pad_read16(psx_pad_t* pad, uint32_t offset) {
    uint32_t v = 0;

    switch (offset) {
        case 0: v = pad_read_rx(pad) & 0xffff; break;
        case 4: v = pad_handle_stat_read(pad) & 0xffff; break;
        case 8: v = pad->mode; break;
        case 10: v = pad->ctrl & 0xffff; break;
        case 14: v = pad->baud; break;
    }

    return v;
}

uint8_t psx_pad_read8(psx_pad_t* pad, uint32_t offset) {
    uint32_t v = 0;

    switch (offset) {
        case 0: v = pad_read_rx(pad) & 0xff; break;
        case 4: v = pad_handle_stat_read(pad) & 0xff; break;
        case 8: v = pad->mode; break;
        case 10: v = pad->ctrl & 0xff; break;
        case 14: v = pad->baud; break;
    }

    // if (offset <= 0xa)
    // printf("slot=%d dest=%02x pad_read8(%02x)   -> %02x\n",
    //     (pad->ctrl & CTRL_SLOT) >> 13,
    //     pad->dest[(pad->ctrl & CTRL_SLOT) >> 13],
    //     offset,
    //     v
    // );

    return v;
}

void psx_pad_write32(psx_pad_t* pad, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0: pad_write_tx(pad, value); return;
        case 8: pad->mode = value & 0xffff; return;
        case 10: pad_handle_ctrl_write(pad, value); return;
        case 14: pad->baud = value & 0xffff; return;
    }

    printf("Unhandled 32-bit PAD write at offset %08x (%08x)", offset, value);
}

void psx_pad_write16(psx_pad_t* pad, uint32_t offset, uint16_t value) {
    switch (offset) {
        case 0: pad_write_tx(pad, value); return;
        case 8: pad->mode = value; return;
        case 10: pad_handle_ctrl_write(pad, value); return;
        case 14: pad->baud = value; return;
    }

    printf("Unhandled 16-bit PAD write at offset %08x (%04x)", offset, value);
}

void psx_pad_write8(psx_pad_t* pad, uint32_t offset, uint8_t value) {
    // if (offset <= 0xa)
    // printf("slot=%d dest=%02x pad_write8(%02x)  -> %02x\n",
    //     (pad->ctrl & CTRL_SLOT) >> 13,
    //     pad->dest[(pad->ctrl & CTRL_SLOT) >> 13],
    //     offset,
    //     value
    // );

    switch (offset) {
        case 0: pad_write_tx(pad, value); return;
        case 8: pad->mode = value; return;
        case 10: pad_handle_ctrl_write(pad, value); return;
        case 14: pad->baud = value; return;
    }

    printf("Unhandled 8-bit PAD write at offset %08x (%02x)", offset, value);
}

void psx_pad_button_press(psx_pad_t* pad, int slot, uint32_t data) {
    psx_input_t* selected_slot = pad->joy_slot[slot];

    if (selected_slot)
        selected_slot->on_button_press_func(selected_slot->udata, data);
}

void psx_pad_button_release(psx_pad_t* pad, int slot, uint32_t data) {
    psx_input_t* selected_slot = pad->joy_slot[slot];

    if (selected_slot)
        selected_slot->on_button_release_func(selected_slot->udata, data);
}

void psx_pad_analog_change(psx_pad_t* pad, int slot, uint32_t stick, uint16_t data) {
    psx_input_t* selected_slot = pad->joy_slot[slot];

    if (selected_slot)
        selected_slot->on_analog_change_func(selected_slot->udata, stick, data);
}

/* The multitap behind `slot`, or NULL when that slot holds anything else. */
static psxi_multitap_t* pad_multitap(psx_pad_t* pad, int slot) {
    psx_input_t* input;

    if (!pad || slot < 0 || slot > 1)
        return NULL;

    input = pad->joy_slot[slot];

    if (!input || !input->udata || input->kind != PSX_INPUT_KIND_MULTITAP)
        return NULL;

    return (psxi_multitap_t*)input->udata;
}

void psx_pad_button_press_player(psx_pad_t* pad, int slot, int player, uint32_t data) {
    psxi_multitap_t* tap = pad_multitap(pad, slot);

    if (tap) {
        psxi_multitap_button_press(tap, player, data);
        return;
    }

    /* No tap: only the port's own player exists. Dropping the rest is
       deliberate — folding player 2 onto player 1 would make a mis-routed pad
       look like a working one. */
    if (player == 0)
        psx_pad_button_press(pad, slot, data);
}

void psx_pad_button_release_player(psx_pad_t* pad, int slot, int player, uint32_t data) {
    psxi_multitap_t* tap = pad_multitap(pad, slot);

    if (tap) {
        psxi_multitap_button_release(tap, player, data);
        return;
    }

    if (player == 0)
        psx_pad_button_release(pad, slot, data);
}

void psx_pad_analog_change_player(psx_pad_t* pad, int slot, int player, uint32_t stick,
                                  uint16_t data) {
    psxi_multitap_t* tap = pad_multitap(pad, slot);

    if (tap) {
        psxi_multitap_analog_change(tap, player, stick, data);
        return;
    }

    if (player == 0)
        psx_pad_analog_change(pad, slot, stick, data);
}

int psx_pad_player_count(psx_pad_t* pad, int slot) {
    if (pad_multitap(pad, slot))
        return PSXI_MULTITAP_SLOTS;

    if (!pad || slot < 0 || slot > 1 || !pad->joy_slot[slot])
        return 0;

    return 1;
}

void psx_pad_attach_joy(psx_pad_t* pad, int slot, psx_input_t* input) {
    if (pad->joy_slot[slot])
        psx_pad_detach_joy(pad, slot);

    pad->joy_slot[slot] = input;
}

void psx_pad_detach_joy(psx_pad_t* pad, int slot) {
    if (!pad->joy_slot[slot])
        return;

    psx_input_destroy(pad->joy_slot[slot]);

    pad->joy_slot[slot] = NULL;
}

int psx_pad_attach_mcd(psx_pad_t* pad, int slot, const char* path) {
    if (!path)
        return 1;

    if (pad->mcd_slot[slot])
        psx_pad_detach_mcd(pad, slot);

    psx_mcd_t* mcd = psx_mcd_create();

    if (!mcd)
        return 1;

    int r = psx_mcd_init(mcd, path);
    
    if (r) {
        psx_mcd_destroy(mcd);

        return r;
    }

    pad->mcd_slot[slot] = mcd;

    return 0;
}

void psx_pad_detach_mcd(psx_pad_t* pad, int slot) {
    if (!pad->mcd_slot[slot])
        return;

    psx_mcd_destroy(pad->mcd_slot[slot]);

    pad->mcd_slot[slot] = NULL;
}

int psx_pad_mcd_fingerprint(psx_pad_t* pad, int slot, uint64_t* out_hash,
                            uint64_t* out_session, uint32_t* out_generation,
                            int64_t* out_mtime) {
    psx_mcd_t* mcd;

    if (out_hash)
        *out_hash = 0;
    if (out_session)
        *out_session = 0;
    if (out_generation)
        *out_generation = 0;
    if (out_mtime)
        *out_mtime = 0;

    if (!pad || slot < 0 || slot > 1)
        return 0;

    mcd = pad->mcd_slot[slot];

    /* An attached card with no image is not a card the state can be compared
       against — treat it as absent rather than fingerprinting a null buffer. */
    if (!mcd || !mcd->buf)
        return 0;

    if (out_hash)
        *out_hash = psx_mcd_content_hash(mcd);
    if (out_session)
        *out_session = psx_mcd_session_id(mcd);
    if (out_generation)
        *out_generation = psx_mcd_write_generation(mcd);
    if (out_mtime)
        *out_mtime = psx_mcd_file_mtime(mcd);

    return 1;
}

void psx_pad_update(psx_pad_t* pad, int cyc) {
    if ((pad->trace_pending || pad->trace_open) && cyc > 0) {
        pad->trace_cycles += (unsigned)cyc;
        if (pad->trace_cycles >= PAD_TRACE_INTERVAL_CYCLES) {
            pad_trace_flush(pad);
            pad->trace_cycles %= PAD_TRACE_INTERVAL_CYCLES;
        }
    }
    if (pad->mcd_slot[0] && pad->mcd_slot[0]->dirty)
        psx_mcd_update(pad->mcd_slot[0], cyc);
    if (pad->mcd_slot[1] && pad->mcd_slot[1]->dirty)
        psx_mcd_update(pad->mcd_slot[1], cyc);
    if (pad->cycles_until_irq) {
        pad->cycles_until_irq -= cyc;

        if (pad->cycles_until_irq <= 0) {
            psx_ic_irq(pad->ic, IC_JOY);

            if (pad->irq_bit) {
                pad->stat |= STAT_IRQ7;
                pad->irq_bit = 0;
            }

            pad->cycles_until_irq = 0;
        }
    }
}


/*
    Save state.

    The PAD section owns the serial port itself plus whatever is plugged into
    its two slots. Slot contents are written as tagged sub-blocks (present flag
    + psx_input_t::kind) so a state taken with a pad in slot 0 does not get
    decoded into a GunCon, and so a state made with a card inserted refuses to
    half-apply when the front-end booted without one.

    pad->ic, pad->joy_slot[] and pad->mcd_slot[] are pointers: the devices they
    reference are restored in place, never reallocated.
*/

static void pad_save_joy(psx_pad_t* pad, int slot, psx_state_writer_t* w) {
    psx_input_t* input = pad->joy_slot[slot];

    if (!input || !input->udata) {
        psx_sw_u32(w, PSX_INPUT_KIND_NONE);
        return;
    }

    psx_sw_u32(w, input->kind);

    switch (input->kind) {
        case PSX_INPUT_KIND_SDA:
            psxi_sda_save_state((psxi_sda_t*)input->udata, w);
            break;

        case PSX_INPUT_KIND_GUNCON:
            psxi_guncon_save_state((psxi_guncon_t*)input->udata, w);
            break;

        case PSX_INPUT_KIND_MULTITAP:
            psxi_multitap_save_state((psxi_multitap_t*)input->udata, w);
            break;

        default:
            /* An untagged controller: nothing to write, and nothing will be
               read back. Its transfer state machine is lost across a load. */
            break;
    }
}

static int pad_load_joy(psx_pad_t* pad, int slot, psx_state_reader_t* r) {
    psx_input_t* input = pad->joy_slot[slot];
    uint32_t kind = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (kind == PSX_INPUT_KIND_NONE)
        return PSX_STATE_OK;

    /* The state had a controller here but this machine does not (or has a
       different one). Skipping is not an option — the payload length is
       type-dependent — so refuse rather than desynchronise the stream.

       This is also what a multitap toggled between save and load looks like: the
       state carries KIND_MULTITAP and the machine has a plain pad, or the other
       way round. The refusal is correct (a four-player packet cannot be decoded
       into a one-player pad), but the generic message is not, so say what
       actually happened. */
    if (!input || !input->udata || input->kind != kind) {
        if (kind == PSX_INPUT_KIND_MULTITAP || (input && input->kind == PSX_INPUT_KIND_MULTITAP))
            log_error("pad: save state and machine disagree about the multitap in port %d "
                      "(state=%s, machine=%s)",
                      slot + 1,
                      kind == PSX_INPUT_KIND_MULTITAP ? "multitap" : "pad",
                      (input && input->kind == PSX_INPUT_KIND_MULTITAP) ? "multitap" : "pad");

        return PSX_STATE_ERR_GEOMETRY;
    }

    switch (kind) {
        case PSX_INPUT_KIND_SDA:
            return psxi_sda_load_state((psxi_sda_t*)input->udata, r);

        case PSX_INPUT_KIND_GUNCON:
            return psxi_guncon_load_state((psxi_guncon_t*)input->udata, r);

        case PSX_INPUT_KIND_MULTITAP:
            return psxi_multitap_load_state((psxi_multitap_t*)input->udata, r);

        default:
            return PSX_STATE_ERR_GEOMETRY;
    }
}

void psx_pad_save_state(psx_pad_t* pad, psx_state_writer_t* w) {
    int i;

    psx_sw_i32(w, pad->enable_once);
    psx_sw_i32(w, pad->cycles_until_irq);
    psx_sw_i32(w, pad->cycle_counter);
    psx_sw_i32(w, pad->dest[0]);
    psx_sw_i32(w, pad->dest[1]);
    psx_sw_i32(w, pad->irq_bit);
    psx_sw_u16(w, pad->mode);
    psx_sw_u16(w, pad->ctrl);
    psx_sw_u16(w, pad->baud);
    psx_sw_u16(w, pad->stat);

    for (i = 0; i < 2; i++)
        pad_save_joy(pad, i, w);

    for (i = 0; i < 2; i++) {
        if (pad->mcd_slot[i] && pad->mcd_slot[i]->buf) {
            psx_sw_u32(w, 1);
            psx_mcd_save_state(pad->mcd_slot[i], w);
        } else {
            psx_sw_u32(w, 0);
        }
    }
}

int psx_pad_load_state(psx_pad_t* pad, psx_state_reader_t* r) {
    int i;
    int rc;

    pad->enable_once = psx_sr_i32(r);
    pad->cycles_until_irq = psx_sr_i32(r);
    pad->cycle_counter = psx_sr_i32(r);
    pad->dest[0] = psx_sr_i32(r);
    pad->dest[1] = psx_sr_i32(r);
    pad->irq_bit = psx_sr_i32(r);
    pad->mode = psx_sr_u16(r);
    pad->ctrl = psx_sr_u16(r);
    pad->baud = psx_sr_u16(r);
    pad->stat = psx_sr_u16(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    for (i = 0; i < 2; i++) {
        rc = pad_load_joy(pad, i, r);

        if (rc != PSX_STATE_OK)
            return rc;
    }

    for (i = 0; i < 2; i++) {
        uint32_t present = psx_sr_u32(r);

        if (r->error)
            return PSX_STATE_ERR_TRUNCATED;

        if (!present)
            continue;

        if (!pad->mcd_slot[i] || !pad->mcd_slot[i]->buf)
            return PSX_STATE_ERR_GEOMETRY;

        rc = psx_mcd_load_state(pad->mcd_slot[i], r);

        if (rc != PSX_STATE_OK)
            return rc;
    }

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_pad_destroy(psx_pad_t* pad) {
    pad_trace_end(pad, 1);
    if (pad->trace_pending)
        pad_trace_flush(pad);
    psx_pad_detach_joy(pad, 0);
    psx_pad_detach_joy(pad, 1);
    psx_pad_detach_mcd(pad, 0);
    psx_pad_detach_mcd(pad, 1);

    free(pad);
}
