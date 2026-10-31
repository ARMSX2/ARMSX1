/*
    ARMSX PS1 core — save-state preview thumbnails. See thumbnail.h for what is
    captured and why the encoder is here rather than in the front-end.
*/

#include "thumbnail.h"
#include "state.h"
#include "psx.h"
#include "bus_init.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Checksums                                                                  */
/* -------------------------------------------------------------------------- */

/* Nibble-table CRC-32 (polynomial 0xedb88320, reflected), used for the PNG
   chunk CRCs. A 16-entry table rather than 256 keeps the constant data small
   without making the per-byte cost matter at thumbnail sizes. */
static uint32_t png_crc32(uint32_t crc, const uint8_t* data, size_t size) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    size_t i;

    crc = ~crc;

    for (i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0x0f] ^ (crc >> 4);
        crc = table[(crc ^ (data[i] >> 4)) & 0x0f] ^ (crc >> 4);
    }

    return ~crc;
}

/* Adler-32 of the UNCOMPRESSED data — the zlib stream's trailer. */
static uint32_t png_adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1;
    uint32_t b = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    return (b << 16) | a;
}

/* -------------------------------------------------------------------------- */
/* Bit-level output                                                           */
/* -------------------------------------------------------------------------- */

/* psx_state_writer_t already is a growable byte buffer with a sticky error flag
   (state.h), so this only adds DEFLATE's bit accumulator on top of it. */
typedef struct {
    psx_state_writer_t w;
    uint32_t bits;
    int bit_count;
} png_bitwriter_t;

/* DEFLATE packs bits into bytes starting at the least-significant bit.
   bit_count is always < 8 on entry and count is at most 13 (the widest extra-
   bits field), so the accumulator never overflows. */
static void png_put_bits(png_bitwriter_t* b, uint32_t value, int count) {
    if (count <= 0)
        return;

    b->bits |= (value & ((1u << count) - 1u)) << b->bit_count;
    b->bit_count += count;

    while (b->bit_count >= 8) {
        psx_sw_u8(&b->w, (uint8_t)(b->bits & 0xffu));

        b->bits >>= 8;
        b->bit_count -= 8;
    }
}

/* Huffman codes go out most-significant bit FIRST, which is the opposite order
   to everything else in the stream, so the code is reversed before packing
   (RFC 1951 3.1.1). */
static void png_put_code(png_bitwriter_t* b, uint32_t code, int count) {
    uint32_t reversed = 0;
    int i;

    for (i = 0; i < count; i++)
        reversed = (reversed << 1) | ((code >> i) & 1u);

    png_put_bits(b, reversed, count);
}

/* The fixed literal/length alphabet, RFC 1951 3.2.6. */
static void png_put_symbol(png_bitwriter_t* b, int symbol) {
    if (symbol < 144)
        png_put_code(b, (uint32_t)(0x30 + symbol), 8);
    else if (symbol < 256)
        png_put_code(b, (uint32_t)(0x190 + (symbol - 144)), 9);
    else if (symbol < 280)
        png_put_code(b, (uint32_t)(symbol - 256), 7);
    else
        png_put_code(b, (uint32_t)(0xc0 + (symbol - 280)), 8);
}

/* -------------------------------------------------------------------------- */
/* DEFLATE (one fixed-Huffman block, greedy LZ77)                             */
/* -------------------------------------------------------------------------- */

static const uint16_t png_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t png_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t png_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t png_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void png_put_match(png_bitwriter_t* b, int length, int distance) {
    int code = 28;
    int dist_code = 29;

    while (code > 0 && png_length_base[code] > length)
        code--;

    png_put_symbol(b, 257 + code);
    png_put_bits(b, (uint32_t)(length - png_length_base[code]), png_length_extra[code]);

    while (dist_code > 0 && png_dist_base[dist_code] > distance)
        dist_code--;

    /* Fixed distance codes are 5-bit, and the code IS the index. */
    png_put_code(b, (uint32_t)dist_code, 5);
    png_put_bits(b, (uint32_t)(distance - png_dist_base[dist_code]), png_dist_extra[dist_code]);
}

#define PNG_HASH_BITS 15
#define PNG_HASH_SIZE (1 << PNG_HASH_BITS)
#define PNG_MIN_MATCH 3
#define PNG_MAX_MATCH 258
#define PNG_WINDOW 32768

static uint32_t png_hash3(const uint8_t* p) {
    uint32_t key = ((uint32_t)p[0] << 16) ^ ((uint32_t)p[1] << 8) ^ (uint32_t)p[2];

    return (key * 2654435761u) >> (32 - PNG_HASH_BITS);
}

