/*
    ARMSX PS1 core — save states. Container format is documented in state.h.
*/

#include "state.h"
#include "psx.h"
#include "bus_init.h"
#include "log.h"
#include "pgxp.h"
#include "rewind.h"
#include "thumbnail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#endif

static void state_mkdir(const char* path) {
    struct stat info;

    if (!path || !*path)
        return;

    if (stat(path, &info) == 0)
        return;

#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

#if defined(_WIN32)
/* Declared by hand so the core does not have to pull in windows.h. */
__declspec(dllimport) void __stdcall Sleep(unsigned long);
#define PSX_STATE_SLEEP_MS(ms) Sleep((unsigned long)(ms))
#else
#include <time.h>
#define PSX_STATE_SLEEP_MS(ms)                                   \
    do {                                                         \
        struct timespec psx_state_ts;                            \
        psx_state_ts.tv_sec = (ms) / 1000;                       \
        psx_state_ts.tv_nsec = ((long)((ms) % 1000)) * 1000000L; \
        nanosleep(&psx_state_ts, NULL);                          \
    } while (0)
#endif

#if !defined(__STDC_NO_ATOMICS__) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#include <stdatomic.h>
#define PSX_STATE_ATOMIC_INT _Atomic int
#define PSX_STATE_LOAD(p) atomic_load_explicit(&(p), memory_order_acquire)
#define PSX_STATE_STORE(p, v) atomic_store_explicit(&(p), (v), memory_order_release)
#define PSX_STATE_CAS(p, expected_var, desired) \
    atomic_compare_exchange_strong_explicit(&(p), &(expected_var), (desired), memory_order_acq_rel, memory_order_acquire)
#else
/* No C11 atomics: fall back to volatile. The request word is a single int
   written by one producer and consumed by one consumer, so the worst case is a
   missed or duplicated wake-up, not a torn value. */
#define PSX_STATE_ATOMIC_INT volatile int
#define PSX_STATE_LOAD(p) (p)
#define PSX_STATE_STORE(p, v) ((p) = (v))
#define PSX_STATE_CAS(p, expected_var, desired) \
    (((p) == (expected_var)) ? (((p) = (desired)), 1) : ((expected_var) = (p), 0))
#endif

/* -------------------------------------------------------------------------- */
/* Writer                                                                     */
/* -------------------------------------------------------------------------- */

void psx_sw_init(psx_state_writer_t* w) {
    w->buf = NULL;
    w->size = 0;
    w->capacity = 0;
    w->error = 0;
}

void psx_sw_adopt(psx_state_writer_t* w, void* buf, size_t capacity) {
    w->buf = (uint8_t*)buf;
    w->size = 0;
    w->capacity = buf ? capacity : 0;
    w->error = 0;
}

void psx_sw_free(psx_state_writer_t* w) {
    free(w->buf);
    w->buf = NULL;
    w->size = 0;
    w->capacity = 0;
}

static int psx_sw_reserve(psx_state_writer_t* w, size_t extra) {
    size_t needed;
    size_t capacity;
    uint8_t* buf;

    if (w->error)
        return 0;

    needed = w->size + extra;

    if (needed <= w->capacity)
        return 1;

    capacity = w->capacity ? w->capacity : 65536;

    while (capacity < needed)
        capacity *= 2;

    buf = (uint8_t*)realloc(w->buf, capacity);

    if (!buf) {
        w->error = 1;
        return 0;
    }

    w->buf = buf;
    w->capacity = capacity;

    return 1;
}

void psx_sw_bytes(psx_state_writer_t* w, const void* data, size_t size) {
    if (!size)
        return;

    if (!psx_sw_reserve(w, size))
        return;

    memcpy(w->buf + w->size, data, size);

    w->size += size;
}

void psx_sw_u8(psx_state_writer_t* w, uint8_t v) {
    if (!psx_sw_reserve(w, 1))
        return;

    w->buf[w->size++] = v;
}

void psx_sw_u16(psx_state_writer_t* w, uint16_t v) {
    if (!psx_sw_reserve(w, 2))
        return;

    w->buf[w->size++] = (uint8_t)(v & 0xff);
    w->buf[w->size++] = (uint8_t)((v >> 8) & 0xff);
}

void psx_sw_u32(psx_state_writer_t* w, uint32_t v) {
    if (!psx_sw_reserve(w, 4))
        return;

    w->buf[w->size++] = (uint8_t)(v & 0xff);
    w->buf[w->size++] = (uint8_t)((v >> 8) & 0xff);
    w->buf[w->size++] = (uint8_t)((v >> 16) & 0xff);
    w->buf[w->size++] = (uint8_t)((v >> 24) & 0xff);
}

void psx_sw_u64(psx_state_writer_t* w, uint64_t v) {
    int i;

    if (!psx_sw_reserve(w, 8))
        return;

    for (i = 0; i < 8; i++)
        w->buf[w->size++] = (uint8_t)((v >> (i * 8)) & 0xff);
}

void psx_sw_i32(psx_state_writer_t* w, int32_t v) {
    psx_sw_u32(w, (uint32_t)v);
}

void psx_sw_i64(psx_state_writer_t* w, int64_t v) {
    psx_sw_u64(w, (uint64_t)v);
}

void psx_sw_f32(psx_state_writer_t* w, float v) {
    uint32_t bits;

    /* IEEE-754 binary32 bit pattern, not the host's float representation as a
       raw object. See the portability note in state.h. */
    memcpy(&bits, &v, sizeof(bits));

    psx_sw_u32(w, bits);
}

void psx_sw_u16_array(psx_state_writer_t* w, const uint16_t* data, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        psx_sw_u16(w, data[i]);
}

void psx_sw_i16_array(psx_state_writer_t* w, const int16_t* data, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        psx_sw_u16(w, (uint16_t)data[i]);
}

void psx_sw_u32_array(psx_state_writer_t* w, const uint32_t* data, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        psx_sw_u32(w, data[i]);
}

void psx_sw_i32_array(psx_state_writer_t* w, const int32_t* data, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        psx_sw_u32(w, (uint32_t)data[i]);
}

/* -------------------------------------------------------------------------- */
/* Reader                                                                     */
/* -------------------------------------------------------------------------- */

void psx_sr_init(psx_state_reader_t* r, const void* data, size_t size) {
    r->buf = (const uint8_t*)data;
    r->size = size;
    r->offset = 0;
    r->error = 0;
}

static int psx_sr_want(psx_state_reader_t* r, size_t size) {
    if (r->error)
        return 0;

    if ((r->size - r->offset) < size) {
        r->error = 1;
        return 0;
    }

    return 1;
}

void psx_sr_bytes(psx_state_reader_t* r, void* out, size_t size) {
    if (!size)
        return;

    if (!psx_sr_want(r, size)) {
        memset(out, 0, size);
        return;
    }

    memcpy(out, r->buf + r->offset, size);

    r->offset += size;
}

void psx_sr_skip(psx_state_reader_t* r, size_t size) {
    if (!psx_sr_want(r, size))
        return;

    r->offset += size;
}

uint8_t psx_sr_u8(psx_state_reader_t* r) {
    if (!psx_sr_want(r, 1))
        return 0;

    return r->buf[r->offset++];
}

uint16_t psx_sr_u16(psx_state_reader_t* r) {
    uint16_t v;

    if (!psx_sr_want(r, 2))
        return 0;

    v = (uint16_t)r->buf[r->offset];
    v |= (uint16_t)((uint16_t)r->buf[r->offset + 1] << 8);
    r->offset += 2;

    return v;
}

uint32_t psx_sr_u32(psx_state_reader_t* r) {
    uint32_t v;

    if (!psx_sr_want(r, 4))
        return 0;

    v = (uint32_t)r->buf[r->offset];
    v |= (uint32_t)r->buf[r->offset + 1] << 8;
    v |= (uint32_t)r->buf[r->offset + 2] << 16;
    v |= (uint32_t)r->buf[r->offset + 3] << 24;
    r->offset += 4;

    return v;
}

uint64_t psx_sr_u64(psx_state_reader_t* r) {
    uint64_t v = 0;
    int i;

    if (!psx_sr_want(r, 8))
        return 0;

    for (i = 0; i < 8; i++)
        v |= (uint64_t)r->buf[r->offset + i] << (i * 8);

    r->offset += 8;

    return v;
}

int32_t psx_sr_i32(psx_state_reader_t* r) {
    return (int32_t)psx_sr_u32(r);
}

int64_t psx_sr_i64(psx_state_reader_t* r) {
    return (int64_t)psx_sr_u64(r);
}

float psx_sr_f32(psx_state_reader_t* r) {
    uint32_t bits = psx_sr_u32(r);
    float v;

    memcpy(&v, &bits, sizeof(v));

    return v;
}

void psx_sr_u16_array(psx_state_reader_t* r, uint16_t* out, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        out[i] = psx_sr_u16(r);
}

void psx_sr_i16_array(psx_state_reader_t* r, int16_t* out, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        out[i] = (int16_t)psx_sr_u16(r);
}

void psx_sr_u32_array(psx_state_reader_t* r, uint32_t* out, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        out[i] = psx_sr_u32(r);
}

void psx_sr_i32_array(psx_state_reader_t* r, int32_t* out, size_t count) {
    size_t i;

    for (i = 0; i < count; i++)
        out[i] = (int32_t)psx_sr_u32(r);
}

/* -------------------------------------------------------------------------- */

uint64_t psx_state_fnv1a(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t hash = seed;
    size_t i;

    for (i = 0; i < size; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ull;
    }

    return hash;
}

const char* psx_state_strerror(int code) {
    switch (code) {
        case PSX_STATE_OK: return "ok";
        case PSX_STATE_ERR_ARG: return "invalid argument";
        case PSX_STATE_ERR_IO: return "file I/O error";
        case PSX_STATE_ERR_MAGIC: return "not an ARMSX save state";
        case PSX_STATE_ERR_VERSION: return "save state was made by an incompatible core version";
        case PSX_STATE_ERR_TRUNCATED: return "save state is truncated or corrupt";
        case PSX_STATE_ERR_MISSING: return "save state is missing a required section";
        case PSX_STATE_ERR_GEOMETRY: return "save state was made with a different memory configuration";
        case PSX_STATE_ERR_NO_DISC: return "save state needs a disc, but none is inserted";
        case PSX_STATE_ERR_WRONG_DISC: return "save state belongs to a different disc";
        case PSX_STATE_ERR_WRONG_BIOS: return "save state was made with a different BIOS";
        case PSX_STATE_ERR_UNSUPPORTED: return "this machine state cannot be captured";
        case PSX_STATE_ERR_NO_MACHINE: return "no emulated machine is running";
        case PSX_STATE_ERR_BUSY: return "another save state request is already pending";
        case PSX_STATE_ERR_TIMEOUT: return "the emulation thread did not service the request";
        case PSX_STATE_ERR_CARD_NEWER: return "the memory card has been saved to since this state was made";
        case PSX_STATE_ERR_CARD_DIVERGED: return "the memory card no longer matches this state";
        default: return "unknown error";
    }
}

/* -------------------------------------------------------------------------- */
/* Section framing                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t id;
    uint32_t version;
    const uint8_t* data;
    size_t size;
} psx_state_section_t;

#define PSX_STATE_MAX_SECTIONS 64

static size_t state_begin_section(psx_state_writer_t* w, uint32_t id, uint32_t version) {
    size_t length_offset;

    psx_sw_u32(w, id);
    psx_sw_u32(w, version);

    length_offset = w->size;

    psx_sw_u64(w, 0);

    return length_offset;
}

static void state_end_section(psx_state_writer_t* w, size_t length_offset) {
    uint64_t length;
    int i;

    if (w->error || (length_offset + 8) > w->size)
        return;

    length = (uint64_t)(w->size - (length_offset + 8));

    for (i = 0; i < 8; i++)
        w->buf[length_offset + i] = (uint8_t)((length >> (i * 8)) & 0xff);
}

static const psx_state_section_t* state_find_section(
    const psx_state_section_t* sections, int count, uint32_t id
) {
    int i;

    for (i = 0; i < count; i++)
        if (sections[i].id == id)
            return &sections[i];

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Identity                                                                   */
/* -------------------------------------------------------------------------- */

static uint64_t state_bios_fingerprint(psx_t* psx) {
    if (!psx->bios || !psx->bios->buf || !psx->bios->io_size)
        return 0;

    return psx_state_fnv1a(psx->bios->buf, psx->bios->io_size, PSX_STATE_FNV_SEED);
}

static void state_write_string(psx_state_writer_t* w, const char* s) {
    size_t len = s ? strlen(s) : 0;

    if (len > 1024)
        len = 1024;

    psx_sw_u32(w, (uint32_t)len);
    psx_sw_bytes(w, s, len);
}

static void state_read_string(psx_state_reader_t* r, char* out, size_t out_size) {
    uint32_t len = psx_sr_u32(r);
    uint32_t copy;

    if (len > 1024) {
        r->error = 1;
        if (out_size)
            out[0] = '\0';
        return;
    }

    copy = len;

    if (out_size && copy >= out_size)
        copy = (uint32_t)(out_size - 1);

    if (out_size) {
        psx_sr_bytes(r, out, copy);
        out[copy] = '\0';
    }

    psx_sr_skip(r, len - copy);
}

/* -------------------------------------------------------------------------- */
/* Memory-card fingerprints                                                   */
/* -------------------------------------------------------------------------- */

/*
    The problem this exists for.

    A save state and a memory card are two independent timelines of the same
    playthrough, and the player can move one without the other. Save a state,
    keep playing, save to the card in-game, then load that old state: the
    console is now behind its own card. A real PS1 game notices — the save file
    it is told to expect does not match the directory it cached, and depending
    on the title it copes, refuses to load its own save, or misbehaves in ways
    that look like corruption. In this core it is worse than an inconsistency:
    psx_mcd_load_state() restores the card image along with the machine, and
    psx_mcd_destroy() writes that rewound image back out at shutdown, so the
    in-game save made after the state is quietly discarded.

    None of that is detectable after the fact, which is why the state records
    what the cards looked like when it was taken.

    What is compared, and why it is the LIVE card and not the file: the image is
    only flushed to disk when the card is destroyed, so a mid-session in-game
    save changes psx_mcd_t::buf and leaves slot1.mcd untouched. Comparing files
    would find nothing in exactly the case that matters.
*/

typedef struct {
    int present;
    uint64_t content_hash;
    uint64_t session_id;
    uint32_t write_generation;
    int64_t file_mtime;
} state_mcard_fp_t;

static void state_mcard_capture(psx_t* psx, state_mcard_fp_t* out) {
    uint32_t i;

    for (i = 0; i < PSX_STATE_MCARD_SLOTS; i++) {
        out[i].present = 0;
        out[i].content_hash = 0;
        out[i].session_id = 0;
        out[i].write_generation = 0;
        out[i].file_mtime = 0;

        if (!psx || !psx->pad)
            continue;

        out[i].present = psx_pad_mcd_fingerprint(psx->pad, (int)i,
                                                 &out[i].content_hash,
                                                 &out[i].session_id,
                                                 &out[i].write_generation,
                                                 &out[i].file_mtime);
    }
}

/* Worst verdict wins across the slots: PSX_STATE_OK, then ERR_CARD_DIVERGED,
   then ERR_CARD_NEWER. Pure function of the two fingerprints — no I/O, nothing
   mutated — so the gate in tests/state_mcard_divergence.c can drive it directly. */
static int state_mcard_verdict_slot(const state_mcard_fp_t* recorded,
                                    const state_mcard_fp_t* live) {
    /* Neither run had a card here: nothing to disagree about. */
    if (!recorded->present && !live->present)
        return PSX_STATE_OK;

    /* A card appeared or vanished between capture and load. Something changed,
       but a presence flag says nothing about which timeline is ahead. */
    if (recorded->present != live->present)
        return PSX_STATE_ERR_CARD_DIVERGED;

    /* The decisive test, and deliberately first: identical contents mean the
       card holds exactly what the state expects, whatever the counters say.
       This is what stops a second load of the same state from asking twice —
       the first load rewound the image to match. */
    if (recorded->content_hash == live->content_hash)
        return PSX_STATE_OK;

    /* Contents differ. Which way? */
    if (recorded->session_id && recorded->session_id == live->session_id) {
        /* Same attach of the same card, so the generations are comparable and
           this is a real answer rather than an inference. */
        if (live->write_generation > recorded->write_generation)
            return PSX_STATE_ERR_CARD_NEWER;

        /* Fewer writes than the state remembers, or the same count with
           different bytes: the card was rewound or rewritten underneath us.
           Different, but not the time-travel case. */
        return PSX_STATE_ERR_CARD_DIVERGED;
    }

    /*
        Different attach: the generations are not comparable, and nothing else
        here can establish direction — so this is the soft verdict, always.

        file_mtime is recorded (and is worth having in a bug report) but is
        deliberately NOT consulted, because it cannot tell the two cross-session
        stories apart. "The same card moved forward" and "a different card was
        imported or erased in the card manager" both leave the file newer than
        the state, and only the first is the time-travel case. Promoting on
        mtime would state the strong warning — the one that says the game saved
        after this state — about a card the game never touched.

        It is also the wrong resolution for the question: st_mtime is whole
        seconds, and the image is only flushed to disk when the card is
        destroyed, so within a session it does not move at all.

        Saying "something changed, I can't tell which way" is both true and
        still actionable: the user is asked, and the dialog says what is unknown.
    */
    return PSX_STATE_ERR_CARD_DIVERGED;
}

static int state_mcard_verdict(const state_mcard_fp_t* recorded,
                               const state_mcard_fp_t* live) {
    int worst = PSX_STATE_OK;
    uint32_t i;

    for (i = 0; i < PSX_STATE_MCARD_SLOTS; i++) {
        int slot_verdict = state_mcard_verdict_slot(&recorded[i], &live[i]);

        if (slot_verdict == PSX_STATE_ERR_CARD_NEWER)
            return PSX_STATE_ERR_CARD_NEWER;

        if (slot_verdict == PSX_STATE_ERR_CARD_DIVERGED)
            worst = PSX_STATE_ERR_CARD_DIVERGED;
    }

    return worst;
}

/* -------------------------------------------------------------------------- */
/* Save                                                                       */
/* -------------------------------------------------------------------------- */

#define STATE_SECTION(w, id, version, body)                     \
    do {                                                        \
        size_t psx_state_len_off = state_begin_section(w, id, version); \
        body;                                                   \
        state_end_section(w, psx_state_len_off);                \
        section_count++;                                        \
    } while (0)

int psx_save_state_to_memory_ex(psx_t* psx, void** io_data, size_t* io_capacity,
                                size_t* out_size, unsigned flags) {
    psx_state_writer_t w;
    uint32_t section_count = 0;
    int i;

    if (!psx || !io_data || !io_capacity || !out_size)
        return PSX_STATE_ERR_ARG;

    *out_size = 0;

    /* Nothing in this core is un-capturable today, but keep the refusal path
       explicit: a machine without a CPU or RAM is not a machine. */
    if (!psx->cpu || !psx->ram || !psx->ram->buf || !psx->gpu || !psx->gpu->vram ||
        !psx->spu || !psx->spu->ram || !psx->cdrom)
        return PSX_STATE_ERR_UNSUPPORTED;

    /* Reuses the caller's buffer when there is one. On the rewind path that is
       the whole point: a snapshot every half second must not also be a 4 MiB
       malloc/free every half second. */
    psx_sw_adopt(&w, *io_data, *io_capacity);

    /* Header. section_count is patched once every section has been emitted. */
    psx_sw_u32(&w, PSX_STATE_MAGIC0);
    psx_sw_u32(&w, PSX_STATE_MAGIC1);
    psx_sw_u32(&w, PSX_STATE_FORMAT_VERSION);
    psx_sw_u32(&w, PSX_STATE_CORE_ABI);
    psx_sw_u32(&w, 0);
    psx_sw_u32(&w, PSX_STATE_HEADER_SIZE);
    psx_sw_u64(&w, 0);

    STATE_SECTION(&w, PSX_SS_IDENTITY, 1, {
        psx_sw_u64(&w, state_bios_fingerprint(psx));
        psx_sw_u64(&w, psx_cdrom_get_disc_fingerprint(psx->cdrom));
        psx_sw_i32(&w, psx_cdrom_get_disc_track_count(psx->cdrom));
        psx_sw_u32(&w, (uint32_t)psx->ram->size);
        state_write_string(&w, psx_cdrom_get_disc_path(psx->cdrom));
        state_write_string(&w, PSXE_VERSION);
    });

    /* Memory-card fingerprints. A NEW optional section rather than extra fields
       on the existing mcd payload, and that is the whole compatibility story:
       adding to psx_mcd_save_state() would change a section that already ships,
       which state.h requires be paid for with a PSX_STATE_CORE_ABI bump — and a
       bump rejects every state already sitting on a user's device. A new
       section at version 1 costs nothing in either direction, exactly as
       PSX_SS_THUMB did. It is NOT added to g_psx_state_mandatory. */
    {
        state_mcard_fp_t cards[PSX_STATE_MCARD_SLOTS];
        uint32_t slot;

        state_mcard_capture(psx, cards);

        STATE_SECTION(&w, PSX_SS_MCARD, 1, {
            psx_sw_u32(&w, PSX_STATE_MCARD_SLOTS);

            for (slot = 0; slot < PSX_STATE_MCARD_SLOTS; slot++) {
                psx_sw_u8(&w, (uint8_t)(cards[slot].present ? 1 : 0));
                psx_sw_u64(&w, cards[slot].content_hash);
                psx_sw_u64(&w, cards[slot].session_id);
                psx_sw_u32(&w, cards[slot].write_generation);
                psx_sw_i64(&w, cards[slot].file_mtime);
            }
        });
    }

    /* Preview image. Written here, near the front of the file, so a picker can
       reach it after one seek instead of walking past several megabytes of RAM
       and VRAM. Optional in every sense: the capture is allowed to fail (no
       display, an allocation that did not come back), and a state without the
       section is a perfectly ordinary state — see the compatibility note in
       state.h. It is NOT added to g_psx_state_mandatory. */
    if (!(flags & PSX_STATE_SAVE_NO_THUMBNAIL)) {
        void* thumbnail = NULL;
        size_t thumbnail_size = 0;
        int thumbnail_width = 0;
        int thumbnail_height = 0;

        /* The capture reports the downscaled size it settled on, so there is no
           second pass to re-read it back out of the PNG header. */
        if (psx_thumbnail_capture_png(psx, &thumbnail, &thumbnail_size,
                                      &thumbnail_width, &thumbnail_height) == 0 &&
            thumbnail && thumbnail_size) {
            STATE_SECTION(&w, PSX_SS_THUMB, 1, {
                psx_sw_u32(&w, PSX_STATE_THUMB_PNG);
                psx_sw_u32(&w, (uint32_t)thumbnail_width);
                psx_sw_u32(&w, (uint32_t)thumbnail_height);
                psx_sw_u32(&w, (uint32_t)thumbnail_size);
                psx_sw_bytes(&w, thumbnail, thumbnail_size);
            });
        }

        free(thumbnail);
    }

    STATE_SECTION(&w, PSX_SS_CPU, 1, { psx_cpu_save_state(psx->cpu, &w); });
    STATE_SECTION(&w, PSX_SS_BUS, 1, { psx_bus_save_state(psx->bus, &w); });
    STATE_SECTION(&w, PSX_SS_RAM, 1, { psx_ram_save_state(psx->ram, &w); });
    STATE_SECTION(&w, PSX_SS_SCRATCHPAD, 1, { psx_scratchpad_save_state(psx->scratchpad, &w); });
    STATE_SECTION(&w, PSX_SS_MC1, 1, { psx_mc1_save_state(psx->mc1, &w); });
    STATE_SECTION(&w, PSX_SS_MC2, 1, { psx_mc2_save_state(psx->mc2, &w); });
    STATE_SECTION(&w, PSX_SS_MC3, 1, { psx_mc3_save_state(psx->mc3, &w); });
    STATE_SECTION(&w, PSX_SS_IC, 1, { psx_ic_save_state(psx->ic, &w); });
    STATE_SECTION(&w, PSX_SS_DMA, 1, { psx_dma_save_state(psx->dma, &w); });
    STATE_SECTION(&w, PSX_SS_GPU, 1, { psx_gpu_save_state(psx->gpu, &w); });
    STATE_SECTION(&w, PSX_SS_SPU, 1, { psx_spu_save_state(psx->spu, &w); });
    STATE_SECTION(&w, PSX_SS_TIMER, 1, { psx_timer_save_state(psx->timer, &w); });
    STATE_SECTION(&w, PSX_SS_CDROM, 1, { psx_cdrom_save_state(psx->cdrom, &w); });
    STATE_SECTION(&w, PSX_SS_PAD, 1, { psx_pad_save_state(psx->pad, &w); });
    STATE_SECTION(&w, PSX_SS_MDEC, 1, { psx_mdec_save_state(psx->mdec, &w); });
    STATE_SECTION(&w, PSX_SS_EXP2, 1, { psx_exp2_save_state(psx->exp2, &w); });

    if (w.error) {
        psx_sw_free(&w);
        *io_data = NULL;
        *io_capacity = 0;
        return PSX_STATE_ERR_IO;
    }

    for (i = 0; i < 4; i++)
        w.buf[16 + i] = (uint8_t)((section_count >> (i * 8)) & 0xff);

    *io_data = w.buf;
    *io_capacity = w.capacity;
    *out_size = w.size;

    return PSX_STATE_OK;
}

int psx_save_state_to_memory(psx_t* psx, void** out_data, size_t* out_size) {
    void* data = NULL;
    size_t capacity = 0;
    int result;

    if (!out_data || !out_size)
        return PSX_STATE_ERR_ARG;

    *out_data = NULL;
    *out_size = 0;

    result = psx_save_state_to_memory_ex(psx, &data, &capacity, out_size, 0);

    if (result != PSX_STATE_OK) {
        free(data);
        return result;
    }

    *out_data = data;

    return PSX_STATE_OK;
}

int psx_save_state(psx_t* psx, const char* path) {
    void* data = NULL;
    size_t size = 0;
    FILE* file;
    size_t written;
    int result;

    if (!psx || !path || !*path)
        return PSX_STATE_ERR_ARG;

    result = psx_save_state_to_memory(psx, &data, &size);

    if (result != PSX_STATE_OK)
        return result;

    file = fopen(path, "wb");

    if (!file) {
        free(data);
        return PSX_STATE_ERR_IO;
    }

    written = fwrite(data, 1, size, file);

    fclose(file);
    free(data);

    if (written != size) {
        remove(path);
        return PSX_STATE_ERR_IO;
    }

    log_info("Save state written to %s (%u bytes)", path, (unsigned)size);

    return PSX_STATE_OK;
}

/* -------------------------------------------------------------------------- */
/* Load                                                                       */
/* -------------------------------------------------------------------------- */

static const uint32_t g_psx_state_mandatory[] = {
    PSX_SS_IDENTITY,
    PSX_SS_CPU,
    PSX_SS_BUS,
    PSX_SS_RAM,
    PSX_SS_SCRATCHPAD,
    PSX_SS_MC1,
    PSX_SS_MC2,
    PSX_SS_MC3,
    PSX_SS_IC,
    PSX_SS_DMA,
    PSX_SS_GPU,
    PSX_SS_SPU,
    PSX_SS_TIMER,
    PSX_SS_CDROM,
    PSX_SS_PAD,
    PSX_SS_MDEC,
    PSX_SS_EXP2
};

/* Applied inline rather than through a generic function pointer: calling a
   device's loader through a cast-to-void* signature would be undefined
   behaviour, and this keeps every call type-checked. */
#define STATE_APPLY(id, fn, device)                                                     \
    do {                                                                                \
        const psx_state_section_t* s = state_find_section(sections, section_count, id); \
        psx_state_reader_t sr;                                                          \
        int rc;                                                                         \
        if (!s)                                                                         \
            return PSX_STATE_ERR_MISSING;                                               \
        psx_sr_init(&sr, s->data, s->size);                                             \
        rc = fn(device, &sr);                                                           \
        if (rc != PSX_STATE_OK)                                                         \
            return rc;                                                                  \
        if (sr.error)                                                                   \
            return PSX_STATE_ERR_TRUNCATED;                                             \
    } while (0)

/*
    Reads PSX_SS_MCARD out of an already-indexed container and compares it with
    the cards attached right now.

    Returns PSX_STATE_OK when the state predates this feature (no section), when
    the section is malformed, or when the cards agree. "No fingerprint recorded"
    is deliberately indistinguishable from "they match": a state written before
    this landed can never grow one, and nagging about every such state forever
    would train the user to dismiss the dialog without reading it — at which
    point the warning is worth less than nothing.
*/
static int state_check_mcard_divergence(psx_t* psx,
                                        const psx_state_section_t* sections,
                                        int section_count) {
    const psx_state_section_t* s =
        state_find_section(sections, section_count, PSX_SS_MCARD);
    state_mcard_fp_t recorded[PSX_STATE_MCARD_SLOTS];
    state_mcard_fp_t live[PSX_STATE_MCARD_SLOTS];
    psx_state_reader_t mr;
    uint32_t slot_count;
    uint32_t i;

    if (!s)
        return PSX_STATE_OK;

    psx_sr_init(&mr, s->data, s->size);

    slot_count = psx_sr_u32(&mr);

    for (i = 0; i < PSX_STATE_MCARD_SLOTS; i++) {
        recorded[i].present = 0;
        recorded[i].content_hash = 0;
        recorded[i].session_id = 0;
        recorded[i].write_generation = 0;
        recorded[i].file_mtime = 0;
    }

    for (i = 0; i < slot_count; i++) {
        uint8_t present = psx_sr_u8(&mr);
        uint64_t hash = psx_sr_u64(&mr);
        uint64_t session = psx_sr_u64(&mr);
        uint32_t generation = psx_sr_u32(&mr);
        int64_t mtime = psx_sr_i64(&mr);

        /* A future core may record more slots than this one understands. Read
           past them so the stream stays aligned, but do not invent a comparison
           for a slot we have no card for. */
        if (i >= PSX_STATE_MCARD_SLOTS)
            continue;

        recorded[i].present = present ? 1 : 0;
        recorded[i].content_hash = hash;
        recorded[i].session_id = session;
        recorded[i].write_generation = generation;
        recorded[i].file_mtime = mtime;
    }

    /* A short or corrupt section is not a reason to block a load the user asked
       for — it only means we cannot answer the question. Stay silent. */
    if (mr.error)
        return PSX_STATE_OK;

    state_mcard_capture(psx, live);

    return state_mcard_verdict(recorded, live);
}

int psx_load_state_from_memory(psx_t* psx, const void* data, size_t size) {
    return psx_load_state_from_memory_ex(psx, data, size, 0);
}

int psx_load_state_from_memory_ex(psx_t* psx, const void* data, size_t size, unsigned flags) {
    psx_state_reader_t r;
    psx_state_section_t sections[PSX_STATE_MAX_SECTIONS];
    int section_count = 0;
    uint32_t declared_sections;
    uint32_t header_size;
    uint32_t i;

    if (!psx || !data)
        return PSX_STATE_ERR_ARG;

    if (!psx->cpu || !psx->ram || !psx->ram->buf || !psx->gpu || !psx->gpu->vram ||
        !psx->spu || !psx->spu->ram || !psx->cdrom)
        return PSX_STATE_ERR_UNSUPPORTED;

    if (size < PSX_STATE_HEADER_SIZE)
        return PSX_STATE_ERR_MAGIC;

    psx_sr_init(&r, data, size);

    if (psx_sr_u32(&r) != PSX_STATE_MAGIC0 || psx_sr_u32(&r) != PSX_STATE_MAGIC1)
        return PSX_STATE_ERR_MAGIC;

    if (psx_sr_u32(&r) != PSX_STATE_FORMAT_VERSION)
        return PSX_STATE_ERR_VERSION;

    if (psx_sr_u32(&r) != PSX_STATE_CORE_ABI)
        return PSX_STATE_ERR_VERSION;

    declared_sections = psx_sr_u32(&r);
    header_size = psx_sr_u32(&r);

    if (r.error || header_size < PSX_STATE_HEADER_SIZE || header_size > size)
        return PSX_STATE_ERR_TRUNCATED;

    r.offset = header_size;

    /* Phase 1: index the container. Nothing touches the machine yet. */
    while (r.offset < r.size) {
        uint32_t id;
        uint32_t version;
        uint64_t length;

        if ((r.size - r.offset) < PSX_STATE_SECTION_HEADER_SIZE)
            return PSX_STATE_ERR_TRUNCATED;

        id = psx_sr_u32(&r);
        version = psx_sr_u32(&r);
        length = psx_sr_u64(&r);

        if (r.error)
            return PSX_STATE_ERR_TRUNCATED;

        if (length > (uint64_t)(r.size - r.offset))
            return PSX_STATE_ERR_TRUNCATED;

        if (section_count < PSX_STATE_MAX_SECTIONS) {
            sections[section_count].id = id;
            sections[section_count].version = version;
            sections[section_count].data = r.buf + r.offset;
            sections[section_count].size = (size_t)length;
            section_count++;
        }

        r.offset += (size_t)length;
    }

    if (declared_sections != (uint32_t)section_count)
        return PSX_STATE_ERR_TRUNCATED;

    for (i = 0; i < (uint32_t)(sizeof(g_psx_state_mandatory) / sizeof(g_psx_state_mandatory[0])); i++)
        if (!state_find_section(sections, section_count, g_psx_state_mandatory[i]))
            return PSX_STATE_ERR_MISSING;

    /* Every section this core knows about is at version 1. Refusing an unknown
       version here (rather than mis-parsing it) is the whole point of the TLV. */
    for (i = 0; i < (uint32_t)section_count; i++)
        if (sections[i].version != 1)
            return PSX_STATE_ERR_VERSION;

    /* Phase 1b: identity + geometry. Still nothing written to the machine. */
    {
        const psx_state_section_t* identity =
            state_find_section(sections, section_count, PSX_SS_IDENTITY);
        psx_state_reader_t ir;
        uint64_t bios_fingerprint;
        uint64_t disc_fingerprint;
        int32_t disc_track_count;
        uint32_t ram_size;
        char disc_path[1024];
        char core_version[1024];

        psx_sr_init(&ir, identity->data, identity->size);

        bios_fingerprint = psx_sr_u64(&ir);
        disc_fingerprint = psx_sr_u64(&ir);
        disc_track_count = psx_sr_i32(&ir);
        ram_size = psx_sr_u32(&ir);
        state_read_string(&ir, disc_path, sizeof(disc_path));
        state_read_string(&ir, core_version, sizeof(core_version));

        if (ir.error)
            return PSX_STATE_ERR_TRUNCATED;

        if (bios_fingerprint != state_bios_fingerprint(psx)) {
            log_error("Save state BIOS fingerprint mismatch (state %016llx, machine %016llx)",
                (unsigned long long)bios_fingerprint,
                (unsigned long long)state_bios_fingerprint(psx));

            return PSX_STATE_ERR_WRONG_BIOS;
        }

        if (ram_size != (uint32_t)psx->ram->size)
            return PSX_STATE_ERR_GEOMETRY;

        if (disc_fingerprint) {
            uint64_t current = psx_cdrom_get_disc_fingerprint(psx->cdrom);

            if (!current)
                return PSX_STATE_ERR_NO_DISC;

            if (current != disc_fingerprint ||
                disc_track_count != psx_cdrom_get_disc_track_count(psx->cdrom)) {
                log_error("Save state disc mismatch: state was made on '%s'", disc_path);

                return PSX_STATE_ERR_WRONG_DISC;
            }
        }
    }

    /* Phase 1c: memory cards. Last of the up-front checks and, unlike the ones
       above, ADVISORY — the state is perfectly loadable, the cards have just
       moved on since it was taken. The front-end turns the refusal into a
       question and re-issues with PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE if the
       user says load anyway. Still nothing has been written to the machine, so
       "cancel" really does leave the session exactly as it was. */
    if (!(flags & PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE)) {
        int card_verdict = state_check_mcard_divergence(psx, sections, section_count);

        if (card_verdict != PSX_STATE_OK) {
            log_info("Save state memory-card divergence: %s",
                psx_state_strerror(card_verdict));

            return card_verdict;
        }
    }

    /* Phase 2: apply. From here the machine is being mutated; a failure past
       this point leaves it in a partial state, which is why every check that
       CAN be made up front is made up front. */
    STATE_APPLY(PSX_SS_CPU, psx_cpu_load_state, psx->cpu);
    STATE_APPLY(PSX_SS_BUS, psx_bus_load_state, psx->bus);
    STATE_APPLY(PSX_SS_RAM, psx_ram_load_state, psx->ram);
    STATE_APPLY(PSX_SS_SCRATCHPAD, psx_scratchpad_load_state, psx->scratchpad);
    STATE_APPLY(PSX_SS_MC1, psx_mc1_load_state, psx->mc1);
    STATE_APPLY(PSX_SS_MC2, psx_mc2_load_state, psx->mc2);
    STATE_APPLY(PSX_SS_MC3, psx_mc3_load_state, psx->mc3);
    STATE_APPLY(PSX_SS_IC, psx_ic_load_state, psx->ic);
    STATE_APPLY(PSX_SS_DMA, psx_dma_load_state, psx->dma);
    STATE_APPLY(PSX_SS_GPU, psx_gpu_load_state, psx->gpu);
    STATE_APPLY(PSX_SS_SPU, psx_spu_load_state, psx->spu);
    STATE_APPLY(PSX_SS_TIMER, psx_timer_load_state, psx->timer);
    STATE_APPLY(PSX_SS_CDROM, psx_cdrom_load_state, psx->cdrom);
    STATE_APPLY(PSX_SS_PAD, psx_pad_load_state, psx->pad);
    STATE_APPLY(PSX_SS_MDEC, psx_mdec_load_state, psx->mdec);
    STATE_APPLY(PSX_SS_EXP2, psx_exp2_load_state, psx->exp2);

    /* Derived host-side caches that must not survive the load. The cached
       interpreter keys its blocks on guest addresses whose contents just
       changed wholesale. */
    psx_cpu_invalidate_cache(psx->cpu);

    /* Same reasoning for the PGXP shadows: they mirror RAM/GTE contents that
       were just replaced. Precision degrades to plain integers for the frame
       it takes the GTE to re-transform the scene, by design (pgxp.h). */
    psx_pgxp_reset();

    return PSX_STATE_OK;
}

int psx_load_state(psx_t* psx, const char* path) {
    return psx_load_state_ex(psx, path, 0);
}

int psx_load_state_ex(psx_t* psx, const char* path, unsigned flags) {
    FILE* file;
    long size;
    void* data;
    size_t read;
    int result;

    if (!psx || !path || !*path)
        return PSX_STATE_ERR_ARG;

    file = fopen(path, "rb");

    if (!file)
        return PSX_STATE_ERR_IO;

    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return PSX_STATE_ERR_IO;
    }

    size = ftell(file);

    if (size <= 0) {
        fclose(file);
        return PSX_STATE_ERR_IO;
    }

    if (fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return PSX_STATE_ERR_IO;
    }

    data = malloc((size_t)size);

    if (!data) {
        fclose(file);
        return PSX_STATE_ERR_IO;
    }

    read = fread(data, 1, (size_t)size, file);

    fclose(file);

    if (read != (size_t)size) {
        free(data);
        return PSX_STATE_ERR_IO;
    }

    result = psx_load_state_from_memory_ex(psx, data, (size_t)size, flags);

    free(data);

    if (result == PSX_STATE_OK) {
        /* A user-initiated load jumps the timeline, so every rewind snapshot and
           the runahead slot describe a machine that no longer exists. NOT done in
           psx_load_state_from_memory(): that is the function rewind itself
           restores through, and resetting there would empty the ring on the first
           step back. */
        psx_rewind_reset();

        log_info("Save state loaded from %s", path);
    }
    /* The card verdicts are a question for the user, not a failure: the state
       is intact and nothing was touched. Logging them at error level would put
       a red line in the diag file every time the front-end asks. */
    else if (result == PSX_STATE_ERR_CARD_NEWER || result == PSX_STATE_ERR_CARD_DIVERGED)
        log_info("Save state '%s' not loaded yet: %s", path, psx_state_strerror(result));
    else
        log_error("Failed to load save state '%s': %s", path, psx_state_strerror(result));

    return result;
}

