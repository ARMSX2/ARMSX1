/*
    ARMSX PS1 core — save states.

    ============================================================================
    CONTAINER FORMAT ("ARMSXPSS", format version 1)
    ============================================================================

    Everything below is written as fixed-width LITTLE-ENDIAN integers through the
    psx_sw_* / psx_sr_* helpers. No struct is ever memcpy'd into the stream, so
    the layout does not depend on the compiler's padding or on the host's byte
    order. Floats are the one exception to "integers only": they are written as
    their raw IEEE-754 binary32 bit pattern (a u32), which is portable across
    every target this core builds for but IS a hard requirement — a build on a
    non-IEEE-754 float host would read back garbage.

    File layout:

        +--------------------------------------------------------------+
        | header (32 bytes)                                            |
        +--------------------------------------------------------------+
        | section 0 (16 byte TLV header + payload)                     |
        | section 1                                                    |
        | ...                                                          |
        +--------------------------------------------------------------+

    Header:

        off  size  field
        0x00 8     magic          "ARMSXPSS"
        0x08 4     format_version PSX_STATE_FORMAT_VERSION. A reader refuses
                                  anything it does not equal exactly.
        0x0c 4     core_abi       PSX_STATE_CORE_ABI. Bumped whenever ANY
                                  section's payload layout changes, so old
                                  states are rejected with a clear error
                                  instead of being half-decoded.
        0x10 4     section_count
        0x14 4     header_size    (32; lets a future header grow)
        0x18 8     reserved       (0)

    Section TLV:

        off  size  field
        0x00 4     id             FourCC, e.g. 'CPU '. See PSX_SS_*.
        0x04 4     version        per-section payload version
        0x08 8     length         payload length in bytes
        0x10 len   payload

    Because every section carries its own length, a section the reader does not
    know about is skipped cleanly, and a device that a future core stops
    emitting simply goes missing rather than shifting every following byte. The
    reader tracks which MANDATORY sections it saw and fails the load (without
    having touched the machine) if any is absent.

    That "skipped cleanly" is what let PSX_SS_THUMB (the preview image a
    front-end's save/load picker draws) be added WITHOUT touching
    format_version or core_abi. It is not in the mandatory list, so:

      - a state written before it existed still loads, it just has no preview;
      - a state written with one still loads on a build that predates it,
        because an unknown id at section version 1 is skipped.

    Nothing already on a user's device is invalidated in either direction. The
    price of that is the obvious one: a state saved before this landed can never
    grow a thumbnail, because the pixels were never recorded. Any new section
    that changes an EXISTING payload still has to bump core_abi.

    ============================================================================
    WHAT IS *NOT* IN THE STREAM
    ============================================================================

    - Pointers, of any kind. Devices are restored in place, into the objects the
      front-end already holds references to (psx_t and every psx_*_t hanging off
      it keep their identity across a load).
    - Host resources: the open disc image, the SDL audio device, the renderer,
      the memory-card FILE paths, the psx_gpu_t::udata[] / event_cb_table[]
      front-end hooks, psx_bus_t::write_observer, psx_cpu_t::bus, the kcall
      hooks, and the cached-interpreter block cache (a derived value — it is
      invalidated on load and rebuilt lazily).
    - The BIOS and EXP1 ROM images (read-only). The BIOS is instead FINGERPRINTED
      into the identity section and a mismatch refuses the load, because a state
      taken under a different BIOS resumes into different kernel code.
    - The disc image. Only the CD-ROM controller's LOGICAL state is stored (LBA,
      state machine, sector/response/parameter FIFOs, XA + CDDA decode state,
      shell/motor bits). The disc is identified by a fingerprint taken from its
      primary volume descriptor; loading a state against a different disc, or
      against no disc at all, is refused.

    ============================================================================
    THREADING
    ============================================================================

    psx_save_state()/psx_load_state() must run on the emulation thread. Hosts on
    another thread (the Android JNI layer) call psx_state_request_slot(), which
    parks the request and blocks until the emulation thread services it from
    psx_state_service_requests() — see state.c for the handshake.
*/

