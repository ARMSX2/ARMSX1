#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
/* Before log.h, which pulls in frontend/diagnostics.h and its printf/fprintf/putc macros.
   Including <stdio.h> after those macros exist is a redeclaration minefield -- the same
   ordering psx/dev/gpu.c documents at its own top. */
#include <stdio.h>
#include <stdarg.h>

#include "texrep.h"
#include "texrep_png.h"
#include "dev/gpu.h"
#include "log.h"

#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define TEXREP_SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#define TEXREP_SEP '/'
#endif

/* ---- budgets ----------------------------------------------------------------------------

   All three exist to bound what an unattended session can do to a phone. A pack with more
   images than TEXREP_MAX_IMAGES, or one whose decoded pixels exceed TEXREP_MAX_BYTES, keeps
   working -- images already resident keep being used, new ones stop being admitted and one
   log line says so. Nothing is evicted, which is what lets frontend/gpu_hw_gl.c hold a
   psx_texrep_image_t* in its atlas bookkeeping across frames. */
#define TEXREP_MAX_IMAGES   4096
#define TEXREP_MAX_BYTES    (96u * 1024u * 1024u)
#define TEXREP_DUMP_BUDGET  8192

/* Power of two. Indexed by a hash of the primitive's texture ADDRESSING, and validated
   against a generation counter that every VRAM write bumps -- the key itself is over
   CONTENT, so an address that has been written since is not a cache hit. */
#define TEXREP_CACHE_SLOTS  1024

#define TEXREP_MAX_TEXELS   (PSX_TEXREP_MAX_DIM * PSX_TEXREP_MAX_DIM)

typedef struct {
    uint64_t             key;
    char*                path;   /* owned */
    psx_texrep_image_t*  img;    /* NULL until first sampled */
    uint8_t              failed; /* decode already tried and lost; never retry */
} texrep_pack_t;

typedef struct {
    uint64_t                  tag;   /* 0 = empty */
    uint64_t                  key;
    const psx_texrep_image_t* img;
    uint32_t                  gen;
} texrep_slot_t;

struct psx_texrep_t {
    int   dump;
    int   replace;
    char* base;       /* owned */
    char* dump_dir;   /* owned */
    char* pack_dir;   /* owned */

    uint32_t epoch;
    uint32_t gen;

    texrep_pack_t* pack;
    size_t         pack_n;

    psx_texrep_image_t** images;
    size_t               images_n;
    size_t               images_cap;
    size_t               image_bytes;

    texrep_slot_t cache[TEXREP_CACHE_SLOTS];

    uint64_t* dumped;       /* open-addressed set of keys already written */
    size_t    dumped_cap;
    size_t    dumped_n;
    int       dump_budget;

    int warned_images;
    int warned_dump;
};

/* ---- hashing ---------------------------------------------------------------------------- */

#define FNV64_OFFSET 1469598103934665603ull
#define FNV64_PRIME  1099511628211ull

static uint64_t texrep_fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= FNV64_PRIME;
    }

    return h;
}

static uint64_t texrep_fnv_u16(uint64_t h, uint16_t v) {
    uint8_t b[2];

    /* Explicitly little-endian so a key computed on a big-endian host names the same file. */
    b[0] = (uint8_t)(v & 0xffu);
    b[1] = (uint8_t)(v >> 8);

    return texrep_fnv(h, b, 2);
}

/* ---- paths ------------------------------------------------------------------------------ */

static char* texrep_dup(const char* s) {
    size_t n;
    char* p;

    if (!s)
        return NULL;

    n = strlen(s);
    p = (char*)malloc(n + 1u);

    if (!p)
        return NULL;

    memcpy(p, s, n + 1u);

    return p;
}

static char* texrep_join(const char* a, const char* b) {
    size_t na, nb;
    char* p;
    int sep;

    if (!a || !b)
        return NULL;

    na = strlen(a);
    nb = strlen(b);
    sep = (na > 0 && a[na - 1] != '/' && a[na - 1] != '\\') ? 1 : 0;

    p = (char*)malloc(na + (size_t)sep + nb + 1u);

    if (!p)
        return NULL;

    memcpy(p, a, na);

    if (sep)
        p[na] = TEXREP_SEP;

    memcpy(p + na + (size_t)sep, b, nb + 1u);

    return p;
}

