/*
    Host gate for the save-state <-> memory-card divergence check
    (PSX_SS_MCARD in psx/state.c, the fingerprint accessors in psx/dev/mcd.c).

    What this is defending.

    A save state and a memory card are two timelines the player can move
    independently. Save a state, keep playing, save to the card in-game, then
    load the old state: the console is now behind its own card. Real PS1 games
    notice — some cope, some refuse to load their own save. Worse here, the core
    restores the card image with the machine and flushes it at shutdown, so the
    in-game save made after the state is quietly thrown away. The state records
    what the cards looked like so the load can ask first.

    Two properties are worth more than the rest of the file put together, and
    both are about NOT crying wolf:

      * A state written before this feature existed has no fingerprint, must
        still load, and must NOT warn. The user has states on disk right now;
        breaking or nagging about them is the regression this gate exists for.
        The fixture is not a hand-built approximation — it is a real state with
        the section physically removed and the header rewritten, which is byte
        for byte what the previous core wrote.

      * Loading the same state twice must warn at most once. The first load
        rewinds the card to match, so the second has nothing to complain about.
        A warning that fires when nothing is wrong gets dismissed unread.

    The machine is real (psx_init against a blank BIOS) and the card writes go
    through the actual MCD_W_STATE_* protocol the game's card driver drives, not
    a memcpy into the image behind the state machine's back — the write
    generation only counts if the thing that increments it is on the real path.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/psx.h"
#include "psx/state.h"
#include "psx/dev/pad.h"
#include "psx/dev/mcd.h"

static int g_failures = 0;

static void check(int condition, const char* case_name, const char* what) {
    if (!condition) {
        printf("MCARD_DIVERGE failed case=%s check=%s\n", case_name, what);
        g_failures++;
    }
}

static void check_code(int got, int want, const char* case_name, const char* what) {
    if (got != want) {
        printf("MCARD_DIVERGE failed case=%s check=%s got=%d (%s) want=%d (%s)\n",
               case_name, what, got, psx_state_strerror(got), want, psx_state_strerror(want));
        g_failures++;
    }
}

/* ------------------------------------------------------------------------ */
/* Fixtures                                                                  */
/* ------------------------------------------------------------------------ */

static int write_blank_bios(const char* path) {
    FILE* file = fopen(path, "wb");
    uint8_t block[4096] = {0};
    size_t offset;

    if (!file)
        return 0;

    for (offset = 0; offset < 512u * 1024u; offset += sizeof(block)) {
        if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
            fclose(file);
            return 0;
        }
    }

    return fclose(file) == 0;
}

static int write_blank_card(const char* path) {
    FILE* file = fopen(path, "wb");
    uint8_t block[4096] = {0};
    size_t offset;

    if (!file)
        return 0;

    for (offset = 0; offset < 0x20000u; offset += sizeof(block)) {
        if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
            fclose(file);
            return 0;
        }
    }

    return fclose(file) == 0;
}

/*
    Drive one 128-byte sector write through the card's real serial state
    machine, the way pad.c does: psx_mcd_write() delivers the byte the console
    is shifting out, psx_mcd_read() clocks the card's reply and advances the
    state. Returns 1 if the card ended up back at rest having taken the data.
*/
static int mcd_open_command(psx_mcd_t* mcd, char command) {
    psx_mcd_reset(mcd);

    /* The console addresses the card (0x81) and clocks out its first reply.
       MCD_STATE_TX_HIZ -> MCD_STATE_TX_FLG. */
    psx_mcd_read(mcd);

    /* The command byte is latched by psx_mcd_write() while the card sits at
       TX_FLG — that case is the ONLY place mcd->mode is assigned, so sending it
       one step early leaves the card with no mode and the ID handshake dead-ends
       back at TX_HIZ. */
    psx_mcd_write(mcd, (uint8_t)command);
    psx_mcd_read(mcd);

    /* Card ID handshake: 0x5a then 0x5d. TX_ID2 routes on mode to
       MCD_R_STATE_RX_MSB or MCD_W_STATE_RX_MSB. */
    psx_mcd_read(mcd);
    psx_mcd_read(mcd);

    /* Sector address, MSB then LSB. The card shifts it left 7 into a byte
       offset, which is what makes a sector 128 bytes. */
    return 1;
}

static void mcd_send_address(psx_mcd_t* mcd, uint16_t sector) {
    psx_mcd_write(mcd, (uint8_t)(sector >> 8));
    psx_mcd_read(mcd);
    psx_mcd_write(mcd, (uint8_t)(sector & 0xff));
    psx_mcd_read(mcd);
}