#ifndef PSX_STATE_H
#define PSX_STATE_H

#include <stdint.h>
#include <stddef.h>

#define PSX_STATE_MAGIC0 0x4d525041u /* "ARMS" */
#define PSX_STATE_MAGIC1 0x53535058u /* "XPSS" */

#define PSX_STATE_FORMAT_VERSION 1u

/* Bump on ANY change to a section payload. */
#define PSX_STATE_CORE_ABI 1u

#define PSX_STATE_HEADER_SIZE 32u
#define PSX_STATE_SECTION_HEADER_SIZE 16u

#define PSX_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a)) | \
     (((uint32_t)(unsigned char)(b)) << 8) | \
     (((uint32_t)(unsigned char)(c)) << 16) | \
     (((uint32_t)(unsigned char)(d)) << 24))

#define PSX_SS_IDENTITY   PSX_FOURCC('I', 'D', 'N', 'T')
#define PSX_SS_CPU        PSX_FOURCC('C', 'P', 'U', ' ')
#define PSX_SS_BUS        PSX_FOURCC('B', 'U', 'S', ' ')
#define PSX_SS_RAM        PSX_FOURCC('R', 'A', 'M', ' ')
#define PSX_SS_SCRATCHPAD PSX_FOURCC('S', 'P', 'A', 'D')
#define PSX_SS_MC1        PSX_FOURCC('M', 'C', '1', ' ')
#define PSX_SS_MC2        PSX_FOURCC('M', 'C', '2', ' ')
#define PSX_SS_MC3        PSX_FOURCC('M', 'C', '3', ' ')
#define PSX_SS_IC         PSX_FOURCC('I', 'C', ' ', ' ')
#define PSX_SS_DMA        PSX_FOURCC('D', 'M', 'A', ' ')
#define PSX_SS_GPU        PSX_FOURCC('G', 'P', 'U', ' ')
#define PSX_SS_SPU        PSX_FOURCC('S', 'P', 'U', ' ')
#define PSX_SS_TIMER      PSX_FOURCC('T', 'M', 'R', ' ')
#define PSX_SS_CDROM      PSX_FOURCC('C', 'D', 'R', 'M')
#define PSX_SS_PAD        PSX_FOURCC('P', 'A', 'D', ' ')
#define PSX_SS_MDEC       PSX_FOURCC('M', 'D', 'E', 'C')
#define PSX_SS_EXP2       PSX_FOURCC('E', 'X', 'P', '2')
/* Optional, never mandatory — see the compatibility note above. Payload:

       off  size  field
       0x00 4     image_format   PSX_STATE_THUMB_PNG
       0x04 4     width          pixels
       0x08 4     height         pixels
       0x0c 4     image_size     bytes that follow
       0x10 n     image          the encoded image

   The width/height are metadata: the image is self-describing, but a reader
   that only wants to lay out a tile should not have to parse it to find out
   how big it is. */
#define PSX_SS_THUMB      PSX_FOURCC('T', 'H', 'M', 'B')

#define PSX_STATE_THUMB_PNG 1u

/* Refuses anything larger, so a corrupt or hostile length cannot turn a picker
   refresh into a huge allocation. A 384x288 preview is a few tens of KB. */
#define PSX_STATE_THUMB_MAX_BYTES (4u * 1024u * 1024u)

/* Flags for psx_save_state_to_memory_ex().

   NO_THUMBNAIL skips the PSX_SS_THUMB capture. The section is optional in every
   sense (see the compatibility note above), so a state written without it is an
   ordinary state that simply has no preview — it loads through the normal
   reader with no special case. Rewind and runahead pass this because the PNG
   encode is the single most expensive part of a capture and nothing is ever
   going to draw a preview of a snapshot that lives for half a second. */
#define PSX_STATE_SAVE_NO_THUMBNAIL 0x00000001u