static int texrep_mkdir(const char* path) {
#if defined(_WIN32)
    return (_mkdir(path) == 0) || (GetLastError() == ERROR_ALREADY_EXISTS) || (errno == EEXIST);
#else
    return (mkdir(path, 0755) == 0) || (errno == EEXIST);
#endif
}

/* Creates every missing component of `path`. Returns 1 if the directory exists afterwards. */
static int texrep_mkdir_p(const char* path) {
    char* work;
    size_t i, n;
    int ok;

    if (!path || !*path)
        return 0;

    work = texrep_dup(path);

    if (!work)
        return 0;

    n = strlen(work);

    for (i = 1; i < n; i++) {
        if (work[i] == '/' || work[i] == '\\') {
            char saved = work[i];

            work[i] = 0;
            texrep_mkdir(work);
            work[i] = saved;
        }
    }

    ok = texrep_mkdir(work);
    free(work);

    return ok;
}

static int texrep_file_exists(const char* path) {
    FILE* f = fopen(path, "rb");

    if (!f)
        return 0;

    fclose(f);

    return 1;
}

/* ---- the axis fold ---------------------------------------------------------------------- */

void psx_texrep_fold_axis(unsigned lo_raw, unsigned count, unsigned mask, unsigned off,
                          uint16_t* out_lo, uint16_t* out_count) {
    unsigned lo = 0x100u, hi = 0;
    unsigned i;

    if (count == 0)
        count = 1;

    if (count > 256u)
        count = 256u;

    /* No window: the range is already the answer, and the loop below would agree. Taken
       explicitly because the overwhelming majority of primitives are in this state. */
    if ((mask & 0xffu) == 0) {
        *out_lo = (uint16_t)(lo_raw & 0xffu);

        /* A raw range that wraps past 255 covers everything once it is 256 long. */
        if (((lo_raw & 0xffu) + count) > 256u) {
            *out_lo = 0;
            *out_count = 256;
        } else {
            *out_count = (uint16_t)count;
        }

        return;
    }

    for (i = 0; i < count; i++) {
        unsigned t = (lo_raw + i) & 0xffu;

        t = ((t & ~mask) | (off & mask)) & 0xffu;

        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }

    *out_lo = (uint16_t)lo;
    *out_count = (uint16_t)(hi - lo + 1u);
}

/* ---- key ---------------------------------------------------------------------------------

   See psx/texrep.h for the format this implements; the order below IS the documented order
   and changing it renames every file in every pack. */

static uint16_t texrep_vram_at(const psx_gpu_t* gpu, uint32_t lin) {
    /* gpu_fetch_texel() indexes VRAM linearly and does NOT wrap the page at x=1024
       (the backend deviation #8); the GLES shader masks to keep a
       pathological page from reading past the surface. Same mask here so the hash covers
       exactly the halfwords the rasterizers can reach. */
    return gpu->vram[lin & 0x7ffffu];
}

