#include "dualshock_state.h"

static unsigned pad_count(const psx_input_t* input) {
    if (!input || !input->udata)
        return 0;
    if (input->kind == PSX_INPUT_KIND_SDA)
        return 1;
    return input->kind == PSX_INPUT_KIND_MULTITAP ? PSXI_MULTITAP_SLOTS : 0;
}

static psxi_sda_t* subpad(const psx_input_t* input, unsigned index) {
    if (input->kind == PSX_INPUT_KIND_MULTITAP)
        return &((psxi_multitap_t*)input->udata)->sub[index];
    return (psxi_sda_t*)input->udata;
}

void psxi_dualshock_save_state(const psx_pad_t* pad, psx_state_writer_t* w) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        const psx_input_t* input = pad->joy_slot[slot];
        psx_sw_u32(w, input && input->udata ? input->kind : PSX_INPUT_KIND_NONE);
        for (unsigned i = 0; i < pad_count(input); ++i)
            psxi_sda_save_extended_state(subpad(input, i), w);
    }
}

int psxi_dualshock_load_state(psx_pad_t* pad, psx_state_reader_t* r, int apply) {
    psxi_sda_t values[2][PSXI_MULTITAP_SLOTS];
    for (unsigned slot = 0; slot < 2; ++slot) {
        const psx_input_t* input = pad->joy_slot[slot];
        const uint32_t kind = psx_sr_u32(r);
        if (r->error)
            return PSX_STATE_ERR_TRUNCATED;
        if (kind != (input && input->udata ? input->kind : PSX_INPUT_KIND_NONE))
            return PSX_STATE_ERR_GEOMETRY;
        for (unsigned i = 0; i < pad_count(input); ++i) {
            values[slot][i] = *subpad(input, i);
            const int result = psxi_sda_load_extended_state(&values[slot][i], r);
            if (result != PSX_STATE_OK)
                return result;
        }
    }
    if (r->offset != r->size)
        return PSX_STATE_ERR_TRUNCATED;
    if (apply) {
        for (unsigned slot = 0; slot < 2; ++slot)
            for (unsigned i = 0; i < pad_count(pad->joy_slot[slot]); ++i)
                *subpad(pad->joy_slot[slot], i) = values[slot][i];
    }
    return PSX_STATE_OK;
}
