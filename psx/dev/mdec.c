#include "mdec.h"
#include "../log.h"
#include "../perf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int zigzag[] = {
    0 , 1 , 5 , 6 , 14, 15, 27, 28,
    2 , 4 , 7 , 13, 16, 26, 29, 42,
    3 , 8 , 12, 17, 25, 30, 41, 43,
    9 , 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

int zagzig[] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

float scalezag[] = {
    0.125000, 0.173380, 0.173380, 0.163320, 0.240485, 0.163320, 0.146984, 0.226532,
    0.226532, 0.146984, 0.125000, 0.203873, 0.213388, 0.203873, 0.125000, 0.098212,
    0.173380, 0.192044, 0.192044, 0.173380, 0.098212, 0.067650, 0.136224, 0.163320,
    0.172835, 0.163320, 0.136224, 0.067650, 0.034487, 0.093833, 0.128320, 0.146984,
    0.146984, 0.128320, 0.093833, 0.034487, 0.047835, 0.088388, 0.115485, 0.125000,
    0.115485, 0.088388, 0.047835, 0.045060, 0.079547, 0.098212, 0.098212, 0.079547,
    0.045060, 0.040553, 0.067650, 0.077165, 0.067650, 0.040553, 0.034487, 0.053152, 
    0.053152, 0.034487, 0.027097, 0.036612, 0.027097, 0.018664, 0.018664, 0.009515
};

#define EXTS10(v) (((int16_t)((v) << 6)) >> 6)
#define CLAMP(v, l, h) ((v <= l) ? l : ((v >= h) ? h : v))
#define MAX(a, b) (a > b ? a : b)

void real_idct(int16_t* blk, int16_t* scale) {
    int16_t buf[64];

    int16_t* src = blk;
    int16_t* dst = buf;

    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                int sum = 0;

                for (int z = 0; z < 8; z++)
                    sum += (int32_t)src[y+z*8] * ((int32_t)scale[x+z*8] / 8);
                
                dst[x+y*8] = (sum + 0xfff) / 0x2000;
            }
        }

        int16_t* temp = src;

        src = dst;
        dst = temp;
    }
}

#define IDCT_FUNC(blk, scale) real_idct(blk, scale)

uint16_t* rl_decode_block(int16_t* blk, uint16_t* src, uint8_t* quant, int16_t* scale) {
    PSX_PERF_INC(mdec_blocks);

    int k = 0;

    for (int i = 0; i < 64; i++)
        blk[i] = 0;
    
    uint16_t n = *src;

    ++src;

    while (n == 0xfe00) {
        n = *src;

        ++src;
    }

    int q_scale = (n >> 10) & 0x3f;

    int16_t val = EXTS10(n & 0x3ff) * quant[k];

    while (k < 64) {
        if (!q_scale)
            val = EXTS10(n & 0x3ff) * 2;

        val = CLAMP(val, -0x400, 0x3ff);
        // val *= scalezag[k]; // For fast IDCT

        if (q_scale > 0)
            blk[zagzig[k]] = val;
        
        if (!q_scale)
            blk[k] = val;

        n = *src;

        if (k == 63)
            break;

        ++src;

        k += ((n >> 10) & 0x3f) + 1;

        val = (EXTS10(n & 0x3ff) * quant[k] * q_scale + 4) / 8;
    }

    IDCT_FUNC(blk, scale);

    return src;
}

//   for y=0 to 7
//     for x=0 to 7
//       R=[Crblk+((x+xx)/2)+((y+yy)/2)*8], B=[Cbblk+((x+xx)/2)+((y+yy)/2)*8]
//       G=(-0.3437*B)+(-0.7143*R), R=(1.402*R), B=(1.772*B)
//       Y=[Yblk+(x)+(y)*8]
//       R=MinMax(-128,127,(Y+R))
//       G=MinMax(-128,127,(Y+G))
//       B=MinMax(-128,127,(Y+B))
//       if unsigned then BGR=BGR xor 808080h  ;aka add 128 to the R,G,B values
//       dst[(x+xx)+(y+yy)*16]=BGR
//     next x
//   next y