static uint64_t texrep_key(const psx_gpu_t* gpu, uint32_t tpx, uint32_t tpy,
                           uint32_t clutx, uint32_t cluty, int depth,
                           uint16_t u0, uint16_t v0, uint16_t w, uint16_t h) {
    uint64_t k = FNV64_OFFSET;
    uint8_t hdr[9];
    int shift = (depth == 0) ? 2 : ((depth == 1) ? 1 : 0);
    uint32_t hx0 = ((uint32_t)u0) >> shift;
    uint32_t hx1 = ((uint32_t)(u0 + w - 1)) >> shift;
    uint32_t row, x;
    int clut_n = (depth == 0) ? 16 : ((depth == 1) ? 256 : 0);
    int i;

    hdr[0] = (uint8_t)((depth >= 2) ? 2 : depth);
    hdr[1] = (uint8_t)(w & 0xffu);
    hdr[2] = (uint8_t)(w >> 8);
    hdr[3] = (uint8_t)(h & 0xffu);
    hdr[4] = (uint8_t)(h >> 8);
    hdr[5] = (uint8_t)(gpu->texw_mx & 0xffu);
    hdr[6] = (uint8_t)(gpu->texw_my & 0xffu);
    hdr[7] = (uint8_t)(gpu->texw_ox & 0xffu);
    hdr[8] = (uint8_t)(gpu->texw_oy & 0xffu);

    k = texrep_fnv(k, hdr, sizeof(hdr));

    for (i = 0; i < clut_n; i++)
        k = texrep_fnv_u16(k, texrep_vram_at(gpu, (clutx + (uint32_t)i) + (cluty << 10)));

    for (row = 0; row < (uint32_t)h; row++) {
        uint32_t base = tpx + ((tpy + v0 + row) << 10);

        for (x = hx0; x <= hx1; x++)
            k = texrep_fnv_u16(k, texrep_vram_at(gpu, base + x));
    }

    /* 0 is the cache's "empty" sentinel; steer the one key that would collide with it. */
    return k ? k : 1u;
}

/* ---- image store -------------------------------------------------------------------------- */

static void texrep_image_free(psx_texrep_image_t* im) {
    if (!im)
        return;

    free(im->rgba);
    free(im);
}

static int texrep_admit(struct psx_texrep_t* t, psx_texrep_image_t* im) {
    size_t bytes = (size_t)im->pw * (size_t)im->ph * 4u;

    if (t->images_n >= TEXREP_MAX_IMAGES || (t->image_bytes + bytes) > TEXREP_MAX_BYTES) {
        if (!t->warned_images) {
            t->warned_images = 1;
            log_warn("texrep: replacement budget reached (%u images, %u MiB); "
                     "further textures will not be replaced",
                     (unsigned)t->images_n, (unsigned)(t->image_bytes >> 20));
        }

        return 0;
    }

    if (t->images_n == t->images_cap) {
        size_t cap = t->images_cap ? (t->images_cap * 2u) : 64u;
        psx_texrep_image_t** grow =
            (psx_texrep_image_t**)realloc(t->images, cap * sizeof(*grow));

        if (!grow)
            return 0;

        t->images = grow;
        t->images_cap = cap;
    }

    t->images[t->images_n++] = im;
    t->image_bytes += bytes;

    return 1;
}

/* ---- pack index --------------------------------------------------------------------------- */

static int texrep_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

/* Pulls the first run of exactly-16 hex digits out of a basename, so ps1-<key>-64x32-4.png,
   ps1-<key>-whatever-the-author-called-it.png and <key>.png all name the same texture. */
static int texrep_key_from_name(const char* name, uint64_t* out) {
    size_t i = 0;

    while (name[i]) {
        if (texrep_hex((unsigned char)name[i]) >= 0) {
            size_t j = i;
            uint64_t v = 0;

            while (name[j] && texrep_hex((unsigned char)name[j]) >= 0)
                j++;

            if ((j - i) == 16u) {
                size_t k;

                for (k = i; k < j; k++)
                    v = (v << 4) | (uint64_t)texrep_hex((unsigned char)name[k]);

                *out = v;

                return 1;
            }

            i = j;
            continue;
        }

        i++;
    }

    return 0;
}

static int texrep_is_png(const char* name) {
    size_t n = strlen(name);

    if (n < 5u)
        return 0;

    return (name[n - 4] == '.') &&
           (name[n - 3] == 'p' || name[n - 3] == 'P') &&
           (name[n - 2] == 'n' || name[n - 2] == 'N') &&
           (name[n - 1] == 'g' || name[n - 1] == 'G');
}

