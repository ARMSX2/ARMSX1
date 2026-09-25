/* Memory-card snapshot cache and truncation regression; no user card files. */
#include "psx/dev/mcd.h"
#include "psx/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int restore(psx_mcd_t* card, psx_state_writer_t* w, size_t size) {
    psx_state_reader_t r;
    psx_sr_init(&r, w->buf, size);
    int result = psx_mcd_load_state(card, &r);
    if (result == PSX_STATE_OK) assert(r.offset == w->size && !r.error);
    return result;
}

int main(int argc, char** argv) {
    psx_mcd_t* card = psx_mcd_create();
    assert(card && psx_mcd_init(card, NULL) == 0);
    for (size_t i = 0; i < MCD_MEMORY_SIZE; ++i) card->buf[i] = (uint8_t)(i * 17 + i / 257);
    card->addr = 123;
    card->checksum = 42;
    card->write_generation = 77;
    const uint64_t session = card->session_id;
    const uint64_t hash = psx_mcd_content_hash(card);
    psx_state_writer_t w;
    psx_sw_init(&w);
    psx_mcd_save_state(card, &w);
    assert(!w.error && w.size > MCD_MEMORY_SIZE);

    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        clock_t start = clock();
        for (int i = 0; i < 5000; ++i) {
            assert(restore(card, &w, w.size) == PSX_STATE_OK);
            assert(psx_mcd_content_hash(card) == hash);
        }
        printf("MCARD_RESTORE benchmark %.6f ms/load+fingerprint\n",
               1000.0 * (clock() - start) / CLOCKS_PER_SEC / 5000);
    } else {
        card->addr = 9;
        card->checksum = 0;
        card->dirty = 0;
        card->flush_cycles = 123;
        assert(restore(card, &w, w.size) == PSX_STATE_OK);
        assert(card->addr == 123 && card->checksum == 42);
        assert(card->dirty && card->flush_cycles == 0);
        assert(card->hash_valid && card->hash_cached == hash);
        assert(card->session_id == session && card->write_generation == 77);

        const size_t positions[] = {0, MCD_MEMORY_SIZE / 2, MCD_MEMORY_SIZE - 1};
        for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
            card->buf[positions[i]] ^= 1;
            card->hash_valid = 0;
            card->dirty = 0;
            card->flush_cycles = 123;
            assert(psx_mcd_content_hash(card) != hash);
            assert(restore(card, &w, w.size) == PSX_STATE_OK);
            assert(card->dirty && card->flush_cycles == 0);
            assert(!card->hash_valid && psx_mcd_content_hash(card) == hash);
            assert(card->write_generation == 77);
        }
        card->hash_valid = 0;
        assert(restore(card, &w, w.size) == PSX_STATE_OK);
        assert(!card->hash_valid && psx_mcd_content_hash(card) == hash);

        const size_t header = w.size - MCD_MEMORY_SIZE;
        const size_t lengths[] = {header, header + 1, w.size - 1};
        for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            assert(restore(card, &w, lengths[i]) == PSX_STATE_ERR_TRUNCATED);
            assert(card->hash_valid && card->hash_cached == hash);
            assert(memcmp(card->buf, w.buf + header, MCD_MEMORY_SIZE) == 0);
        }

        card->buf[0] ^= 1;
        card->hash_valid = 0;
        uint64_t changed = psx_mcd_content_hash(card);
        psx_mcd_set_state_restores_image(0);
        card->addr = 9;
        assert(restore(card, &w, w.size) == PSX_STATE_OK);
        assert(card->addr == 123 && card->hash_valid);
        assert(psx_mcd_content_hash(card) == changed);
        assert(restore(card, &w, w.size - 1) == PSX_STATE_ERR_TRUNCATED);
        assert(psx_mcd_content_hash(card) == changed);
        psx_mcd_set_state_restores_image(1);
        assert(restore(card, &w, w.size) == PSX_STATE_OK);
        assert(psx_mcd_content_hash(card) == hash);
        printf("MCARD_RESTORE all cases passed\n");
    }
    psx_sw_free(&w);
    psx_mcd_destroy(card);
    return 0;
}