void yuv_to_rgb(psx_mdec_t* mdec, uint8_t* buf, int xx, int yy) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int16_t r = mdec->crblk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
            int16_t b = mdec->cbblk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
            int16_t g = (-0.3437 * (float)b) + (-0.7143 * (float)r);

            r = (1.402 * (float)r);
            b = (1.772 * (float)b);

            int16_t l = mdec->yblk[x + y * 8];

            r = CLAMP(l + r, -128, 127);
            g = CLAMP(l + g, -128, 127);
            b = CLAMP(l + b, -128, 127);

            if (!mdec->output_signed) {
                r ^= 0x80;
                g ^= 0x80;
                b ^= 0x80;
            }

            if (mdec->output_depth == 3) {
                uint16_t r5 = ((uint8_t)r) >> 3;
                uint16_t g5 = ((uint8_t)g) >> 3;
                uint16_t b5 = ((uint8_t)b) >> 3;

                uint16_t rgb = (b5 << 10) | (g5 << 5) | r5;

                if (mdec->output_bit15)
                    rgb |= 0x8000;
                
                buf[0 + ((x + xx) + (y + yy) * 16) * 2] = rgb & 0xff;
                buf[1 + ((x + xx) + (y + yy) * 16) * 2] = rgb >> 8;
            } else {
                buf[0 + ((x + xx) + (y + yy) * 16) * 3] = r & 0xff;
                buf[1 + ((x + xx) + (y + yy) * 16) * 3] = g & 0xff;
                buf[2 + ((x + xx) + (y + yy) * 16) * 3] = b & 0xff;
            }
        }
    }
}

void mdec_nop(psx_mdec_t* mdec) { /* Do nothing */ }

void mdec_decode_macroblock(psx_mdec_t* mdec) {
    PSX_PERF_INC(mdec_macroblocks);

    if (mdec->output_depth < 2) {
        size_t block_size = (mdec->output_depth == 3) ? 512 : 768;
        size_t size = block_size;

        mdec->output = malloc(size);

        rl_decode_block(mdec->yblk, (uint16_t*)mdec->input, mdec->y_quant_table, mdec->scale_table);

        for (int i = 0; i < 64; i++) {
            int16_t y = mdec->yblk[i] & 0xff;

            if (mdec->output_depth == 1) {
                mdec->output[i] = y;
            } else {
                // To-do
                mdec->output[i] = 0;
            }
        }

        mdec->output_words_remaining = ((mdec->output_depth == 1) ? 64 : 32) >> 2;
        mdec->output_empty = 0;
        mdec->output_index = 0;
    } else {
        uint16_t* in = (uint16_t*)mdec->input;

        size_t block_size = (mdec->output_depth == 3) ? 512 : 768;
        size_t size = block_size;

        unsigned long bytes_processed = 0;

        int block_count = 1;

        while (bytes_processed < mdec->input_size) {
            if (!mdec->output) {
                mdec->output = malloc(block_count * size);
            } else {
                mdec->output = realloc(mdec->output, block_count * size);
            }

            in = rl_decode_block(mdec->crblk, in, mdec->uv_quant_table, mdec->scale_table);
            in = rl_decode_block(mdec->cbblk, in, mdec->uv_quant_table, mdec->scale_table);
            in = rl_decode_block(mdec->yblk, in, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 0, 0);
            in = rl_decode_block(mdec->yblk, in, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 8, 0);
            in = rl_decode_block(mdec->yblk, in, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 0, 8);
            in = rl_decode_block(mdec->yblk, in, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 8, 8);

            bytes_processed = (uintptr_t)in - (uintptr_t)mdec->input;

            ++block_count;
        }

        mdec->output_words_remaining = ((block_count - 1) * block_size) >> 2;
        mdec->output_empty = 0;
        mdec->output_index = 0;

        // printf("output words remaining: %d (%x) count=%d block_size=%lld size=%lld\n", mdec->output_words_remaining, mdec->output_words_remaining, block_count, block_size, size);

        // log_set_quiet(0);
        // log_fatal("Finished decoding %u-bit MDEC data input=(%04x -> %08x)",
        //     (mdec->output_depth == 3) ? 15 : 24,
        //     mdec->input_size,
        //     mdec->output_words_remaining
        // );
        // log_set_quiet(1);
    }
}

void mdec_set_iqtab(psx_mdec_t* mdec) {
    memcpy(mdec->y_quant_table, mdec->input, 64);

    if (mdec->recv_color)
        memcpy(mdec->uv_quant_table, &mdec->input[16], 64);
}

void mdec_set_scale(psx_mdec_t* mdec) {
    memcpy(mdec->scale_table, mdec->input, 128);
}