static void texrep_pack_add(struct psx_texrep_t* t, size_t* cap, uint64_t key, char* path) {
    if (t->pack_n == *cap) {
        size_t next = *cap ? (*cap * 2u) : 128u;
        texrep_pack_t* grow = (texrep_pack_t*)realloc(t->pack, next * sizeof(*grow));

        if (!grow) {
            free(path);

            return;
        }

        t->pack = grow;
        *cap = next;
    }

    t->pack[t->pack_n].key = key;
    t->pack[t->pack_n].path = path;
    t->pack[t->pack_n].img = NULL;
    t->pack[t->pack_n].failed = 0;
    t->pack_n++;
}

static void texrep_scan_dir(struct psx_texrep_t* t, size_t* cap, const char* dir, int depth);

static void texrep_scan_entry(struct psx_texrep_t* t, size_t* cap, const char* dir,
                              const char* name, int is_dir, int depth) {
    uint64_t key;
    char* full;

    if (name[0] == '.')
        return;

    full = texrep_join(dir, name);

    if (!full)
        return;

    if (is_dir) {
        /* One level of subdirectories, so a pack author can group by area or character
           without the loader walking a whole SD card. */
        if (depth < 2)
            texrep_scan_dir(t, cap, full, depth + 1);

        free(full);

        return;
    }

    if (texrep_is_png(name) && texrep_key_from_name(name, &key)) {
        texrep_pack_add(t, cap, key, full);

        return;
    }

    free(full);
}

static void texrep_scan_dir(struct psx_texrep_t* t, size_t* cap, const char* dir, int depth) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE hnd;
    char* pattern = texrep_join(dir, "*");

    if (!pattern)
        return;

    hnd = FindFirstFileA(pattern, &fd);
    free(pattern);

    if (hnd == INVALID_HANDLE_VALUE)
        return;

    do {
        texrep_scan_entry(t, cap, dir, fd.cFileName,
                          (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, depth);
    } while (FindNextFileA(hnd, &fd));

    FindClose(hnd);
#else
    DIR* d = opendir(dir);
    struct dirent* e;

    if (!d)
        return;

    while ((e = readdir(d)) != NULL) {
        int is_dir = 0;
        char* full = texrep_join(dir, e->d_name);

        if (full) {
            struct stat st;

            if (stat(full, &st) == 0)
                is_dir = S_ISDIR(st.st_mode) ? 1 : 0;

            free(full);
        }

        texrep_scan_entry(t, cap, dir, e->d_name, is_dir, depth);
    }

    closedir(d);
#endif
}

static int texrep_pack_cmp(const void* a, const void* b) {
    const texrep_pack_t* x = (const texrep_pack_t*)a;
    const texrep_pack_t* y = (const texrep_pack_t*)b;

    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;

    return 0;
}

static texrep_pack_t* texrep_pack_find(struct psx_texrep_t* t, uint64_t key) {
    size_t lo = 0, hi = t->pack_n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;

        if (t->pack[mid].key < key)
            lo = mid + 1u;
        else if (t->pack[mid].key > key)
            hi = mid;
        else
            return &t->pack[mid];
    }

    return NULL;
}

/* ---- dumped-key set ----------------------------------------------------------------------- */

static int texrep_dumped_mark(struct psx_texrep_t* t, uint64_t key) {
    size_t i, mask;

    if (!t->dumped) {
        t->dumped_cap = 2048u;
        t->dumped = (uint64_t*)calloc(t->dumped_cap, sizeof(uint64_t));

        if (!t->dumped) {
            t->dumped_cap = 0;

            return 1;   /* no set means "dump it"; the on-disk existence check still guards */
        }
    }

    mask = t->dumped_cap - 1u;
    i = (size_t)key & mask;

    for (;;) {
        if (t->dumped[i] == key)
            return 0;

        if (t->dumped[i] == 0) {
            t->dumped[i] = key;
            t->dumped_n++;

            /* Past 3/4 full, stop recording rather than rehash: at that point the session
               has already dumped thousands of textures and the on-disk check takes over. */
            return 1;
        }

        i = (i + 1u) & mask;

        if (t->dumped_n > ((t->dumped_cap * 3u) / 4u))
            return 1;
    }
}