/* -------------------------------------------------------------------------- */
/* Slot paths                                                                 */
/* -------------------------------------------------------------------------- */

static void state_sanitise_stem(const char* path, char* out, size_t out_size) {
    const char* base;
    const char* slash;
    const char* dot;
    size_t len;
    size_t i;

    if (!out_size)
        return;

    out[0] = '\0';

    if (!path || !*path) {
        snprintf(out, out_size, "nodisc");
        return;
    }

    base = path;
    slash = strrchr(path, '/');

    if (slash)
        base = slash + 1;

#if defined(_WIN32)
    slash = strrchr(base, '\\');

    if (slash)
        base = slash + 1;
#endif

    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);

    if (len >= out_size)
        len = out_size - 1;

    for (i = 0; i < len; i++) {
        char c = base[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out[i] = c;
        else
            out[i] = '_';
    }

    out[len] = '\0';

    if (!len)
        snprintf(out, out_size, "nodisc");
}

int psx_state_slot_path(psx_t* psx, int slot, const char* base_dir, char* out, size_t out_size) {
    char stem[128];
    uint64_t fingerprint;
    size_t len;

    if (!psx || !out || !out_size || slot < 0)
        return PSX_STATE_ERR_ARG;

    if (!base_dir || !*base_dir)
        return PSX_STATE_ERR_ARG;

    state_sanitise_stem(psx_cdrom_get_disc_path(psx->cdrom), stem, sizeof(stem));

    fingerprint = psx_cdrom_get_disc_fingerprint(psx->cdrom);

    len = strlen(base_dir);

    if (snprintf(out, out_size, "%s%ssavestates/%s-%08x.slot%d.pss",
            base_dir,
            (len && (base_dir[len - 1] == '/' || base_dir[len - 1] == '\\')) ? "" : "/",
            stem,
            (unsigned)(fingerprint & 0xffffffffu),
            slot) < 0)
        return PSX_STATE_ERR_ARG;

    return PSX_STATE_OK;
}