mdec_fn_t g_mdec_cmd_table[] = {
    mdec_nop,
    mdec_decode_macroblock,
    mdec_set_iqtab,
    mdec_set_scale,
    mdec_nop,
    mdec_nop,
    mdec_nop,
    mdec_nop
};

psx_mdec_t* psx_mdec_create(void) {
    return (psx_mdec_t*)malloc(sizeof(psx_mdec_t));
}

void psx_mdec_init(psx_mdec_t* mdec) {
    memset(mdec, 0, sizeof(psx_mdec_t));

    mdec->io_base = PSX_MDEC_BEGIN;
    mdec->io_size = PSX_MDEC_SIZE;

    mdec->state = MDEC_RECV_CMD;
}

uint32_t psx_mdec_read32(psx_mdec_t* mdec, uint32_t offset) {
    switch (offset) {
        case 0: {
            // printf("mdec data read\n");
            // mdec->output_empty = 1;
            // mdec->output_index = 0;
            // mdec->output_request = 0;

            // return 0xaaaaaaaa;

            if (mdec->output_words_remaining) {
                --mdec->output_words_remaining;

                // log_set_quiet(0);
                // log_fatal("output read %08x", 0);
                // log_set_quiet(1);

                return ((uint32_t*)mdec->output)[mdec->output_index++];
            } else {
                // printf("no read words remaining\n");
                mdec->output_empty = 0;
                mdec->output_index = 0;
                mdec->output_request = 0;

                return 0xaaaaaaaa;
            }
        } break;
        case 4: {
            //printf("mdec status read\n");
            uint32_t status = 0;

            status |= mdec->words_remaining;
            status |= mdec->current_block    << 16;
            status |= mdec->output_bit15     << 23;
            status |= mdec->output_signed    << 24;
            status |= mdec->output_depth     << 25;
            status |= mdec->output_request   << 27;
            status |= mdec->input_request    << 28;
            status |= mdec->busy             << 29;
            status |= mdec->input_full       << 30;
            status |= mdec->output_empty     << 31;

            return status;
        } break;
    }

    return 0x0;
}

uint16_t psx_mdec_read16(psx_mdec_t* mdec, uint32_t offset) {
    printf("Unhandled 16-bit MDEC read offset=%u\n", offset);

    exit(1);
}

uint8_t psx_mdec_read8(psx_mdec_t* mdec, uint32_t offset) {
    printf("Unhandled 8-bit MDEC read offset=%u\n", offset);

    exit(1);
}

void psx_mdec_write32(psx_mdec_t* mdec, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0: {
            //printf("mdec data write\n");
            if (mdec->words_remaining) {
                mdec->input[mdec->input_index++] = value;

                --mdec->words_remaining;

                if (!mdec->words_remaining) {
                    //printf("no words remaining\n");
                    mdec->output_empty = 0;
                    mdec->input_full = 1;
                    mdec->input_request = 0;
                    mdec->busy = 0;
                    mdec->output_request = mdec->enable_dma1;

                    g_mdec_cmd_table[mdec->cmd >> 29](mdec);

                    free(mdec->input);
                }

                break;
            }

            mdec->cmd = value;
            mdec->output_request = 0;
            mdec->output_empty = 1;
            mdec->output_bit15 = (value >> 25) & 1;
            mdec->output_signed = (value >> 26) & 1;
            mdec->output_depth = (value >> 27) & 3;
            mdec->input_index = 0;
            mdec->input_full = 0;
            mdec->busy = 1;

            //log_set_quiet(0);
            switch (mdec->cmd >> 29) {
                case MDEC_CMD_NOP: {
                    //printf("mdec nop\n");
                    mdec->busy = 0;
                    mdec->words_remaining = 0;

                    log_fatal("MDEC %08x: NOP", mdec->cmd);
                } break;

                case MDEC_CMD_DECODE: {
                    //printf("mdec decode\n");
                    mdec->words_remaining = mdec->cmd & 0xffff;

                    // printf("MDEC %08x: decode macroblock %04x\n",
                    //     mdec->cmd,
                    //     mdec->words_remaining
                    // );
                } break;

                case MDEC_CMD_SET_QT: {
                    //printf("mdec setqt\n");
                    mdec->recv_color = mdec->cmd & 1;
                    mdec->words_remaining = mdec->recv_color ? 32 : 16;

                    log_fatal("MDEC %08x: set quant tables %04x",
                        mdec->cmd,
                        mdec->words_remaining
                    );
                } break;

                case MDEC_CMD_SET_ST: {
                    //printf("mdec setst\n");
                    mdec->words_remaining = 32;

                    log_fatal("MDEC %08x: set scale table %04x",
                        mdec->cmd,
                        mdec->words_remaining
                    );
                } break;
            }
            // log_set_quiet(1);

            if (mdec->words_remaining) {
                mdec->input_request = 1;
                mdec->input_size = mdec->words_remaining * sizeof(uint32_t);
                mdec->input_full = 0;
                mdec->input_index = 0;
                mdec->input = malloc(mdec->input_size);
            }
        } break;

        case 4: {
            //printf("mdec status write\n");
            mdec->enable_dma0 = (value & 0x40000000) != 0;
            mdec->enable_dma1 = (value & 0x20000000) != 0;

            // Reset
            if (value & 0x80000000) {
                // status = 80040000h
                mdec->busy            = 0;
                mdec->words_remaining = 0;
                mdec->output_bit15    = 0;
                mdec->output_signed   = 0;
                mdec->output_depth    = 0;
                mdec->input_request   = 0;
                mdec->output_request  = 0;
                mdec->input_full      = 0;
                mdec->output_empty    = 1;
                mdec->current_block   = 4;
            }
        } break;
    }

    // log_fatal("32-bit MDEC write offset=%u, value=%08x", offset, value);
}