/* ---- dumping ------------------------------------------------------------------------------- */

static void texrep_dump(struct psx_texrep_t* t, psx_gpu_t* gpu, uint64_t key,
                        uint32_t tpx, uint32_t tpy, uint32_t clutx, uint32_t cluty, int depth,
                        uint16_t u0, uint16_t v0, uint16_t w, uint16_t h) {
    char name[96];
    char* path;
    uint8_t* rgba;
    int x, y;

    if (t->dump_budget <= 0) {
        if (!t->warned_dump) {
            t->warned_dump = 1;
            log_warn("texrep: dump budget of %d textures reached; stopping",
                     TEXREP_DUMP_BUDGET);
        }

        return;
    }

    if (!texrep_dumped_mark(t, key))
        return;

    snprintf(name, sizeof(name), "ps1-%016llx-%ux%u-%d.png",
             (unsigned long long)key, (unsigned)w, (unsigned)h,
             (depth == 0) ? 4 : ((depth == 1) ? 8 : 16));

    path = texrep_join(t->dump_dir, name);

    if (!path)
        return;

    if (texrep_file_exists(path)) {
        free(path);

        return;
    }

    rgba = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);

    if (!rgba) {
        free(path);

        return;
    }

    /* Through the REAL sampler, so the texture window, the CLUT and the depth packing are
       applied by the same code the rasterizers use rather than by a second implementation
       that could drift from it. gpu->texrep_bind.img is NULL here -- psx_texrep_bind_prim()
       clears it on entry -- so this cannot recurse into a replacement. */
    for (y = 0; y < (int)h; y++) {
        for (x = 0; x < (int)w; x++) {
            uint16_t texel = gpu_fetch_texel(gpu, (uint16_t)(u0 + x), (uint16_t)(v0 + y),
                                             tpx, tpy, (uint16_t)clutx, (uint16_t)cluty,
                                             depth);
            uint8_t* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            unsigned r5 = texel & 0x1fu;
            unsigned g5 = (texel >> 5) & 0x1fu;
            unsigned b5 = (texel >> 10) & 0x1fu;

            if (texel == 0) {
                p[0] = p[1] = p[2] = p[3] = 0;

                continue;
            }

            /* 5 -> 8 as (v<<3)|(v>>2), which is exactly invertible by v>>3. That is what
               makes a dump fed back as a replacement bit-identical; see psx/texrep.h. */
            p[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            p[1] = (uint8_t)((g5 << 3) | (g5 >> 2));
            p[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
            p[3] = (texel & 0x8000u) ? 128u : 255u;
        }
    }

    if (psx_png_write(path, rgba, (int)w, (int)h))
        t->dump_budget--;
    else
        log_warn("texrep: could not write %s", path);

    free(rgba);
    free(path);
}

/* ---- replacement load ----------------------------------------------------------------------- */

static psx_texrep_image_t* texrep_load(struct psx_texrep_t* t, texrep_pack_t* e,
                                       uint16_t nw, uint16_t nh) {
    FILE* f;
    long size;
    uint8_t* bytes;
    uint8_t* rgba;
    const char* why = NULL;
    int w = 0, h = 0;
    psx_texrep_image_t* im;
    int scale;

    if (e->failed)
        return NULL;

    e->failed = 1;   /* cleared only on the success path, so a bad file is read once */

    f = fopen(e->path, "rb");

    if (!f) {
        log_warn("texrep: cannot open %s", e->path);

        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);

        return NULL;
    }

    size = ftell(f);

    if (size <= 0 || size > (32L * 1024L * 1024L)) {
        fclose(f);
        log_warn("texrep: %s is empty or implausibly large", e->path);

        return NULL;
    }

    rewind(f);
    bytes = (uint8_t*)malloc((size_t)size);

    if (!bytes) {
        fclose(f);

        return NULL;
    }

    if (fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        free(bytes);
        fclose(f);

        return NULL;
    }

    fclose(f);

    rgba = psx_png_decode(bytes, (size_t)size, &w, &h, &why);
    free(bytes);

    if (!rgba) {
        log_warn("texrep: %s rejected: %s", e->path, why ? why : "unreadable");

        return NULL;
    }

    /* Integer multiples only. Anything else has no defensible sub-texel mapping, and
       guessing one is how a pack ends up half a texel off everywhere. */
    scale = (nw > 0) ? (w / (int)nw) : 0;

    if (scale < 1 || scale > PSX_TEXREP_MAX_SCALE ||
        (int)nw * scale != w || (int)nh * scale != h) {
        log_warn("texrep: %s is %dx%d but must be %ux%u times an integer 1..%d",
                 e->path, w, h, (unsigned)nw, (unsigned)nh, PSX_TEXREP_MAX_SCALE);
        free(rgba);

        return NULL;
    }

    im = (psx_texrep_image_t*)calloc(1, sizeof(*im));

    if (!im) {
        free(rgba);

        return NULL;
    }

    im->key = e->key;
    im->nw = nw;
    im->nh = nh;
    im->pw = (uint16_t)w;
    im->ph = (uint16_t)h;
    im->scale = (uint8_t)scale;
    im->rgba = rgba;

    if (!texrep_admit(t, im)) {
        texrep_image_free(im);

        return NULL;
    }

    e->failed = 0;
    e->img = im;

    return im;
}