static int mcd_write_sector(psx_mcd_t* mcd, uint16_t sector, uint8_t fill) {
    int i;

    mcd_open_command(mcd, 'W');
    mcd_send_address(mcd, sector);

    if (mcd->state != MCD_W_STATE_RX_DATA)
        return 0;

    for (i = 0; i < 128; i++) {
        psx_mcd_write(mcd, (uint8_t)(fill + i));
        psx_mcd_read(mcd);
    }

    return 1;
}

/* The read counterpart, byte for byte: ACK1, ACK2, MSB, LSB, 128 data bytes,
   checksum, end marker. Used to prove a read leaves the fingerprint alone. */
static int mcd_read_sector(psx_mcd_t* mcd, uint16_t sector) {
    int i;

    mcd_open_command(mcd, 'R');
    mcd_send_address(mcd, sector);

    if (mcd->state != MCD_R_STATE_TX_ACK1)
        return 0;

    for (i = 0; i < 4; i++)
        psx_mcd_read(mcd);

    if (mcd->state != MCD_R_STATE_TX_DATA)
        return 0;

    for (i = 0; i < 128; i++)
        psx_mcd_read(mcd);

    psx_mcd_read(mcd); /* checksum */
    psx_mcd_read(mcd); /* 'G' end marker; returns the card to TX_HIZ */

    return mcd->state == MCD_STATE_TX_HIZ;
}

