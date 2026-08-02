#include "mcd.h"
#include "../log.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#endif

static void psx_mcd_ensure_parent(const char* path) {
    if (!path)
        return;

#ifdef PSVITA_TARGET
    const size_t path_len = strlen(path);
    if (path_len == 0)
        return;

    char* tmp = malloc(path_len + 1);
    if (!tmp)
        return;

    memcpy(tmp, path, path_len + 1);
#else
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
#endif

    char* slash = strrchr(tmp, '/');
#ifdef _WIN32
    char* bslash = strrchr(tmp, '\\');
    if (!slash || (bslash && bslash > slash))
        slash = bslash;
#endif
    if (!slash || slash == tmp) {
#ifdef PSVITA_TARGET
        free(tmp);
#endif
        return;
    }

    *slash = '\0';

    struct stat st;
    if (stat(tmp, &st) == 0) {
#ifdef PSVITA_TARGET
        free(tmp);
#endif
        return;
    }

#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif

#ifdef PSVITA_TARGET
    free(tmp);
#endif
}

psx_mcd_t* psx_mcd_create(void) {
    return (psx_mcd_t*)malloc(sizeof(psx_mcd_t));
}

/*
    Session nonce for the divergence check.

    write_generation only means anything relative to another sample of the SAME
    card instance: it starts at 0 on every attach, so comparing a generation
    across two runs of the app would happily "prove" that a fresh card is older
    than a state taken last week. Stamping each attach with a nonce lets the
    comparison say "these two numbers are not comparable" instead of guessing —
    which is the difference between the confident warning and the soft one.

    Mixes a clock reading (distinct across processes) with a counter (distinct
    within one process, where two cards attach in the same second).
*/
static uint64_t g_mcd_session_seq = 0;

static uint64_t psx_mcd_new_session_id(void) {
    uint64_t now = (uint64_t)time(NULL);
    uint64_t seq = ++g_mcd_session_seq;

    return psx_state_fnv1a(&now, sizeof(now), PSX_STATE_FNV_SEED) ^
           (seq * 0x9e3779b97f4a7c15ull);
}

int psx_mcd_init(psx_mcd_t* mcd, const char* path) {
    memset(mcd, 0, sizeof(psx_mcd_t));

    mcd->state = MCD_STATE_TX_HIZ;
    mcd->flag = 0x08;

    /* Fresh instance: generation 0, no cached hash, new session. */
    mcd->session_id = psx_mcd_new_session_id();
    mcd->write_generation = 0;
    mcd->hash_valid = 0;

    /*
        OWN the path. This used to store the caller's pointer.

        The frontend built its slot paths as local std::strings and passed .c_str(); the moment
        that scope ended the pointer dangled, and the next flush did fopen(mcd->path, "wb") on
        whatever had since been written to that memory. What had been written to it was the disc
        path — so saving a memory card OVERWROTE THE USER'S .cue FILE with 128 KB of card image,
        destroying the game. Confirmed on device: a 97-byte cue replaced by exactly
        MCD_MEMORY_SIZE bytes.

        A borrowed string is never safe here: this pointer outlives every caller, is used on a
        different thread's schedule, and is opened for WRITING.
    */
    mcd->path = NULL;

    if (path) {
        size_t len = strlen(path) + 1;
        char* owned = malloc(len);

        if (owned) {
            memcpy(owned, path, len);
            mcd->path = owned;
        }
    }
    mcd->buf = malloc(MCD_MEMORY_SIZE);
    mcd->tx_data_ready = 0;

    memset(mcd->buf, 0, MCD_MEMORY_SIZE);

    if (!path)
        return 0;

    psx_mcd_ensure_parent(path);

    FILE* file = fopen(path, "rb");

    if (!file) {
        // Create a blank card if missing
        file = fopen(path, "wb");
        if (!file)
            return 1;

        fwrite(mcd->buf, 1, MCD_MEMORY_SIZE, file);
        fclose(file);

        // Re-open for read so subsequent logic flows
        file = fopen(path, "rb");
        if (!file)
            return 1;
    }

    if (!fread(mcd->buf, 1, MCD_MEMORY_SIZE, file))
        return 2;

    fclose(file);

    return 0;
}