/* ---- lifetime -------------------------------------------------------------------------------- */

static void texrep_free(struct psx_texrep_t* t) {
    size_t i;

    if (!t)
        return;

    for (i = 0; i < t->pack_n; i++)
        free(t->pack[i].path);

    free(t->pack);

    for (i = 0; i < t->images_n; i++)
        texrep_image_free(t->images[i]);

    free(t->images);
    free(t->dumped);
    free(t->base);
    free(t->dump_dir);
    free(t->pack_dir);
    free(t);
}

void psx_texrep_destroy(psx_gpu_t* gpu) {
    if (!gpu)
        return;

    texrep_free(gpu->texrep);
    gpu->texrep = NULL;
    gpu->texrep_bind.img = NULL;
    gpu->texrep_bind.u0 = 0;
    gpu->texrep_bind.v0 = 0;
}

uint32_t psx_texrep_epoch(const psx_gpu_t* gpu) {
    return (gpu && gpu->texrep) ? gpu->texrep->epoch : 0u;
}

void psx_texrep_invalidate(psx_gpu_t* gpu) {
    if (gpu && gpu->texrep)
        gpu->texrep->gen++;
}

void psx_texrep_configure(psx_gpu_t* gpu, int dump_enabled, int replace_enabled,
                          const char* dir) {
    struct psx_texrep_t* t;
    uint32_t epoch;

    if (!gpu)
        return;

    if (!dir || !*dir)
        dump_enabled = replace_enabled = 0;

    /* Already in exactly this state: do nothing rather than rebuild, because this is called
       from a settings push that fires on every edit and a rebuild rescans the pack. */
    if (gpu->texrep &&
        gpu->texrep->dump == (dump_enabled ? 1 : 0) &&
        gpu->texrep->replace == (replace_enabled ? 1 : 0) &&
        gpu->texrep->base && dir && strcmp(gpu->texrep->base, dir) == 0)
        return;

    epoch = gpu->texrep ? gpu->texrep->epoch : 0u;
    psx_texrep_destroy(gpu);

    if (!dump_enabled && !replace_enabled)
        return;

    t = (struct psx_texrep_t*)calloc(1, sizeof(*t));

    if (!t)
        return;

    t->dump = dump_enabled ? 1 : 0;
    t->replace = replace_enabled ? 1 : 0;
    t->base = texrep_dup(dir);
    t->dump_dir = texrep_join(dir, "dump");
    t->pack_dir = texrep_join(dir, "replacements");
    t->epoch = epoch + 1u;
    t->gen = 1u;
    t->dump_budget = TEXREP_DUMP_BUDGET;

    if (!t->base || !t->dump_dir || !t->pack_dir) {
        texrep_free(t);

        return;
    }

    if (t->dump && !texrep_mkdir_p(t->dump_dir)) {
        log_warn("texrep: cannot create %s; dumping disabled", t->dump_dir);
        t->dump = 0;
    }

    if (t->replace) {
        size_t cap = 0;

        texrep_mkdir_p(t->pack_dir);
        texrep_scan_dir(t, &cap, t->pack_dir, 0);

        if (t->pack_n > 1u)
            qsort(t->pack, t->pack_n, sizeof(t->pack[0]), texrep_pack_cmp);

        log_info("texrep: %u replacement texture(s) indexed from %s",
                 (unsigned)t->pack_n, t->pack_dir);
    }

    if (t->dump)
        log_info("texrep: dumping to %s", t->dump_dir);

    if (!t->dump && !t->replace) {
        texrep_free(t);

        return;
    }

    gpu->texrep = t;
    gpu->texrep_bind.img = NULL;
}