/* Best-effort mkdir of <base_dir>/savestates. */
static void state_ensure_slot_dir(const char* path) {
    char dir[1024];
    char* slash;

    if (!path)
        return;

    snprintf(dir, sizeof(dir), "%s", path);

    slash = strrchr(dir, '/');

#if defined(_WIN32)
    {
        char* back = strrchr(dir, '\\');

        if (!slash || (back && back > slash))
            slash = back;
    }
#endif

    if (!slash || slash == dir)
        return;

    *slash = '\0';

    state_mkdir(dir);
}

/* -------------------------------------------------------------------------- */
/* Cross-thread request queue                                                 */
/* -------------------------------------------------------------------------- */

/*
    The Android host calls saveStateToSlot()/loadStateFromSlot() from a worker
    thread while the core runs on the emulation thread. Nothing here touches
    psx_t from the caller's thread: the request is parked in the word below and
    executed by psx_state_service_requests(), which psx_update() calls at the
    top of every emulated step (i.e. on an instruction boundary, on the
    emulation thread).

    g_state_request is a small state machine:
        0                    idle
        PSX_STATE_OP_SAVE    a save is parked
        PSX_STATE_OP_LOAD    a load is parked
        -1                   the emulation thread has finished; result is valid

    A single producer at a time is enforced by the 0 -> op compare-and-swap, so
    a second concurrent request gets PSX_STATE_ERR_BUSY rather than clobbering
    the first one's parameters.
*/