uint8_t psx_mcd_read(psx_mcd_t* mcd) {
    switch (mcd->state) {
        case MCD_STATE_TX_HIZ: mcd->tx_data = 0xff; break;
        case MCD_STATE_TX_FLG:
            if ((mcd->rx_data == 0x81) || (mcd->rx_data == 0x01)) {
                mcd->tx_data_ready = 1;
                mcd->tx_data = 0xff;
                mcd->state = MCD_STATE_TX_HIZ;
                break;
            }

            mcd->tx_data = mcd->flag;
            mcd->flag = 0x00;
            break;
        case MCD_STATE_TX_ID1: mcd->tx_data = 0x5a; break;
        case MCD_STATE_TX_ID2: {
            mcd->tx_data_ready = 1;
            mcd->tx_data = 0x5d;

            switch (mcd->mode) {
                case 'R': mcd->state = MCD_R_STATE_RX_MSB; break;
                case 'W': mcd->state = MCD_W_STATE_RX_MSB; break;
                case 'S': mcd->state = MCD_S_STATE_TX_ACK1; break;
                default:
                    mcd->tx_data_ready = 0;
                    mcd->tx_data = 0xff;
                    mcd->state = MCD_STATE_TX_HIZ;
                    return mcd->tx_data;
            }

            // printf("mcd read %02x\n", mcd->tx_data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", mcd->tx_data);
            // log_set_quiet(1);

            return mcd->tx_data;
        } break;

        // Read states
        case MCD_R_STATE_RX_MSB: mcd->tx_data = 0x00; break;
        case MCD_R_STATE_RX_LSB: mcd->tx_data = mcd->msb; break;
        case MCD_R_STATE_TX_ACK1: mcd->tx_data = 0x5c; break;
        case MCD_R_STATE_TX_ACK2: mcd->tx_data = 0x5d; break;
        case MCD_R_STATE_TX_MSB: mcd->tx_data = mcd->msb; mcd->checksum  = mcd->msb; break;
        case MCD_R_STATE_TX_LSB: mcd->tx_data = mcd->lsb; mcd->checksum ^= mcd->lsb;
                                 mcd->pending_bytes = 128; break;
        case MCD_R_STATE_TX_DATA: {
            --mcd->pending_bytes;

            uint8_t data = mcd->buf[mcd->addr++];

            mcd->checksum ^= data;

            if (!mcd->pending_bytes) {
                mcd->tx_data = data;

                break;
            }

            // printf("mcd read %02x\n", data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", data);
            // log_set_quiet(1);

            return data;
        } break;
        case MCD_R_STATE_TX_CHK: mcd->tx_data = mcd->checksum; break;
        case MCD_R_STATE_TX_MEB: {
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", 'G');
            // log_set_quiet(1);

            // printf("mcd read %02x\n", 'G');

            return 'G';
        } break;

        /* Write states */
        case MCD_W_STATE_RX_MSB: mcd->tx_data = 0x00; break;
        case MCD_W_STATE_RX_LSB: mcd->tx_data = mcd->msb;
                                 mcd->pending_bytes = 128; break;
        case MCD_W_STATE_RX_DATA: {
             --mcd->pending_bytes;

            mcd->buf[mcd->addr++] = mcd->rx_data;

            /*
                The card image just changed, so any cached content hash is stale.
                Two stores on a path that already does a bounds-free byte write;
                nothing here reads mcd->path, touches the file, or can alter what
                the game observes on the wire.

                The generation counts COMPLETED 128-byte sectors rather than
                bytes, because a sector is what the card protocol calls a write
                and what a game's save actually consists of.
            */
            mcd->hash_valid = 0;

            if (!mcd->pending_bytes) {
                mcd->write_generation++;
                mcd->tx_data = mcd->rx_data;

                break;
            }

            // printf("mcd read %02x\n", mcd->rx_data);

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", mcd->rx_data);
            // log_set_quiet(1);

            return mcd->rx_data;
        } break;
        case MCD_W_STATE_RX_CHK: mcd->tx_data = mcd->rx_data; break;
        case MCD_W_STATE_RX_CHK2: mcd->tx_data = mcd->rx_data; break;
        case MCD_W_STATE_TX_ACK1: mcd->tx_data = 0x5c; break;
        case MCD_W_STATE_TX_ACK2: mcd->tx_data = 0x5d; break;
        case MCD_W_STATE_TX_MEB: {
            mcd->tx_data_ready = 0;
            mcd->state = MCD_STATE_TX_HIZ;

            // log_set_quiet(0);
            // log_fatal("mcd read %02x", 'G');
            // log_set_quiet(1);

            // printf("mcd read %02x\n", 'G');

            return 'G';
        } break;
    }

    mcd->tx_data_ready = 1;
    mcd->state++;

    // log_set_quiet(0);
    // log_fatal("mcd read %02x", mcd->tx_data);
    // log_set_quiet(1);

    // printf("mcd read %02x\n", mcd->tx_data);

    return mcd->tx_data;
}