/* ---- per-primitive bind -------------------------------------------------------------------- */

void psx_texrep_unbind(psx_gpu_t* gpu) {
    gpu->texrep_bind.img = NULL;
}

void psx_texrep_bind_prim(psx_gpu_t* gpu, const uint16_t* u, const uint16_t* v, int n,
                          uint32_t tpx, uint32_t tpy, uint32_t clutx, uint32_t cluty,
                          int depth) {
    struct psx_texrep_t* t;
    unsigned ulo, ucount, vlo, vcount;
    uint16_t u0, v0, w, h;
    uint64_t tag, key;
    texrep_slot_t* slot;
    const psx_texrep_image_t* img = NULL;

    gpu->texrep_bind.img = NULL;


    t = gpu->texrep;

    if (!t || n < 2)
        return;

    if (n == 2) {
        /* Sprite: an inclusive RAW range that may run past 255 and wrap, which is what
           gpu_render_rect() does when it adds the sprite width to v0.tx. */
        ulo = u[0];
        ucount = (unsigned)(u[1] - u[0] + 1);
        vlo = v[0];
        vcount = (unsigned)(v[1] - v[0] + 1);
    } else {
        int i;
        unsigned umin = u[0], umax = u[0], vmin = v[0], vmax = v[0];

        for (i = 1; i < n; i++) {
            if (u[i] < umin) umin = u[i];
            if (u[i] > umax) umax = u[i];
            if (v[i] < vmin) vmin = v[i];
            if (v[i] > vmax) vmax = v[i];
        }

        ulo = umin;
        ucount = umax - umin + 1u;
        vlo = vmin;
        vcount = vmax - vmin + 1u;
    }

    psx_texrep_fold_axis(ulo, ucount, gpu->texw_mx, gpu->texw_ox, &u0, &w);
    psx_texrep_fold_axis(vlo, vcount, gpu->texw_my, gpu->texw_oy, &v0, &h);

    if (w == 0 || h == 0 || w > PSX_TEXREP_MAX_DIM || h > PSX_TEXREP_MAX_DIM ||
        ((uint32_t)w * (uint32_t)h) > TEXREP_MAX_TEXELS)
        return;

    /* Address-space identity of this fetch, for the key cache only. It is a hash rather than
       a bit-packing because the fields do not fit in 64 bits; a collision costs a wrong
       replacement, never a crash, and at 64 bits over ten small integers there is not one. */
    tag = texrep_fnv(FNV64_OFFSET, &tpx, sizeof(tpx));
    tag = texrep_fnv(tag, &tpy, sizeof(tpy));
    tag = texrep_fnv(tag, &clutx, sizeof(clutx));
    tag = texrep_fnv(tag, &cluty, sizeof(cluty));
    tag = texrep_fnv(tag, &depth, sizeof(depth));
    tag = texrep_fnv_u16(tag, u0);
    tag = texrep_fnv_u16(tag, v0);
    tag = texrep_fnv_u16(tag, w);
    tag = texrep_fnv_u16(tag, h);
    tag = texrep_fnv_u16(tag, (uint16_t)((gpu->texw_mx & 0xffu) | ((gpu->texw_my & 0xffu) << 8)));
    tag = texrep_fnv_u16(tag, (uint16_t)((gpu->texw_ox & 0xffu) | ((gpu->texw_oy & 0xffu) << 8)));

    if (!tag)
        tag = 1u;

    slot = &t->cache[(size_t)tag & (TEXREP_CACHE_SLOTS - 1u)];

    if (slot->tag == tag && slot->gen == t->gen) {
        img = slot->img;
    } else {
        key = texrep_key(gpu, tpx, tpy, clutx, cluty, depth, u0, v0, w, h);

        if (t->dump)
            texrep_dump(t, gpu, key, tpx, tpy, clutx, cluty, depth, u0, v0, w, h);

        if (t->replace) {
            texrep_pack_t* e = texrep_pack_find(t, key);

            if (e)
                img = e->img ? e->img : texrep_load(t, e, w, h);
        }

        slot->tag = tag;
        slot->key = key;
        slot->img = img;
        slot->gen = t->gen;
    }

    if (img) {
        gpu->texrep_bind.img = img;
        gpu->texrep_bind.u0 = u0;
        gpu->texrep_bind.v0 = v0;
    }
}