void psx_mdec_write16(psx_mdec_t* mdec, uint32_t offset, uint16_t value) {
    printf("Unhandled 16-bit MDEC write offset=%u, value=%04x\n", offset, value);
}

void psx_mdec_write8(psx_mdec_t* mdec, uint32_t offset, uint8_t value) {
    printf("Unhandled 8-bit MDEC write offset=%u, value=%02x\n", offset, value);
}


/* ---------------------------------------------------------------------------
   Save state.

   The two heap buffers need care, because neither is a plain fixed-size array:

     mdec->input   Allocated (input_size bytes) when a command with a parameter
                   count arrives, filled word by word, then consumed and
                   free()d the instant words_remaining hits zero — WITHOUT the
                   pointer being cleared. So it is only live while
                   words_remaining != 0; at any other observable moment it is a
                   dangling pointer that nothing reads. We therefore save it
                   only in that window (input_index words, the part that has
                   actually been written; the tail is uninitialised and will be
                   overwritten before it is read), and on load we allocate a
                   fresh buffer and NULL the pointer out otherwise.

     mdec->output  Allocated/realloc'd by the decoder and never resized down;
                   its byte size is not tracked anywhere. What the machine can
                   still observe is exactly the words between output_index and
                   output_index + output_words_remaining, so that is what we
                   store. On load we allocate exactly those words and rebase
                   output_index to 0 — every subsequent read returns the same
                   value it would have, which is what "faithful" means here.

   mdec->status is a scratch value recomputed on every status read; it is saved
   anyway so a state round-trips byte for byte.
   --------------------------------------------------------------------------- */

void psx_mdec_save_state(psx_mdec_t* mdec, psx_state_writer_t* w) {
    int i;
    uint32_t input_words;
    uint32_t output_words;

    psx_sw_u32(w, mdec->cmd);
    psx_sw_i32(w, mdec->state);
    psx_sw_i32(w, mdec->data_remaining);
    psx_sw_i32(w, mdec->index);
    psx_sw_u32(w, mdec->words_remaining);
    psx_sw_i32(w, mdec->current_block);
    psx_sw_i32(w, mdec->output_bit15);
    psx_sw_i32(w, mdec->output_signed);
    psx_sw_i32(w, mdec->output_depth);
    psx_sw_i32(w, mdec->input_request);
    psx_sw_i32(w, mdec->output_request);
    psx_sw_i32(w, mdec->busy);
    psx_sw_i32(w, mdec->input_full);
    psx_sw_i32(w, mdec->output_empty);
    psx_sw_i32(w, mdec->enable_dma0);
    psx_sw_i32(w, mdec->enable_dma1);
    psx_sw_i32(w, mdec->recv_color);
    psx_sw_u32(w, mdec->status);

    psx_sw_bytes(w, mdec->uv_quant_table, MDEC_QUANT_TABLE_SIZE);
    psx_sw_bytes(w, mdec->y_quant_table, MDEC_QUANT_TABLE_SIZE);
    psx_sw_i16_array(w, mdec->scale_table, MDEC_SCALE_TABLE_SIZE);

    psx_sw_i16_array(w, mdec->yblk, 64);
    psx_sw_i16_array(w, mdec->crblk, 64);
    psx_sw_i16_array(w, mdec->cbblk, 64);

    /* Input FIFO: live only while a command is still collecting parameters. */
    input_words = 0;

    if (mdec->words_remaining && mdec->input && mdec->input_index > 0)
        input_words = (uint32_t)mdec->input_index;

    psx_sw_u32(w, (uint32_t)mdec->input_size);
    psx_sw_u32(w, input_words);

    for (i = 0; i < (int)input_words; i++)
        psx_sw_u32(w, mdec->input[i]);

    /* Output FIFO: only the words the machine can still read out. */
    output_words = 0;

    if (mdec->output && mdec->output_words_remaining)
        output_words = mdec->output_words_remaining;

    psx_sw_u32(w, output_words);

    for (i = 0; i < (int)output_words; i++)
        psx_sw_u32(w, ((const uint32_t*)mdec->output)[mdec->output_index + i]);
}