void psx_mcd_write(psx_mcd_t* mcd, uint8_t data) {
    mcd->rx_data = data;

    switch (mcd->state) {
        case MCD_STATE_TX_FLG: mcd->mode = data; break;
        case MCD_R_STATE_RX_MSB: mcd->msb = data; break;
        case MCD_R_STATE_RX_LSB: {
            mcd->lsb = data;
            mcd->addr = ((mcd->msb << 8) | mcd->lsb) << 7;
        } break;
        case MCD_W_STATE_RX_MSB: mcd->msb = data; break;
        case MCD_W_STATE_RX_LSB: {
            mcd->lsb = data;
            mcd->addr = ((mcd->msb << 8) | mcd->lsb) << 7;
        } break;
        case MCD_W_STATE_RX_DATA: break;
        case MCD_W_STATE_RX_CHK: /* Don't care */ break;
        case MCD_W_STATE_RX_CHK2: /* Don't care */ break;
    }
}

int psx_mcd_query(psx_mcd_t* mcd) {
    return mcd->tx_data_ready;
}

void psx_mcd_reset(psx_mcd_t* mcd) {
    mcd->state = MCD_STATE_TX_HIZ;
}


/*
    Save state.

    Two halves, and they are deliberately separable:

      * The serial transfer state machine (state/mode/addr/checksum/...). Always
        restored — a state captured mid-card-access must resume mid-access or
        the game's card driver sees a protocol violation.

      * The 128 KiB card image. Restored by default, because game RAM and the
        card have to agree: a state that rewinds the game but not the card can
        leave the in-RAM directory cache describing blocks that no longer look
        like that. The consequence — loading a state rewinds any card writes
        made after it was taken, and psx_mcd_destroy() then persists the
        rewound image — is the standard emulator behaviour, but it IS data loss
        from the user's point of view, so it can be switched off with
        psx_mcd_set_state_restores_image(0). The image is always WRITTEN either
        way, so toggling the setting does not change the file format.

    mcd->path is a borrowed pointer to the host's filename and is never saved.
*/

static int g_psx_mcd_state_restores_image = 1;

void psx_mcd_set_state_restores_image(int enabled) {
    g_psx_mcd_state_restores_image = enabled ? 1 : 0;
}