/* ---- sampling --------------------------------------------------------------------------------

   The C half of the mapping PSX_TEXREP_GLSL states for the shader. Both must agree, which is
   why the shader compiles from a string in the header rather than from a second hand-written
   copy -- ,  and  were all one formula written three times. */

uint16_t psx_texrep_sample(const psx_gpu_t* gpu, float tx, float ty) {
    const psx_texrep_image_t* im = gpu->texrep_bind.img;
    unsigned itx, ity;
    int lx, ly, fx = 0, fy = 0;
    int s;
    const uint8_t* p;
    unsigned a, c;

    if (!im)
        return 0;

    /*
        The same window transform gpu_fetch_texel() applies, and idempotent, so folding an
        already-folded coordinate is a no-op.

        Truncate to int and mask, rather than casting the float straight to uint16_t as the
        call sites historically do. Float -> unsigned is UNDEFINED for a negative value and
        the two architectures this ships on disagree about it: ARM64's fcvtzu saturates to 0,
        x86's cvttss2si wraps. Float -> int is defined for anything in range, and the mask
        after it is plain integer arithmetic, so this lands on the same texel everywhere.
        A rasterizer can present a slightly negative UV on an edge fragment, so the case is
        reachable, not theoretical.
    */
    itx = ((((unsigned)((int)tx & 0xffff)) & ~gpu->texw_mx) | (gpu->texw_ox & gpu->texw_mx)) & 0xffu;
    ity = ((((unsigned)((int)ty & 0xffff)) & ~gpu->texw_my) | (gpu->texw_oy & gpu->texw_my)) & 0xffu;

    lx = (int)itx - (int)gpu->texrep_bind.u0;
    ly = (int)ity - (int)gpu->texrep_bind.v0;

    if (lx < 0) lx = 0; else if (lx >= (int)im->nw) lx = (int)im->nw - 1;
    if (ly < 0) ly = 0; else if (ly >= (int)im->nh) ly = (int)im->nh - 1;

    s = (int)im->scale;

    if (s > 1) {
        float ftx = tx - floorf(tx);
        float fty = ty - floorf(ty);

        fx = (int)(ftx * (float)s);
        fy = (int)(fty * (float)s);

        if (fx < 0) fx = 0; else if (fx >= s) fx = s - 1;
        if (fy < 0) fy = 0; else if (fy >= s) fy = s - 1;
    }

    p = im->rgba + (((size_t)(ly * s + fy) * (size_t)im->pw) + (size_t)(lx * s + fx)) * 4u;

    a = p[3];

    if (a == 0)
        return 0;

    c = ((unsigned)p[0] >> 3) | (((unsigned)p[1] >> 3) << 5) | (((unsigned)p[2] >> 3) << 10);

    if (a != 255u)
        c |= 0x8000u;

    return (uint16_t)c;
}
