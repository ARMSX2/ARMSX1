#include "psx/dev/mdec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            fprintf(stderr, "MDEC_BOUNDS failed line=%d expression=%s\n", __LINE__, #expr); \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static psx_mdec_t* new_mdec(void) {
    psx_mdec_t* mdec = psx_mdec_create();

    CHECK(mdec != NULL);
    psx_mdec_init(mdec);
    return mdec;
}

static void write_halfwords(psx_mdec_t* mdec, uint32_t command,
                            const uint16_t* values, size_t count) {
    CHECK((count & 1u) == 0);
    CHECK(count / 2u <= 0xffffu);

    psx_mdec_write32(mdec, 0, command | (uint32_t)(count / 2u));

    for (size_t i = 0; i < count; i += 2) {
        uint32_t word = values[i] | ((uint32_t)values[i + 1] << 16u);

        psx_mdec_write32(mdec, 0, word);
    }
}

static void write_decode(psx_mdec_t* mdec, unsigned depth,
                         const uint16_t* values, size_t count) {
    write_halfwords(mdec, UINT32_C(1) << 29u | (uint32_t)depth << 27u, values, count);
}

static void fill_minimal_blocks(uint16_t* values, size_t blocks, uint16_t terminator) {
    for (size_t i = 0; i < blocks; i++) {
        values[i * 2u] = 0;
        values[i * 2u + 1u] = terminator;
    }
}

static void expect_empty(const psx_mdec_t* mdec) {
    CHECK(mdec->output == NULL);
    CHECK(mdec->output_words_remaining == 0);
    CHECK(mdec->output_empty == 1);
    CHECK(mdec->output_request == 0);
}

static void test_exact_color_macroblock(void) {
    uint16_t stream[12];
    psx_mdec_t* mdec = new_mdec();

    fill_minimal_blocks(stream, 6, 0xfe00);
    write_decode(mdec, 2, stream, 12);

    CHECK(mdec->output != NULL);
    CHECK(mdec->output_words_remaining == 192);
    CHECK(mdec->output_empty == 0);

    for (unsigned i = 0; i < 192; i++)
        CHECK(psx_mdec_read32(mdec, 0) == UINT32_C(0x80808080));

    CHECK(mdec->output_words_remaining == 0);
    CHECK(mdec->output_empty == 1);
    CHECK((psx_mdec_read32(mdec, 4) >> 31u) == 1u);
    psx_mdec_destroy(mdec);
}

static void test_partial_color_macroblocks(void) {
    for (size_t blocks = 1; blocks < 6; blocks++) {
        uint16_t stream[10];
        psx_mdec_t* mdec = new_mdec();

        fill_minimal_blocks(stream, blocks, 0xfe00);
        write_decode(mdec, 2, stream, blocks * 2u);
        expect_empty(mdec);
        psx_mdec_destroy(mdec);
    }
}

static void test_complete_then_partial(void) {
    uint16_t stream[14];
    psx_mdec_t* mdec = new_mdec();

    fill_minimal_blocks(stream, 6, 0xfe00);
    stream[12] = 0;
    stream[13] = 0; /* coefficient 1, then the command ends without an EOB */
    write_decode(mdec, 2, stream, 14);

    CHECK(mdec->output != NULL);
    CHECK(mdec->output_words_remaining == 192);
    psx_mdec_destroy(mdec);
}

static void test_padding_only(void) {
    const uint16_t stream[] = {0xfe00, 0xfe00};
    psx_mdec_t* mdec = new_mdec();

    write_decode(mdec, 2, stream, sizeof(stream) / sizeof(stream[0]));
    expect_empty(mdec);
    psx_mdec_destroy(mdec);
}

static void test_oversized_runs(void) {
    uint16_t stream[12];
    psx_mdec_t* mdec = new_mdec();

    /* Run=63 advances k=0 to 64. The value bits must never be used to index quant[64]. */
    fill_minimal_blocks(stream, 6, 0xfc01);
    write_decode(mdec, 2, stream, 12);
    CHECK(mdec->output_words_remaining == 192);
    psx_mdec_destroy(mdec);

    mdec = new_mdec();
    /* Run=62 lands exactly on coefficient 63, which is stored and completes the block. */
    fill_minimal_blocks(stream, 6, 0xf801);
    write_decode(mdec, 3, stream, 12);
    CHECK(mdec->output_words_remaining == 128);
    psx_mdec_destroy(mdec);
}

static void test_mono_depths(void) {
    const uint16_t stream[] = {0, 0xfe00};
    psx_mdec_t* mdec = new_mdec();

    write_decode(mdec, 1, stream, 2);
    CHECK(mdec->output_words_remaining == 16);
    for (unsigned i = 0; i < 16; i++)
        CHECK(psx_mdec_read32(mdec, 0) == UINT32_C(0x80808080));
    psx_mdec_destroy(mdec);

    mdec = new_mdec();
    write_decode(mdec, 0, stream, 2);
    CHECK(mdec->output_words_remaining == 8);
    for (unsigned i = 0; i < 8; i++)
        CHECK(psx_mdec_read32(mdec, 0) == UINT32_C(0x88888888));
    psx_mdec_destroy(mdec);
}

static void test_parameter_endianness(void) {
    uint16_t quant_halfwords[64];
    uint16_t scale_halfwords[64];
    psx_mdec_t* mdec = new_mdec();

    for (unsigned i = 0; i < 64; i++) {
        unsigned byte = i * 2u;
        quant_halfwords[i] = (uint16_t)(byte | ((byte + 1u) << 8u));
        scale_halfwords[i] = (uint16_t)(0x8000u + i);
    }

    write_halfwords(mdec, (UINT32_C(2) << 29u) | 1u, quant_halfwords, 64);
    for (unsigned i = 0; i < 64; i++) {
        CHECK(mdec->y_quant_table[i] == i);
        CHECK(mdec->uv_quant_table[i] == 64u + i);
    }

    write_halfwords(mdec, UINT32_C(3) << 29u, scale_halfwords, 64);
    for (unsigned i = 0; i < 64; i++)
        CHECK(mdec->scale_table[i] == (int16_t)(0x8000u + i));

    psx_mdec_destroy(mdec);
}

static void test_reset_drops_live_buffers(void) {
    const uint16_t stream[] = {0, 0xfe00};
    psx_mdec_t* mdec = new_mdec();

    write_decode(mdec, 1, stream, 2);
    CHECK(mdec->output != NULL);
    psx_mdec_write32(mdec, 4, UINT32_C(0x80000000));
    expect_empty(mdec);
    CHECK(mdec->input == NULL);
    CHECK(mdec->input_size == 0);
    psx_mdec_destroy(mdec);
}

int main(void) {
    test_exact_color_macroblock();
    test_partial_color_macroblocks();
    test_complete_then_partial();
    test_padding_only();
    test_oversized_runs();
    test_mono_depths();
    test_parameter_endianness();
    test_reset_drops_live_buffers();

    puts("MDEC_BOUNDS all cases passed");
    return 0;
}