static psx_t* g_state_machine = NULL;
static PSX_STATE_ATOMIC_INT g_state_request = 0;
static PSX_STATE_ATOMIC_INT g_state_result = 0;
static int g_state_slot = 0;
static char g_state_path[1024];
/* Plain int, like g_state_slot and g_state_path: written by the producer BEFORE
   the release-store that parks the request, and read by the emulation thread
   after its acquire-load of it, so the CAS that admits one producer at a time
   is what publishes it. */
static unsigned g_state_flags = 0;

void psx_state_set_machine(psx_t* psx) {
    g_state_machine = psx;
}

/* Little-endian scalar reads straight out of a byte buffer. The loader reads the
   whole state into memory and uses psx_sr_*; the preview reader deliberately does
   not — a picker refreshing ten tiles must not pull ten multi-megabyte states into
   RAM to fetch a few KB of PNG each, so it streams and needs these. */
static uint32_t psx_rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t psx_rd_u64(const uint8_t* p) {
    return (uint64_t)psx_rd_u32(p) | ((uint64_t)psx_rd_u32(p + 4) << 32);
}

int psx_state_read_thumbnail(const char* path, void** out_data, size_t* out_size) {
    uint8_t header[PSX_STATE_HEADER_SIZE];
    uint8_t tlv[PSX_STATE_SECTION_HEADER_SIZE];
    uint8_t meta[16];
    FILE* file;
    uint32_t sections;
    uint32_t i;

    if (out_data)
        *out_data = NULL;

    if (out_size)
        *out_size = 0;

    if (!path || !*path || !out_data || !out_size)
        return PSX_STATE_ERR_ARG;

    file = fopen(path, "rb");

    if (!file)
        return PSX_STATE_ERR_IO;

    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return PSX_STATE_ERR_TRUNCATED;
    }

    if (psx_rd_u32(header) != PSX_STATE_MAGIC0 || psx_rd_u32(header + 4) != PSX_STATE_MAGIC1) {
        fclose(file);
        return PSX_STATE_ERR_MAGIC;
    }

    if (psx_rd_u32(header + 8) != PSX_STATE_FORMAT_VERSION) {
        fclose(file);
        return PSX_STATE_ERR_VERSION;
    }

    /* core_abi deliberately NOT checked — see the contract in state.h. A future
       ABI bump changes device payloads, not the container framing this walks,
       so refusing here would blind the picker for no reason. */

    sections = psx_rd_u32(header + 16);

    /* Sections may be preceded by a larger future header. */
    if (fseek(file, (long)psx_rd_u32(header + 20), SEEK_SET) != 0) {
        fclose(file);
        return PSX_STATE_ERR_TRUNCATED;
    }

    for (i = 0; i < sections; i++) {
        uint32_t id;
        uint64_t length;

        if (fread(tlv, 1, sizeof(tlv), file) != sizeof(tlv)) {
            fclose(file);
            return PSX_STATE_ERR_TRUNCATED;
        }

        id = psx_rd_u32(tlv);
        length = psx_rd_u64(tlv + 8);

        if (id != PSX_SS_THUMB) {
            if (fseek(file, (long)length, SEEK_CUR) != 0) {
                fclose(file);
                return PSX_STATE_ERR_TRUNCATED;
            }

            continue;
        }

        if (length < sizeof(meta) || fread(meta, 1, sizeof(meta), file) != sizeof(meta)) {
            fclose(file);
            return PSX_STATE_ERR_TRUNCATED;
        }

        /* An unknown encoding means "no preview", not "guess" — the format tag
           exists so the encoding can change without every reader in lockstep. */
        if (psx_rd_u32(meta) != PSX_STATE_THUMB_PNG) {
            fclose(file);
            return PSX_STATE_ERR_MISSING;
        }

        {
            const uint32_t image_size = psx_rd_u32(meta + 12);
            void* image;

            if (!image_size || image_size > PSX_STATE_THUMB_MAX_BYTES ||
                (uint64_t)image_size > (length - sizeof(meta))) {
                fclose(file);
                return PSX_STATE_ERR_TRUNCATED;
            }

            image = malloc(image_size);

            if (!image) {
                fclose(file);
                return PSX_STATE_ERR_IO;
            }

            if (fread(image, 1, image_size, file) != image_size) {
                free(image);
                fclose(file);
                return PSX_STATE_ERR_TRUNCATED;
            }

            fclose(file);

            *out_data = image;
            *out_size = image_size;

            return PSX_STATE_OK;
        }
    }

    fclose(file);

    /* No THUMB section: a state written before previews existed. Not an error. */
    return PSX_STATE_ERR_MISSING;
}