void psx_mcd_save_state(psx_mcd_t* mcd, psx_state_writer_t* w) {
    psx_sw_u8(w, mcd->flag);
    psx_sw_u16(w, mcd->msb);
    psx_sw_u16(w, mcd->lsb);
    psx_sw_u16(w, mcd->addr);
    psx_sw_u8(w, mcd->rx_data);
    psx_sw_i32(w, mcd->pending_bytes);
    psx_sw_u8(w, (uint8_t)mcd->mode);
    psx_sw_i32(w, mcd->state);
    psx_sw_u8(w, mcd->tx_data);
    psx_sw_i32(w, mcd->tx_data_ready);
    psx_sw_u8(w, mcd->checksum);

    psx_sw_u32(w, MCD_MEMORY_SIZE);
    psx_sw_bytes(w, mcd->buf, MCD_MEMORY_SIZE);
}

int psx_mcd_load_state(psx_mcd_t* mcd, psx_state_reader_t* r) {
    uint32_t image_size;

    mcd->flag = psx_sr_u8(r);
    mcd->msb = psx_sr_u16(r);
    mcd->lsb = psx_sr_u16(r);
    mcd->addr = psx_sr_u16(r);
    mcd->rx_data = psx_sr_u8(r);
    mcd->pending_bytes = psx_sr_i32(r);
    mcd->mode = (char)psx_sr_u8(r);
    mcd->state = psx_sr_i32(r);
    mcd->tx_data = psx_sr_u8(r);
    mcd->tx_data_ready = psx_sr_i32(r);
    mcd->checksum = psx_sr_u8(r);

    image_size = psx_sr_u32(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    if (image_size != MCD_MEMORY_SIZE)
        return PSX_STATE_ERR_GEOMETRY;

    if (mcd->state < MCD_STATE_TX_HIZ || mcd->state > MCD_S_STATE_TX_DAT3)
        return PSX_STATE_ERR_TRUNCATED;

    if (g_psx_mcd_state_restores_image) {
        psx_sr_bytes(r, mcd->buf, MCD_MEMORY_SIZE);

        /* The image was just replaced wholesale — the cached hash describes the
           bytes that were there a moment ago. The generation is deliberately NOT
           reset: it counts writes this card instance has seen, and a load is not
           the game writing to the card. Leaving it monotonic is what stops a
           reload of the same state from re-triggering the warning. */
        mcd->hash_valid = 0;
    } else {
        psx_sr_skip(r, MCD_MEMORY_SIZE);
    }

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

/* --------------------------------------------------------------------------
   Fingerprint accessors. Read-only: see the contract in mcd.h.
   -------------------------------------------------------------------------- */

uint64_t psx_mcd_content_hash(psx_mcd_t* mcd) {
    if (!mcd || !mcd->buf)
        return 0;

    if (!mcd->hash_valid) {
        mcd->hash_cached = psx_state_fnv1a(mcd->buf, MCD_MEMORY_SIZE, PSX_STATE_FNV_SEED);
        mcd->hash_valid = 1;
    }

    return mcd->hash_cached;
}

uint32_t psx_mcd_write_generation(const psx_mcd_t* mcd) {
    return mcd ? mcd->write_generation : 0;
}

uint64_t psx_mcd_session_id(const psx_mcd_t* mcd) {
    return mcd ? mcd->session_id : 0;
}

int64_t psx_mcd_file_mtime(const psx_mcd_t* mcd) {
    struct stat info;

    if (!mcd || !mcd->path || !*mcd->path)
        return 0;

    /* stat() only. The path is read, never re-owned, freed or opened here. */
    if (stat(mcd->path, &info) != 0)
        return 0;

    return (int64_t)info.st_mtime;
}

void psx_mcd_destroy(psx_mcd_t* mcd) {
    FILE* file = mcd->path ? fopen(mcd->path, "wb") : NULL;

    if (file) {
        fwrite(mcd->buf, 1, MCD_MEMORY_SIZE, file);
        fclose(file);
    }

    free(mcd->buf);
    free(mcd->path);
    free(mcd);
}