/*
    One BFINAL fixed-Huffman block over the whole input.

    The match search keeps only the MOST RECENT position for each 3-byte hash
    (no chain walk). That is a deliberate trade: on PNG-filtered image data the
    win comes overwhelmingly from long runs — a filtered flat region is a run of
    zeroes, and the most recent candidate for those is one byte back, which
    extends into a single 258-byte overlapping match. A full chain search would
    buy a few more percent for several times the code.
*/
static void png_deflate(png_bitwriter_t* b, const uint8_t* data, size_t size) {
    int32_t* head;
    size_t pos = 0;
    int i;

    head = (int32_t*)malloc(PNG_HASH_SIZE * sizeof(int32_t));

    if (!head) {
        b->w.error = 1;
        return;
    }

    for (i = 0; i < PNG_HASH_SIZE; i++)
        head[i] = -1;

    png_put_bits(b, 1, 1); /* BFINAL */
    png_put_bits(b, 1, 2); /* BTYPE = 01, fixed Huffman */

    while (pos < size) {
        int best_length = 0;
        int best_distance = 0;

        if ((size - pos) >= PNG_MIN_MATCH) {
            uint32_t slot = png_hash3(data + pos);
            int32_t candidate = head[slot];

            head[slot] = (int32_t)pos;

            if (candidate >= 0) {
                size_t distance = pos - (size_t)candidate;

                if (distance > 0 && distance <= PNG_WINDOW) {
                    size_t limit = size - pos;
                    size_t length = 0;

                    if (limit > PNG_MAX_MATCH)
                        limit = PNG_MAX_MATCH;

                    /* candidate + length may run past pos: an overlapping match
                       is legal DEFLATE and is exactly what turns a run into one
                       length/distance pair. */
                    while (length < limit && data[(size_t)candidate + length] == data[pos + length])
                        length++;

                    if (length >= PNG_MIN_MATCH) {
                        best_length = (int)length;
                        best_distance = (int)distance;
                    }
                }
            }
        }

        if (best_length >= PNG_MIN_MATCH) {
            size_t k;

            png_put_match(b, best_length, best_distance);

            /* Index the bytes the match covered so later positions can still
               find them. */
            for (k = 1; k < (size_t)best_length; k++) {
                if ((pos + k + PNG_MIN_MATCH) > size)
                    break;

                head[png_hash3(data + pos + k)] = (int32_t)(pos + k);
            }

            pos += (size_t)best_length;
        } else {
            png_put_symbol(b, data[pos]);

            pos++;
        }
    }

    png_put_symbol(b, 256); /* end of block */

    if (b->bit_count > 0)
        png_put_bits(b, 0, 8 - b->bit_count);

    free(head);
}

/* -------------------------------------------------------------------------- */
/* PNG container                                                              */
/* -------------------------------------------------------------------------- */

static void png_put_be32(psx_state_writer_t* w, uint32_t value) {
    psx_sw_u8(w, (uint8_t)((value >> 24) & 0xffu));
    psx_sw_u8(w, (uint8_t)((value >> 16) & 0xffu));
    psx_sw_u8(w, (uint8_t)((value >> 8) & 0xffu));
    psx_sw_u8(w, (uint8_t)(value & 0xffu));
}

/* length + type + payload + CRC(type, payload). */
static void png_put_chunk(psx_state_writer_t* w, const char* type,
                          const uint8_t* data, size_t size) {
    uint32_t crc;

    png_put_be32(w, (uint32_t)size);
    psx_sw_bytes(w, type, 4);
    psx_sw_bytes(w, data, size);

    crc = png_crc32(0, (const uint8_t*)type, 4);
    crc = png_crc32(crc, data, size);

    png_put_be32(w, crc);
}

/* PNG's five per-row filters (RFC 2083 6). bpp is the byte offset of the
   pixel to the left; a/b/c are left / above / above-left. */
static uint8_t png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
        return (uint8_t)a;

    if (pb <= pc)
        return (uint8_t)b;

    return (uint8_t)c;
}