int psx_state_slot_thumbnail(int slot, const char* base_dir, void** out_data, size_t* out_size) {
    char path[1024];

    if (out_data)
        *out_data = NULL;

    if (out_size)
        *out_size = 0;

    if (!g_state_machine)
        return PSX_STATE_ERR_NO_MACHINE;

    if (psx_state_slot_path(g_state_machine, slot, base_dir, path, sizeof(path)) != PSX_STATE_OK)
        return PSX_STATE_ERR_ARG;

    return psx_state_read_thumbnail(path, out_data, out_size);
}

int psx_state_slot_info(int slot, const char* base_dir, char* out_disc_path, size_t out_size) {
    char path[1024];
    const char* disc;
    FILE* file;

    if (!g_state_machine || !out_disc_path || !out_size)
        return 0;

    *out_disc_path = '\0';

    if (psx_state_slot_path(g_state_machine, slot, base_dir, path, sizeof(path)) != PSX_STATE_OK)
        return 0;

    /* Existence only. Opening is enough and avoids a stat() portability split;
       the picker just needs to know whether the tile is loadable. */
    file = fopen(path, "rb");

    if (!file)
        return 0;

    fclose(file);

    disc = psx_cdrom_get_disc_path(g_state_machine->cdrom);

    if (!disc || !*disc)
        return 0;

    snprintf(out_disc_path, out_size, "%s", disc);

    return 1;
}