int psx_mdec_load_state(psx_mdec_t* mdec, psx_state_reader_t* r) {
    uint32_t input_size;
    uint32_t input_words;
    uint32_t output_words;
    uint32_t i;

    mdec->cmd = psx_sr_u32(r);
    mdec->state = psx_sr_i32(r);
    mdec->data_remaining = psx_sr_i32(r);
    mdec->index = psx_sr_i32(r);
    mdec->words_remaining = psx_sr_u32(r);
    mdec->current_block = psx_sr_i32(r);
    mdec->output_bit15 = psx_sr_i32(r);
    mdec->output_signed = psx_sr_i32(r);
    mdec->output_depth = psx_sr_i32(r);
    mdec->input_request = psx_sr_i32(r);
    mdec->output_request = psx_sr_i32(r);
    mdec->busy = psx_sr_i32(r);
    mdec->input_full = psx_sr_i32(r);
    mdec->output_empty = psx_sr_i32(r);
    mdec->enable_dma0 = psx_sr_i32(r);
    mdec->enable_dma1 = psx_sr_i32(r);
    mdec->recv_color = psx_sr_i32(r);
    mdec->status = psx_sr_u32(r);

    psx_sr_bytes(r, mdec->uv_quant_table, MDEC_QUANT_TABLE_SIZE);
    psx_sr_bytes(r, mdec->y_quant_table, MDEC_QUANT_TABLE_SIZE);
    psx_sr_i16_array(r, mdec->scale_table, MDEC_SCALE_TABLE_SIZE);

    psx_sr_i16_array(r, mdec->yblk, 64);
    psx_sr_i16_array(r, mdec->crblk, 64);
    psx_sr_i16_array(r, mdec->cbblk, 64);

    input_size = psx_sr_u32(r);
    input_words = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (input_words > (0x10000u)) /* the parameter count is a 16-bit field */
        return PSX_STATE_ERR_TRUNCATED;

    /* Reallocate rather than reuse: the previous buffer may already have been
       free()d by the running machine, leaving a dangling pointer behind. */
    mdec->input = NULL;
    mdec->input_size = input_size;
    mdec->input_index = 0;

    if (mdec->words_remaining && input_size) {
        mdec->input = (uint32_t*)malloc(input_size);

        if (!mdec->input)
            return PSX_STATE_ERR_IO;

        memset(mdec->input, 0, input_size);

        for (i = 0; i < input_words; i++)
            mdec->input[i] = psx_sr_u32(r);

        mdec->input_index = (int)input_words;
    } else {
        for (i = 0; i < input_words; i++)
            (void)psx_sr_u32(r);
    }

    output_words = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (output_words > (1u << 20))
        return PSX_STATE_ERR_TRUNCATED;

    free(mdec->output);
    mdec->output = NULL;
    mdec->output_index = 0;
    mdec->output_words_remaining = output_words;

    if (output_words) {
        mdec->output = (uint8_t*)malloc((size_t)output_words * sizeof(uint32_t));

        if (!mdec->output)
            return PSX_STATE_ERR_IO;

        for (i = 0; i < output_words; i++)
            ((uint32_t*)mdec->output)[i] = psx_sr_u32(r);
    }

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_mdec_destroy(psx_mdec_t* mdec) {
    free(mdec);
}

#undef CLAMP
#undef MAX