/* psx_save_state / psx_load_state result codes. 0 == success. */
enum {
    PSX_STATE_OK = 0,
    PSX_STATE_ERR_ARG = -1,          /* NULL machine / path */
    PSX_STATE_ERR_IO = -2,           /* fopen / fread / fwrite failed */
    PSX_STATE_ERR_MAGIC = -3,        /* not an ARMSX state */
    PSX_STATE_ERR_VERSION = -4,      /* format_version / core_abi mismatch */
    PSX_STATE_ERR_TRUNCATED = -5,    /* stream ran out mid-section */
    PSX_STATE_ERR_MISSING = -6,      /* a mandatory section is absent */
    PSX_STATE_ERR_GEOMETRY = -7,     /* RAM / SPU RAM / FIFO size mismatch */
    PSX_STATE_ERR_NO_DISC = -8,      /* state has a disc, machine does not */
    PSX_STATE_ERR_WRONG_DISC = -9,   /* different disc in the drive */
    PSX_STATE_ERR_WRONG_BIOS = -10,  /* different BIOS image */
    PSX_STATE_ERR_UNSUPPORTED = -11, /* the machine cannot be captured faithfully */
    PSX_STATE_ERR_NO_MACHINE = -12,  /* no VM registered / running */
    PSX_STATE_ERR_BUSY = -13,        /* another request is already parked */
    PSX_STATE_ERR_TIMEOUT = -14      /* the emulation thread never serviced it */
};

const char* psx_state_strerror(int code);

/* -------------------------------------------------------------------------- */
/* Byte-stream helpers. Little-endian, fixed width, never a raw struct.        */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t* buf;
    size_t size;     /* bytes written */
    size_t capacity;
    int error;       /* sticky: set on an allocation failure */
} psx_state_writer_t;

typedef struct {
    const uint8_t* buf;
    size_t size;
    size_t offset;
    int error;       /* sticky: set on an over-read */
} psx_state_reader_t;

void psx_sw_init(psx_state_writer_t*);
/* Start a writer on a buffer the CALLER already owns, so a repeated capture
   (rewind, runahead) reuses one allocation instead of malloc/free per frame.
   The writer grows it in place if it has to; ownership goes back to the caller
   through the writer's buf/capacity fields. Passing NULL/0 is identical to
   psx_sw_init(). */
void psx_sw_adopt(psx_state_writer_t*, void* buf, size_t capacity);
void psx_sw_free(psx_state_writer_t*);
void psx_sw_bytes(psx_state_writer_t*, const void*, size_t);
void psx_sw_u8(psx_state_writer_t*, uint8_t);
void psx_sw_u16(psx_state_writer_t*, uint16_t);
void psx_sw_u32(psx_state_writer_t*, uint32_t);
void psx_sw_u64(psx_state_writer_t*, uint64_t);
void psx_sw_i32(psx_state_writer_t*, int32_t);
void psx_sw_i64(psx_state_writer_t*, int64_t);
void psx_sw_f32(psx_state_writer_t*, float);
/* u16/i16 arrays, written element-wise so the on-disk order is fixed. */
void psx_sw_u16_array(psx_state_writer_t*, const uint16_t*, size_t count);
void psx_sw_i16_array(psx_state_writer_t*, const int16_t*, size_t count);
void psx_sw_u32_array(psx_state_writer_t*, const uint32_t*, size_t count);
void psx_sw_i32_array(psx_state_writer_t*, const int32_t*, size_t count);

void psx_sr_init(psx_state_reader_t*, const void*, size_t);
void psx_sr_bytes(psx_state_reader_t*, void*, size_t);
void psx_sr_skip(psx_state_reader_t*, size_t);
uint8_t psx_sr_u8(psx_state_reader_t*);
uint16_t psx_sr_u16(psx_state_reader_t*);
uint32_t psx_sr_u32(psx_state_reader_t*);
uint64_t psx_sr_u64(psx_state_reader_t*);
int32_t psx_sr_i32(psx_state_reader_t*);
int64_t psx_sr_i64(psx_state_reader_t*);
float psx_sr_f32(psx_state_reader_t*);
void psx_sr_u16_array(psx_state_reader_t*, uint16_t*, size_t count);
void psx_sr_i16_array(psx_state_reader_t*, int16_t*, size_t count);
void psx_sr_u32_array(psx_state_reader_t*, uint32_t*, size_t count);
void psx_sr_i32_array(psx_state_reader_t*, int32_t*, size_t count);