void psx_state_service_requests(void) {
    int op = PSX_STATE_LOAD(g_state_request);
    int result;

    if (op != PSX_STATE_OP_SAVE && op != PSX_STATE_OP_LOAD)
        return;

    if (!g_state_machine) {
        PSX_STATE_STORE(g_state_result, PSX_STATE_ERR_NO_MACHINE);
        PSX_STATE_STORE(g_state_request, -1);
        return;
    }

    if (op == PSX_STATE_OP_SAVE) {
        state_ensure_slot_dir(g_state_path);

        result = psx_save_state(g_state_machine, g_state_path);
    } else {
        result = psx_load_state_ex(g_state_machine, g_state_path, g_state_flags);
    }

    (void)g_state_slot;

    PSX_STATE_STORE(g_state_result, result);
    PSX_STATE_STORE(g_state_request, -1);
}

int psx_state_request_slot(int op, int slot, const char* base_dir, int timeout_ms) {
    return psx_state_request_slot_ex(op, slot, base_dir, timeout_ms, 0);
}

int psx_state_request_slot_ex(int op, int slot, const char* base_dir, int timeout_ms,
                              unsigned flags) {
    int expected = 0;
    int waited = 0;
    int result;

    if (op != PSX_STATE_OP_SAVE && op != PSX_STATE_OP_LOAD)
        return PSX_STATE_ERR_ARG;

    if (!g_state_machine)
        return PSX_STATE_ERR_NO_MACHINE;

    if (PSX_STATE_LOAD(g_state_request) != 0)
        return PSX_STATE_ERR_BUSY;

    result = psx_state_slot_path(g_state_machine, slot, base_dir, g_state_path, sizeof(g_state_path));

    if (result != PSX_STATE_OK)
        return result;

    g_state_slot = slot;
    g_state_flags = flags;

    PSX_STATE_STORE(g_state_result, PSX_STATE_ERR_TIMEOUT);

    if (!PSX_STATE_CAS(g_state_request, expected, op))
        return PSX_STATE_ERR_BUSY;

    if (timeout_ms < 0)
        timeout_ms = 0;

    while (waited < timeout_ms) {
        if (PSX_STATE_LOAD(g_state_request) == -1) {
            result = PSX_STATE_LOAD(g_state_result);
            PSX_STATE_STORE(g_state_request, 0);

            return result;
        }

        PSX_STATE_SLEEP_MS(2);
        waited += 2;
    }

    /* The emulation thread never got to it (the front-end almost certainly has
       the VM paused). Withdraw the request so the machine does not silently
       save/load minutes later when the user resumes. */
    expected = op;

    if (PSX_STATE_CAS(g_state_request, expected, 0))
        return PSX_STATE_ERR_TIMEOUT;

    /* It completed while we were withdrawing. */
    result = PSX_STATE_LOAD(g_state_result);
    PSX_STATE_STORE(g_state_request, 0);

    return result;
}