static void png_filter_row(int filter, const uint8_t* row, const uint8_t* prev,
                           size_t row_bytes, int bpp, uint8_t* out) {
    size_t i;

    for (i = 0; i < row_bytes; i++) {
        int a = (i >= (size_t)bpp) ? row[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
        int predictor;

        switch (filter) {
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: predictor = png_paeth(a, b, c); break;
            default: predictor = 0; break;
        }

        out[i] = (uint8_t)((int)row[i] - predictor);
    }
}

/* The reference heuristic: pick the filter whose output has the smallest sum of
   absolute values read as signed bytes, which correlates well with how well the
   row will then compress. */
static size_t png_filter_score(const uint8_t* data, size_t size) {
    size_t score = 0;
    size_t i;

    for (i = 0; i < size; i++)
        score += (data[i] < 128) ? data[i] : (size_t)(256 - data[i]);

    return score;
}

int psx_png_encode_rgb(const uint8_t* rgb, int width, int height,
                       void** out_data, size_t* out_size) {
    png_bitwriter_t b;
    psx_state_writer_t idat;
    uint8_t header[13];
    uint8_t* raw;
    uint8_t* candidate;
    size_t row_bytes;
    size_t raw_size;
    int y;
    int filter;

    if (!out_data || !out_size)
        return -1;

    *out_data = NULL;
    *out_size = 0;

    if (!rgb || width <= 0 || height <= 0)
        return -1;

    row_bytes = (size_t)width * 3u;
    raw_size = (size_t)height * (row_bytes + 1u);

    raw = (uint8_t*)malloc(raw_size);
    candidate = (uint8_t*)malloc(row_bytes * 5u);

    if (!raw || !candidate) {
        free(raw);
        free(candidate);
        return -1;
    }

    /* Filter every row, keeping the cheapest of the five. */
    for (y = 0; y < height; y++) {
        const uint8_t* row = rgb + ((size_t)y * row_bytes);
        const uint8_t* prev = y ? (rgb + ((size_t)(y - 1) * row_bytes)) : NULL;
        uint8_t* dst = raw + ((size_t)y * (row_bytes + 1u));
        int best = 0;
        size_t best_score = 0;

        for (filter = 0; filter < 5; filter++) {
            uint8_t* slot = candidate + ((size_t)filter * row_bytes);
            size_t score;

            png_filter_row(filter, row, prev, row_bytes, 3, slot);

            score = png_filter_score(slot, row_bytes);

            if (filter == 0 || score < best_score) {
                best = filter;
                best_score = score;
            }
        }

        dst[0] = (uint8_t)best;
        memcpy(dst + 1, candidate + ((size_t)best * row_bytes), row_bytes);
    }

    free(candidate);

    /* zlib stream: 0x78 0x01 (deflate, 32K window, no preset dictionary; the
       header word is a multiple of 31 as the format requires), the block, then
       the Adler-32 of the unfiltered-through-filtered bytes we just built. */
    psx_sw_init(&b.w);
    b.bits = 0;
    b.bit_count = 0;

    psx_sw_u8(&b.w, 0x78);
    psx_sw_u8(&b.w, 0x01);

    png_deflate(&b, raw, raw_size);

    png_put_be32(&b.w, png_adler32(raw, raw_size));

    free(raw);

    if (b.w.error) {
        psx_sw_free(&b.w);
        return -1;
    }

    idat = b.w;

    psx_sw_init(&b.w);

    psx_sw_bytes(&b.w, "\x89PNG\r\n\x1a\n", 8);

    header[0] = (uint8_t)(((uint32_t)width >> 24) & 0xffu);
    header[1] = (uint8_t)(((uint32_t)width >> 16) & 0xffu);
    header[2] = (uint8_t)(((uint32_t)width >> 8) & 0xffu);
    header[3] = (uint8_t)((uint32_t)width & 0xffu);
    header[4] = (uint8_t)(((uint32_t)height >> 24) & 0xffu);
    header[5] = (uint8_t)(((uint32_t)height >> 16) & 0xffu);
    header[6] = (uint8_t)(((uint32_t)height >> 8) & 0xffu);
    header[7] = (uint8_t)((uint32_t)height & 0xffu);
    header[8] = 8;  /* bit depth */
    header[9] = 2;  /* colour type: truecolour RGB */
    header[10] = 0; /* compression: deflate */
    header[11] = 0; /* filter method: adaptive */
    header[12] = 0; /* interlace: none */

    png_put_chunk(&b.w, "IHDR", header, sizeof(header));
    png_put_chunk(&b.w, "IDAT", idat.buf, idat.size);
    png_put_chunk(&b.w, "IEND", NULL, 0);

    psx_sw_free(&idat);

    if (b.w.error) {
        psx_sw_free(&b.w);
        return -1;
    }

    *out_data = b.w.buf;
    *out_size = b.w.size;

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Frame capture                                                              */
/* -------------------------------------------------------------------------- */

static uint8_t thumb_expand5(uint32_t component) {
    return (uint8_t)((component << 3) | (component >> 2));
}

int psx_thumbnail_capture_png(psx_t* psx, void** out_data, size_t* out_size,
                              int* out_width, int* out_height) {
    const uint8_t* source;
    uint8_t* rgb;
    uint32_t stride = PSX_GPU_FB_STRIDE;
    int scale = 1;
    int is_24bpp;
    int use_vram;
    int width;
    int height;
    int source_width;
    int source_height;
    int factor = 1;
    int thumb_width;
    int thumb_height;
    int x;
    int y;
    int result;

    if (!out_data || !out_size)
        return -1;

    *out_data = NULL;
    *out_size = 0;

    if (out_width)
        *out_width = 0;

    if (out_height)
        *out_height = 0;

    if (!psx || !psx->gpu || !psx->gpu->vram)
        return -1;

    width = (int)psx_get_display_width(psx);
    height = (int)psx_get_display_height(psx);

    if (width <= 0 || height <= 0)
        return -1;

    /* Mirrors frontend/main.cpp updateTexture(): 24bpp reinterprets VRAM bytes
       as packed RGB888, and a display window that reaches past the bottom of
       VRAM is read from the VRAM origin instead. Both force the native surface. */
    is_24bpp = psx_get_display_format(psx) ? 1 : 0;
    use_vram = ((int)psx->gpu->disp_y + height) > PSX_GPU_FB_HEIGHT;

    source = NULL;

#ifdef USE_HARDWARE
    if (!is_24bpp && !use_vram) {
        uint32_t backend_stride = PSX_GPU_FB_STRIDE;
        const void* surface = psx_gpu_get_display_surface(psx->gpu, 0, &scale, &backend_stride);

        /* scale == 1 means it handed back the native buffer, which the branch
           below reaches anyway. */
        if (surface && scale > 1 && backend_stride) {
            source = (const uint8_t*)surface;
            stride = backend_stride;
        } else {
            scale = 1;
        }
    }
#endif

    if (!source) {
        scale = 1;
        stride = PSX_GPU_FB_STRIDE;
        source = (const uint8_t*)(use_vram ? (void*)psx->gpu->vram : psx_gpu_get_display_buffer(psx->gpu));
    }

    if (!source)
        return -1;

    source_width = width * scale;
    source_height = height * scale;

    while ((source_width / factor) > PSX_THUMBNAIL_MAX_W ||
           (source_height / factor) > PSX_THUMBNAIL_MAX_H)
        factor++;

    thumb_width = source_width / factor;
    thumb_height = source_height / factor;

    if (thumb_width < 1 || thumb_height < 1)
        return -1;

    rgb = (uint8_t*)malloc((size_t)thumb_width * (size_t)thumb_height * 3u);

    if (!rgb)
        return -1;

    /* Box-average each factor x factor source block. At factor 1 this is a
       straight format conversion, which is the common (native-resolution) case. */
    for (y = 0; y < thumb_height; y++) {
        for (x = 0; x < thumb_width; x++) {
            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            uint32_t count = 0;
            uint8_t* out = rgb + (((size_t)y * (size_t)thumb_width + (size_t)x) * 3u);
            int sy;

            for (sy = y * factor; sy < (y + 1) * factor && sy < source_height; sy++) {
                const uint8_t* row = source + ((size_t)sy * (size_t)stride);
                int sx;

                for (sx = x * factor; sx < (x + 1) * factor && sx < source_width; sx++) {
                    if (is_24bpp) {
                        const uint8_t* texel = row + ((size_t)sx * 3u);

                        red += texel[0];
                        green += texel[1];
                        blue += texel[2];
                    } else {
                        uint16_t texel = (uint16_t)(row[(size_t)sx * 2u] |
                                                    ((uint16_t)row[((size_t)sx * 2u) + 1u] << 8));

                        red += thumb_expand5((uint32_t)(texel & 0x1fu));
                        green += thumb_expand5((uint32_t)((texel >> 5) & 0x1fu));
                        blue += thumb_expand5((uint32_t)((texel >> 10) & 0x1fu));
                    }

                    count++;
                }
            }

            if (!count)
                count = 1;

            out[0] = (uint8_t)(red / count);
            out[1] = (uint8_t)(green / count);
            out[2] = (uint8_t)(blue / count);
        }
    }

    result = psx_png_encode_rgb(rgb, thumb_width, thumb_height, out_data, out_size);

    free(rgb);

    if (result == 0) {
        if (out_width)
            *out_width = thumb_width;

        if (out_height)
            *out_height = thumb_height;
    }

    return result;
}