static long file_size(const char* path) {
    FILE* file = fopen(path, "rb");
    long size;

    if (!file)
        return -1;

    if (fseek(file, 0, SEEK_END)) {
        fclose(file);
        return -1;
    }

    size = ftell(file);
    fclose(file);

    return size;
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t* p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/*
    Turn a state into the state the PREVIOUS core would have written: walk the
    TLV chain, drop PSX_SS_MCARD entirely, and decrement section_count in the
    header so the reader's declared-vs-found check still balances.

    Deliberately a real strip rather than "save with the section suppressed". A
    flag that skips the write could drift away from what shipped; removing the
    bytes cannot. Returns 1 on success, 0 if the section was not found (which
    would itself be a bug worth failing on).
*/
static int strip_mcard_section(const char* in_path, const char* out_path) {
    uint8_t* data;
    long size;
    size_t offset;
    uint32_t declared;
    uint32_t header_size;
    uint8_t* out;
    size_t out_size = 0;
    uint32_t kept = 0;
    int removed = 0;
    FILE* file;

    size = file_size(in_path);

    if (size <= (long)PSX_STATE_HEADER_SIZE)
        return 0;

    data = (uint8_t*)malloc((size_t)size);
    out = (uint8_t*)malloc((size_t)size);

    if (!data || !out) {
        free(data);
        free(out);
        return 0;
    }

    file = fopen(in_path, "rb");

    if (!file || fread(data, 1, (size_t)size, file) != (size_t)size) {
        if (file)
            fclose(file);
        free(data);
        free(out);
        return 0;
    }

    fclose(file);

    declared = rd_u32(data + 16);
    header_size = rd_u32(data + 20);

    if (header_size > (uint32_t)size) {
        free(data);
        free(out);
        return 0;
    }

    memcpy(out, data, header_size);
    out_size = header_size;
    offset = header_size;

    while (offset + PSX_STATE_SECTION_HEADER_SIZE <= (size_t)size) {
        uint32_t id = rd_u32(data + offset);
        uint64_t length = rd_u64(data + offset + 8);
        size_t total = PSX_STATE_SECTION_HEADER_SIZE + (size_t)length;

        if (offset + total > (size_t)size)
            break;

        if (id == PSX_SS_MCARD) {
            removed = 1;
        } else {
            memcpy(out + out_size, data + offset, total);
            out_size += total;
            kept++;
        }

        offset += total;
    }

    if (removed && kept + 1 == declared)
        wr_u32(out + 16, kept);

    file = fopen(out_path, "wb");

    if (file) {
        if (fwrite(out, 1, out_size, file) != out_size)
            removed = 0;

        fclose(file);
    } else {
        removed = 0;
    }

    free(data);
    free(out);

    return removed;
}

static int state_has_section(const char* path, uint32_t want_id) {
    uint8_t* data;
    long size = file_size(path);
    size_t offset;
    int found = 0;
    FILE* file;

    if (size <= (long)PSX_STATE_HEADER_SIZE)
        return 0;

    data = (uint8_t*)malloc((size_t)size);

    if (!data)
        return 0;

    file = fopen(path, "rb");

    if (!file || fread(data, 1, (size_t)size, file) != (size_t)size) {
        if (file)
            fclose(file);
        free(data);
        return 0;
    }

    fclose(file);
    offset = rd_u32(data + 20);

    while (offset + PSX_STATE_SECTION_HEADER_SIZE <= (size_t)size) {
        uint32_t id = rd_u32(data + offset);
        uint64_t length = rd_u64(data + offset + 8);

        if (id == want_id)
            found = 1;

        offset += PSX_STATE_SECTION_HEADER_SIZE + (size_t)length;
    }

    free(data);

    return found;
}

/* ------------------------------------------------------------------------ */

static const char* kBios = "build/tests/mcard_bios.bin";
static const char* kSlot1 = "build/tests/mcard_slot1.mcd";
static const char* kSlot2 = "build/tests/mcard_slot2.mcd";
static const char* kState = "build/tests/mcard_state.pss";
static const char* kLegacyState = "build/tests/mcard_state_legacy.pss";

static psx_t* make_machine(const char* case_name) {
    psx_t* psx = psx_create();

    if (!psx || psx_init(psx, kBios, NULL) != 0) {
        printf("MCARD_DIVERGE failed case=%s check=init\n", case_name);
        g_failures++;
        return NULL;
    }

    if (psx_pad_attach_mcd(psx->pad, 0, kSlot1) != 0 ||
        psx_pad_attach_mcd(psx->pad, 1, kSlot2) != 0) {
        printf("MCARD_DIVERGE failed case=%s check=attach-cards\n", case_name);
        g_failures++;
        psx_destroy(psx);
        return NULL;
    }

    return psx;
}

/* ------------------------------------------------------------------------ */
/* Cases                                                                     */
/* ------------------------------------------------------------------------ */

/* The card is untouched between save and load: no question to ask. */
static void case_untouched_card_is_silent(void) {
    const char* name = "untouched-card-loads-silently";
    psx_t* psx = make_machine(name);

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");
    check(state_has_section(kState, PSX_SS_MCARD), name, "section-written");
    check_code(psx_load_state(psx, kState), PSX_STATE_OK, name, "load");

    psx_destroy(psx);
}

/* The case the whole feature exists for: the game saved to the card AFTER the
   state was taken, so loading it puts the console behind its own card. */
static void case_card_written_after_state_is_newer(void) {
    const char* name = "card-written-after-state-is-newer";
    psx_t* psx = make_machine(name);
    uint32_t gen_before;
    uint32_t gen_after;

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");

    gen_before = psx_mcd_write_generation(psx->pad->mcd_slot[0]);
    check(mcd_write_sector(psx->pad->mcd_slot[0], 4, 0xa5), name, "sector-write-drove-protocol");
    gen_after = psx_mcd_write_generation(psx->pad->mcd_slot[0]);

    check(gen_after == gen_before + 1, name, "generation-advanced");

    check_code(psx_load_state(psx, kState), PSX_STATE_ERR_CARD_NEWER, name, "load-warns");

    psx_destroy(psx);
}

/* "Load anyway" has to actually load. And the refusal must not have left the
   machine half-applied, which is why the state is loadable at all afterwards. */
static void case_override_loads_anyway(void) {
    const char* name = "override-loads-anyway";
    psx_t* psx = make_machine(name);

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");
    check(mcd_write_sector(psx->pad->mcd_slot[0], 6, 0x3c), name, "sector-write");

    check_code(psx_load_state(psx, kState), PSX_STATE_ERR_CARD_NEWER, name, "load-warns-first");
    check_code(psx_load_state_ex(psx, kState, PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE),
               PSX_STATE_OK, name, "override-loads");

    psx_destroy(psx);
}

/* Warn once, not every time. The load rewound the card to what the state
   expects, so a second load has genuinely nothing to report. */
static void case_second_load_does_not_nag(void) {
    const char* name = "second-load-does-not-nag";
    psx_t* psx = make_machine(name);

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");
    check(mcd_write_sector(psx->pad->mcd_slot[0], 8, 0x11), name, "sector-write");

    check_code(psx_load_state(psx, kState), PSX_STATE_ERR_CARD_NEWER, name, "first-load-warns");
    check_code(psx_load_state_ex(psx, kState, PSX_STATE_LOAD_IGNORE_CARD_DIVERGENCE),
               PSX_STATE_OK, name, "user-said-load-anyway");

    /* The card image now equals what the state carries. Silence. */
    check_code(psx_load_state(psx, kState), PSX_STATE_OK, name, "second-load-silent");

    psx_destroy(psx);
}

/*
    A card whose contents differ with no evidence of direction — here a fresh
    attach, so the session nonce does not match and the generations are not
    comparable. Softer verdict, still a warning.

    This is also the case that pins the mtime decision. The card file here is
    rewritten (blank) AFTER the state was taken, so its mtime is newer; an
    implementation that promoted on "card file is newer" would call this
    CARD_NEWER and tell the user the game had saved since — about a card that
    was in fact erased in the card manager and that the game never touched.

    Worth noting how this was caught: an earlier version DID promote on mtime,
    and this case passed only because the whole test runs inside one wall-clock
    second (st_mtime has second resolution, so the two stamps compared equal). A
    verdict that changes depending on whether the test straddles a second
    boundary is not a verdict. The heuristic came out; the case stays, and now
    asserts the outcome for a reason that has nothing to do with the clock.
*/
static void case_unrelated_card_is_direction_unknown(void) {
    const char* name = "unrelated-card-is-direction-unknown";
    psx_t* psx = make_machine(name);
    psx_t* second;

    if (!psx)
        return;

    check(mcd_write_sector(psx->pad->mcd_slot[0], 12, 0x77), name, "sector-write");
    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");

    /* Flush and tear down, then come back as a separate run of the app against
       a blank card. Different attach => different session nonce. */
    psx_destroy(psx);

    check(write_blank_card(kSlot1), name, "blank-card");

    second = make_machine(name);

    if (!second)
        return;

    check_code(psx_load_state(second, kState), PSX_STATE_ERR_CARD_DIVERGED, name,
               "load-warns-softly");

    psx_destroy(second);
}

/*
    The same shape, but with the card file's mtime pushed FAR into the future,
    well past any second-boundary accident. Still the soft verdict.

    Without this, the case above would silently start passing for the wrong
    reason again the moment someone reintroduced an mtime rule and happened to
    run the suite quickly.
*/
static void case_future_card_mtime_does_not_promote(void) {
    const char* name = "future-card-mtime-does-not-promote";
    psx_t* psx = make_machine(name);
    psx_t* second;

    if (!psx)
        return;

    check(mcd_write_sector(psx->pad->mcd_slot[0], 14, 0x9d), name, "sector-write");
    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");

    psx_destroy(psx);

    check(write_blank_card(kSlot1), name, "blank-card");

    /* +1 day. touch(1) is portable enough for a host gate and keeps this free
       of a utimes() portability split. */
    if (system("touch -t 210001010000 build/tests/mcard_slot1.mcd") != 0) {
        printf("MCARD_DIVERGE failed case=%s check=touch\n", name);
        g_failures++;
        return;
    }

    second = make_machine(name);

    if (!second)
        return;

    check(psx_mcd_file_mtime(second->pad->mcd_slot[0]) > 0, name, "mtime-readable");
    check_code(psx_load_state(second, kState), PSX_STATE_ERR_CARD_DIVERGED, name,
               "still-soft-despite-newer-file");

    psx_destroy(second);
}

/*
    THE backward-compatibility gate.

    A state with no PSX_SS_MCARD section is what every state already on a user's
    device looks like. It must load, and it must not warn — even though the card
    plainly disagrees with it, because "no fingerprint" is not evidence of
    anything and a warning nobody can act on is just noise.
*/
static void case_fingerprintless_state_loads_without_warning(void) {
    const char* name = "fingerprintless-state-loads-without-warning";
    psx_t* psx = make_machine(name);

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");
    check(strip_mcard_section(kState, kLegacyState), name, "stripped-fixture-built");
    check(!state_has_section(kLegacyState, PSX_SS_MCARD), name, "fixture-has-no-fingerprint");

    /* Make the card diverge as loudly as possible: with a fingerprint this
       would be PSX_STATE_ERR_CARD_NEWER. Without one it must be silent. */
    check(mcd_write_sector(psx->pad->mcd_slot[0], 16, 0x5e), name, "sector-write");
    check(mcd_write_sector(psx->pad->mcd_slot[1], 17, 0x6f), name, "sector-write-slot2");

    check_code(psx_load_state(psx, kLegacyState), PSX_STATE_OK, name, "legacy-state-still-loads");

    psx_destroy(psx);
}

/* The container contract that makes the above possible: the new section is
   optional, so a state carrying it must also load on a reader that has no idea
   what it is. Same shape as the THUMB precedent — an unknown id at version 1 is
   skipped, and the mandatory list is unchanged. */
static void case_new_section_is_not_mandatory(void) {
    const char* name = "new-section-is-optional";
    psx_t* psx = make_machine(name);
    long with;
    long without;

    if (!psx)
        return;

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");
    check(strip_mcard_section(kState, kLegacyState), name, "strip");

    with = file_size(kState);
    without = file_size(kLegacyState);

    /* Removing it must actually have removed bytes, or the fixture is a no-op
       and every assertion resting on it is vacuous. */
    check(with > without, name, "section-occupies-bytes");

    /* Both directions load: with the section (this reader understands it) and
       without (a state from before it existed). */
    check_code(psx_load_state(psx, kState), PSX_STATE_OK, name, "load-with-section");
    check_code(psx_load_state(psx, kLegacyState), PSX_STATE_OK, name, "load-without-section");

    psx_destroy(psx);
}

/* A card the game never wrote to must not drift. Guards against the cached
   hash being invalidated (or the generation bumped) by a card READ, which would
   make the warning fire on every load of every state. */
static void case_reads_do_not_count_as_writes(void) {
    const char* name = "reads-do-not-count-as-writes";
    psx_t* psx = make_machine(name);
    psx_mcd_t* mcd;
    uint64_t hash_before;
    uint32_t gen_before;
    int i;

    if (!psx)
        return;

    mcd = psx->pad->mcd_slot[0];

    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");

    hash_before = psx_mcd_content_hash(mcd);
    gen_before = psx_mcd_write_generation(mcd);

    for (i = 0; i < 3; i++)
        check(mcd_read_sector(mcd, (uint16_t)(2 + i)), name, "read-drove-protocol");

    check(psx_mcd_content_hash(mcd) == hash_before, name, "hash-unchanged-by-reads");
    check(psx_mcd_write_generation(mcd) == gen_before, name, "generation-unchanged-by-reads");
    check_code(psx_load_state(psx, kState), PSX_STATE_OK, name, "load-still-silent");

    psx_destroy(psx);
}

/* Divergence is advisory: it must be raised before the machine is touched, so
   cancelling leaves the session exactly as it was. Checked by proving RAM the
   state would have overwritten is still what the caller put there. */
static void case_refusal_does_not_mutate_the_machine(void) {
    const char* name = "refusal-does-not-mutate-machine";
    psx_t* psx = make_machine(name);
    uint32_t sentinel = 0xC0FFEE01u;

    if (!psx)
        return;

    psx_ram_write32(psx->ram, 0x0A0000, 0x11111111u);
    check_code(psx_save_state(psx, kState), PSX_STATE_OK, name, "save");

    /* Move RAM away from what the state holds, then make the card diverge. */
    psx_ram_write32(psx->ram, 0x0A0000, sentinel);
    check(mcd_write_sector(psx->pad->mcd_slot[0], 20, 0x2b), name, "sector-write");

    check_code(psx_load_state(psx, kState), PSX_STATE_ERR_CARD_NEWER, name, "load-refused");

    /* If the refusal had happened mid-apply this would be 0x11111111. */
    check(psx_ram_read32(psx->ram, 0x0A0000) == sentinel, name, "ram-untouched-by-refusal");

    psx_destroy(psx);
}

int main(void) {
#ifdef _WIN32
    system("mkdir build\\tests 2>nul");
#else
    if (system("mkdir -p build/tests") != 0) {
        printf("MCARD_DIVERGE failed case=setup check=mkdir\n");
        return 1;
    }
#endif

    if (!write_blank_bios(kBios) || !write_blank_card(kSlot1) || !write_blank_card(kSlot2)) {
        printf("MCARD_DIVERGE failed case=setup check=fixtures\n");
        return 1;
    }

    case_untouched_card_is_silent();
    case_card_written_after_state_is_newer();
    case_override_loads_anyway();
    case_second_load_does_not_nag();
    case_unrelated_card_is_direction_unknown();
    case_future_card_mtime_does_not_promote();
    case_fingerprintless_state_loads_without_warning();
    case_new_section_is_not_mandatory();
    case_reads_do_not_count_as_writes();
    case_refusal_does_not_mutate_the_machine();

    remove(kBios);
    remove(kSlot1);
    remove(kSlot2);
    remove(kState);
    remove(kLegacyState);

    if (g_failures) {
        printf("MCARD_DIVERGE %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("MCARD_DIVERGE all cases passed\n");

    return 0;
}