/* FNV-1a, used for the BIOS and disc fingerprints. */
uint64_t psx_state_fnv1a(const void* data, size_t size, uint64_t seed);
#define PSX_STATE_FNV_SEED 0xcbf29ce484222325ull

/* -------------------------------------------------------------------------- */
/* Deferred, cross-thread request queue (see the THREADING note above).        */
/* -------------------------------------------------------------------------- */

enum {
    PSX_STATE_OP_NONE = 0,
    PSX_STATE_OP_SAVE = 1,
    PSX_STATE_OP_LOAD = 2
};

struct psx_t;

/* Registered by psx_init() / cleared by psx_destroy(). Lets a host that only
   has a JNI entry point reach the running machine without the front-end
   handing it a pointer. */
void psx_state_set_machine(struct psx_t*);

/* Emulation thread. Cheap (one atomic load) when nothing is parked. Called
   from psx_update(). */
void psx_state_service_requests(void);

/* Any thread. Parks a slot save/load and blocks up to timeout_ms for the
   emulation thread to run it. Returns PSX_STATE_OK or one of the negative
   codes above; PSX_STATE_ERR_TIMEOUT means the emulation thread never got
   there (typically: the front-end has the VM paused). base_dir is the host's
   data directory; slot files land in <base_dir>/savestates/. */
int psx_state_request_slot(int op, int slot, const char* base_dir, int timeout_ms);

/* Builds the on-disk path for a slot: <base_dir>/savestates/<game>.slot<N>.pss
   where <game> is derived from the mounted disc (sanitised file stem + the
   low 32 bits of the disc fingerprint), or "nodisc" when running the BIOS
   shell. Returns 0 on success. */
int psx_state_slot_path(struct psx_t*, int slot, const char* base_dir, char* out, size_t out_size);

/* Slot occupancy, for a front-end's save/load picker. Returns 1 when a state
   for [slot] exists under <base_dir>/savestates/ for the disc currently in the
   drive, filling out_disc_path with that disc's path; 0 otherwise (including
   "no machine registered").

   Uses the machine registered by psx_state_set_machine() so a host with only a
   JNI entry point can ask without holding a psx_t. Any thread: this only stats
   a file and reads the disc path, which does not change while a disc is
   mounted — it never touches the machine's mutable state, so it does NOT have
   to be deferred to the emulation thread the way a save or load does. */
int psx_state_slot_info(int slot, const char* base_dir, char* out_disc_path, size_t out_size);

/* -------------------------------------------------------------------------- */
/* Preview images                                                             */
/* -------------------------------------------------------------------------- */

/* Pulls the PSX_SS_THUMB image out of a state file and hands back a malloc()'d
   copy the caller frees. Returns PSX_STATE_OK, or a negative code — most often
   PSX_STATE_ERR_MISSING, which just means "that state predates thumbnails" and
   is not an error a front-end should report.

   Only the container framing is parsed: no device payload is decoded, nothing
   is written to any machine, and the core_abi is deliberately NOT checked, so a
   future ABI bump does not blind the picker. Any thread — it opens a file, and
   nothing else.

   The image is a PNG (image_format == PSX_STATE_THUMB_PNG). A caller that gets
   any other image_format should treat the state as having no preview rather
   than guessing: the format tag exists precisely so the encoding can change
   without every reader having to be updated in lockstep. */
int psx_state_read_thumbnail(const char* path, void** out_data, size_t* out_size);

/* The same, addressed by slot for the disc currently in the drive. Returns
   PSX_STATE_ERR_NO_MACHINE when nothing is running. */
int psx_state_slot_thumbnail(int slot, const char* base_dir, void** out_data, size_t* out_size);

#endif
