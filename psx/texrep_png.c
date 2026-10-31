#include "texrep_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- checksums ------------------------------------------------------------------------ */

static uint32_t png_crc_table[256];
static int      png_crc_ready = 0;

static void png_crc_init(void) {
    uint32_t n, c, k;

    for (n = 0; n < 256; n++) {
        c = n;

        for (k = 0; k < 8; k++)
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);

        png_crc_table[n] = c;
    }

    png_crc_ready = 1;
}

static uint32_t png_crc(const uint8_t* p, size_t n, uint32_t c) {
    size_t i;

    if (!png_crc_ready)
        png_crc_init();

    for (i = 0; i < n; i++)
        c = png_crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);

    return c;
}

static uint32_t png_adler(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    return (b << 16) | a;
}

/* ---- inflate ---------------------------------------------------------------------------

   Canonical-Huffman decode straight off the DEFLATE spec (RFC 1951): counts per code length
   plus symbols in code order, walked one bit at a time. Slower per symbol than a lookup
   table and perhaps a hundred lines shorter; a texture is decoded once, when it is first
   sampled, and never again for the rest of the session. */

typedef struct {
    const uint8_t* src;
    size_t         src_len;
    size_t         src_pos;
    uint32_t       bitbuf;
    int            bitcnt;

    uint8_t*       dst;
    size_t         dst_cap;
    size_t         dst_len;

    int            err;
} inf_t;

typedef struct {
    short count[16];
    short symbol[288];
} inf_huff_t;

static int inf_bits(inf_t* s, int need) {
    int val;

    while (s->bitcnt < need) {
        if (s->src_pos >= s->src_len) {
            s->err = 1;

            return 0;
        }

        s->bitbuf |= (uint32_t)s->src[s->src_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }

    val = (int)(s->bitbuf & ((1u << need) - 1u));
    s->bitbuf >>= need;
    s->bitcnt -= need;

    return val;
}

static int inf_build(inf_huff_t* h, const uint8_t* len, int n) {
    int i, left;
    int offs[16];

    for (i = 0; i < 16; i++)
        h->count[i] = 0;

    for (i = 0; i < n; i++)
        h->count[len[i]]++;

    if (h->count[0] == n)
        return 0;

    /* Kraft: every length level doubles the code space and consumes one slot per code. A
       negative remainder is an over-subscribed table, which is a corrupt stream. */
    left = 1;

    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];

        if (left < 0)
            return -1;
    }

    offs[1] = 0;

    for (i = 1; i < 15; i++)
        offs[i + 1] = offs[i] + h->count[i];

    for (i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (short)i;

    return left;
}

static int inf_decode(inf_t* s, const inf_huff_t* h) {
    int len, code = 0, first = 0, index = 0;

    for (len = 1; len <= 15; len++) {
        code |= inf_bits(s, 1);

        if (s->err)
            return -1;

        {
            int count = h->count[len];

            if ((code - count) < first)
                return h->symbol[index + (code - first)];

            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }

    s->err = 1;

    return -1;
}

static int inf_put(inf_t* s, uint8_t b) {
    if (s->dst_len >= s->dst_cap) {
        /* The caller sizes dst from IHDR, so overrunning it means the stream claims more
           pixels than the header does. Stop rather than grow: this is untrusted input. */
        s->err = 1;

        return 0;
    }

    s->dst[s->dst_len++] = b;

    return 1;
}

static const short inf_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const short inf_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const short inf_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const short inf_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void inf_block(inf_t* s, const inf_huff_t* lc, const inf_huff_t* dc) {
    for (;;) {
        int sym = inf_decode(s, lc);

        if (s->err || sym < 0)
            return;

        if (sym < 256) {
            if (!inf_put(s, (uint8_t)sym))
                return;

            continue;
        }

        if (sym == 256)
            return;

        sym -= 257;

        if (sym >= 29) {
            s->err = 1;

            return;
        }

        {
            int len = inf_len_base[sym] + inf_bits(s, inf_len_extra[sym]);
            int dsym = inf_decode(s, dc);
            int dist;
            int i;

            if (s->err || dsym < 0 || dsym >= 30) {
                s->err = 1;

                return;
            }

            dist = inf_dist_base[dsym] + inf_bits(s, inf_dist_extra[dsym]);

            if (s->err)
                return;

            if ((size_t)dist > s->dst_len) {
                s->err = 1;

                return;
            }

            for (i = 0; i < len; i++)
                if (!inf_put(s, s->dst[s->dst_len - (size_t)dist]))
                    return;
        }
    }
}

static void inf_fixed(inf_t* s) {
    static inf_huff_t lc, dc;
    static int built = 0;

    if (!built) {
        uint8_t len[288];
        int i;

        for (i = 0; i < 144; i++) len[i] = 8;
        for (; i < 256; i++)      len[i] = 9;
        for (; i < 280; i++)      len[i] = 7;
        for (; i < 288; i++)      len[i] = 8;

        inf_build(&lc, len, 288);

        for (i = 0; i < 30; i++) len[i] = 5;

        inf_build(&dc, len, 30);
        built = 1;
    }

    inf_block(s, &lc, &dc);
}

static void inf_dynamic(inf_t* s) {
    static const uint8_t ord[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    int nlen = inf_bits(s, 5) + 257;
    int ndist = inf_bits(s, 5) + 1;
    int ncode = inf_bits(s, 4) + 4;
    uint8_t lengths[320];
    inf_huff_t lc, dc;
    int i;

    if (s->err || nlen > 286 || ndist > 30) {
        s->err = 1;

        return;
    }

    memset(lengths, 0, sizeof(lengths));

    for (i = 0; i < ncode; i++)
        lengths[ord[i]] = (uint8_t)inf_bits(s, 3);

    if (s->err)
        return;

    if (inf_build(&lc, lengths, 19) != 0) {
        s->err = 1;

        return;
    }

    i = 0;

    while (i < (nlen + ndist)) {
        int sym = inf_decode(s, &lc);
        int len = 0, rep = 0;

        if (s->err || sym < 0)
            return;

        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;

            continue;
        }

        if (sym == 16) {
            if (i == 0) {
                s->err = 1;

                return;
            }

            len = lengths[i - 1];
            rep = 3 + inf_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + inf_bits(s, 3);
        } else {
            rep = 11 + inf_bits(s, 7);
        }

        if (s->err || (i + rep) > (nlen + ndist)) {
            s->err = 1;

            return;
        }

        while (rep--)
            lengths[i++] = (uint8_t)len;
    }

    if (inf_build(&lc, lengths, nlen) < 0) {
        s->err = 1;

        return;
    }

    if (inf_build(&dc, lengths + nlen, ndist) < 0) {
        s->err = 1;

        return;
    }

    inf_block(s, &lc, &dc);
}

/* Inflates a raw DEFLATE stream into a caller-sized buffer. Returns bytes produced, or 0. */
static size_t inf_run(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap) {
    inf_t s;
    int last;

    memset(&s, 0, sizeof(s));
    s.src = src;
    s.src_len = src_len;
    s.dst = dst;
    s.dst_cap = dst_cap;

    do {
        int type;

        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);

        if (s.err)
            return 0;

        if (type == 0) {
            unsigned len;

            s.bitbuf = 0;
            s.bitcnt = 0;

            if ((s.src_pos + 4u) > s.src_len)
                return 0;

            len = (unsigned)s.src[s.src_pos] | ((unsigned)s.src[s.src_pos + 1] << 8);
            s.src_pos += 4;   /* LEN then its one's complement, which we do not verify */

            if ((s.src_pos + len) > s.src_len || (s.dst_len + len) > s.dst_cap)
                return 0;

            memcpy(s.dst + s.dst_len, s.src + s.src_pos, len);
            s.dst_len += len;
            s.src_pos += len;
        } else if (type == 1) {
            inf_fixed(&s);
        } else if (type == 2) {
            inf_dynamic(&s);
        } else {
            return 0;
        }

        if (s.err)
            return 0;
    } while (!last);

    return s.dst_len;
}

/* ---- PNG decode ------------------------------------------------------------------------ */

static uint32_t png_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
        return a;

    return (pb <= pc) ? b : c;
}

/* Expands the low `depth` bits at index `i` of a packed row. depth is 1, 2 or 4. */
static unsigned png_bits_at(const uint8_t* row, int i, int depth) {
    int per = 8 / depth;
    int byte = i / per;
    int shift = 8 - depth * ((i % per) + 1);

    return (unsigned)((row[byte] >> shift) & ((1u << depth) - 1u));
}

uint8_t* psx_png_decode(const uint8_t* data, size_t len, int* out_w, int* out_h,
                        const char** why) {
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    size_t pos = 8;
    int w = 0, h = 0, depth = 0, colour = 0, interlace = 0;
    int channels = 0, have_ihdr = 0;
    uint8_t pal[256 * 3];
    uint8_t pal_a[256];
    int pal_n = 0;
    int have_trns = 0;
    uint8_t trns_key[6];
    int trns_key_n = 0;

    uint8_t* idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    uint8_t* raw = NULL;
    uint8_t* out = NULL;
    size_t raw_cap = 0, raw_len = 0;
    size_t stride = 0;
    int bpp = 0;   /* bytes per pixel, rounded up, for the filter's `a`/`c` taps */
    int y;

    if (why)
        *why = "ok";

    if (!data || len < 8 || memcmp(data, sig, 8) != 0) {
        if (why) *why = "not a PNG";

        return NULL;
    }

    memset(pal, 0, sizeof(pal));
    memset(pal_a, 255, sizeof(pal_a));
    memset(trns_key, 0, sizeof(trns_key));

    while ((pos + 8u) <= len) {
        uint32_t clen = png_be32(data + pos);
        const uint8_t* type = data + pos + 4;
        const uint8_t* body = data + pos + 8;

        if (clen > (len - pos - 8u))
            break;

        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = (int)png_be32(body);
            h = (int)png_be32(body + 4);
            depth = body[8];
            colour = body[9];
            interlace = body[12];
            have_ihdr = 1;

            if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
                if (why) *why = "implausible dimensions";

                return NULL;
            }

            if (interlace) {
                if (why) *why = "interlaced (Adam7); re-save without interlacing";

                return NULL;
            }

            switch (colour) {
                case 0: channels = 1; break;
                case 2: channels = 3; break;
                case 3: channels = 1; break;
                case 4: channels = 2; break;
                case 6: channels = 4; break;
                default:
                    if (why) *why = "unknown colour type";

                    return NULL;
            }

            if (colour == 3) {
                if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
                    if (why) *why = "indexed bit depth must be 1, 2, 4 or 8";

                    return NULL;
                }
            } else if (depth != 8) {
                if (why) *why = "bit depth must be 8; re-save as 8 bits per channel";

                return NULL;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            pal_n = (int)(clen / 3u);

            if (pal_n > 256)
                pal_n = 256;

            memcpy(pal, body, (size_t)pal_n * 3u);
        } else if (!memcmp(type, "tRNS", 4)) {
            have_trns = 1;

            if (colour == 3) {
                size_t n = clen > 256u ? 256u : clen;

                memcpy(pal_a, body, n);
            } else if (clen <= sizeof(trns_key)) {
                memcpy(trns_key, body, clen);
                trns_key_n = (int)clen;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if ((idat_len + clen) > idat_cap) {
                size_t want = (idat_len + clen) * 2u + 1024u;
                uint8_t* grow = (uint8_t*)realloc(idat, want);

                if (!grow) {
                    free(idat);

                    if (why) *why = "out of memory";

                    return NULL;
                }

                idat = grow;
                idat_cap = want;
            }

            memcpy(idat + idat_len, body, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }

        pos += 12u + clen;   /* length + type + body + crc */
    }

    if (!have_ihdr || !idat || idat_len < 3u) {
        free(idat);

        if (why) *why = "truncated or missing image data";

        return NULL;
    }

    if (colour == 3 && pal_n == 0) {
        free(idat);

        if (why) *why = "indexed image with no palette";

        return NULL;
    }

    /* One filter byte per row, then the packed samples. */
    stride = ((size_t)w * (size_t)channels * (size_t)depth + 7u) / 8u;
    raw_cap = (stride + 1u) * (size_t)h;
    raw = (uint8_t*)malloc(raw_cap ? raw_cap : 1u);

    if (!raw) {
        free(idat);

        if (why) *why = "out of memory";

        return NULL;
    }

    /* Skip the 2-byte zlib header; the trailing adler32 is simply never reached. */
    raw_len = inf_run(idat + 2, idat_len - 2u, raw, raw_cap);
    free(idat);

    if (raw_len != raw_cap) {
        free(raw);

        if (why) *why = "compressed data did not expand to the declared size";

        return NULL;
    }

    bpp = (channels * depth + 7) / 8;

    if (bpp < 1)
        bpp = 1;

    /* Unfilter in place, row by row, per RFC 2083 section 6. */
    for (y = 0; y < h; y++) {
        uint8_t* row = raw + (size_t)y * (stride + 1u) + 1u;
        const uint8_t* prev = (y > 0) ? (raw + (size_t)(y - 1) * (stride + 1u) + 1u) : NULL;
        int filter = raw[(size_t)y * (stride + 1u)];
        size_t i;

        for (i = 0; i < stride; i++) {
            int a = (i >= (size_t)bpp) ? row[i - (size_t)bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)bpp) ? prev[i - (size_t)bpp] : 0;
            int v = row[i];

            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += png_paeth(a, b, c); break;
                default:
                    free(raw);

                    if (why) *why = "unknown row filter";

                    return NULL;
            }

            row[i] = (uint8_t)v;
        }
    }

    out = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);

    if (!out) {
        free(raw);

        if (why) *why = "out of memory";

        return NULL;
    }

    for (y = 0; y < h; y++) {
        const uint8_t* row = raw + (size_t)y * (stride + 1u) + 1u;
        uint8_t* dst = out + (size_t)y * (size_t)w * 4u;
        int x;

        for (x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;

            if (colour == 3) {
                unsigned idx = (depth == 8) ? row[x] : png_bits_at(row, x, depth);

                if ((int)idx >= pal_n)
                    idx = 0;

                r = pal[idx * 3 + 0];
                g = pal[idx * 3 + 1];
                b = pal[idx * 3 + 2];
                a = pal_a[idx];
            } else if (colour == 0) {
                r = g = b = row[x];

                if (have_trns && trns_key_n >= 2 && row[x] == trns_key[1])
                    a = 0;
            } else if (colour == 4) {
                r = g = b = row[x * 2 + 0];
                a = row[x * 2 + 1];
            } else if (colour == 2) {
                r = row[x * 3 + 0];
                g = row[x * 3 + 1];
                b = row[x * 3 + 2];

                if (have_trns && trns_key_n >= 6 &&
                    r == trns_key[1] && g == trns_key[3] && b == trns_key[5])
                    a = 0;
            } else {
                r = row[x * 4 + 0];
                g = row[x * 4 + 1];
                b = row[x * 4 + 2];
                a = row[x * 4 + 3];
            }

            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }

    free(raw);

    *out_w = w;
    *out_h = h;

    return out;
}

/* ---- PNG encode ------------------------------------------------------------------------ */

static int png_chunk(FILE* f, const char* type, const uint8_t* body, size_t n) {
    uint8_t hdr[8];
    uint8_t crcb[4];
    uint32_t crc;

    hdr[0] = (uint8_t)(n >> 24);
    hdr[1] = (uint8_t)(n >> 16);
    hdr[2] = (uint8_t)(n >> 8);
    hdr[3] = (uint8_t)n;
    hdr[4] = (uint8_t)type[0];
    hdr[5] = (uint8_t)type[1];
    hdr[6] = (uint8_t)type[2];
    hdr[7] = (uint8_t)type[3];

    crc = png_crc(hdr + 4, 4, 0xffffffffu);

    if (n)
        crc = png_crc(body, n, crc);

    crc ^= 0xffffffffu;

    crcb[0] = (uint8_t)(crc >> 24);
    crcb[1] = (uint8_t)(crc >> 16);
    crcb[2] = (uint8_t)(crc >> 8);
    crcb[3] = (uint8_t)crc;

    if (fwrite(hdr, 1, 8, f) != 8)
        return 0;

    if (n && fwrite(body, 1, n, f) != n)
        return 0;

    return fwrite(crcb, 1, 4, f) == 4;
}

int psx_png_write(const char* path, const uint8_t* rgba, int w, int h) {
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    FILE* f;
    uint8_t ihdr[13];
    uint8_t* lines = NULL;
    uint8_t* zs = NULL;
    size_t lines_len, zs_len, zs_pos, i;
    uint32_t adler;
    int ok = 0;
    int y;

    if (!path || !rgba || w <= 0 || h <= 0)
        return 0;

    lines_len = ((size_t)w * 4u + 1u) * (size_t)h;
    lines = (uint8_t*)malloc(lines_len);

    if (!lines)
        return 0;

    for (y = 0; y < h; y++) {
        uint8_t* row = lines + (size_t)y * ((size_t)w * 4u + 1u);

        row[0] = 0;   /* filter "none": the encoder does not compress, so filtering earns
                         nothing and only costs the decoder work */
        memcpy(row + 1, rgba + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
    }

    adler = png_adler(lines, lines_len);

    /* zlib header, then stored deflate blocks of at most 65535 bytes, then adler32. */
    {
        size_t blocks = (lines_len + 65534u) / 65535u;

        if (blocks == 0)
            blocks = 1;

        zs_len = 2u + blocks * 5u + lines_len + 4u;
        zs = (uint8_t*)malloc(zs_len);

        if (!zs) {
            free(lines);

            return 0;
        }

        zs[0] = 0x78;   /* deflate, 32K window */
        zs[1] = 0x01;   /* no dictionary, fastest; (0x7801) % 31 == 0 as zlib requires */
        zs_pos = 2;

        i = 0;

        while (i < lines_len || i == 0) {
            size_t chunk = lines_len - i;
            int last;

            if (chunk > 65535u)
                chunk = 65535u;

            last = ((i + chunk) >= lines_len) ? 1 : 0;

            zs[zs_pos++] = (uint8_t)last;
            zs[zs_pos++] = (uint8_t)(chunk & 0xffu);
            zs[zs_pos++] = (uint8_t)((chunk >> 8) & 0xffu);
            zs[zs_pos++] = (uint8_t)(~chunk & 0xffu);
            zs[zs_pos++] = (uint8_t)((~chunk >> 8) & 0xffu);

            if (chunk) {
                memcpy(zs + zs_pos, lines + i, chunk);
                zs_pos += chunk;
            }

            i += chunk;

            if (last)
                break;
        }

        zs[zs_pos++] = (uint8_t)(adler >> 24);
        zs[zs_pos++] = (uint8_t)(adler >> 16);
        zs[zs_pos++] = (uint8_t)(adler >> 8);
        zs[zs_pos++] = (uint8_t)adler;
    }

    f = fopen(path, "wb");

    if (!f) {
        free(lines);
        free(zs);

        return 0;
    }

    ihdr[0] = (uint8_t)((unsigned)w >> 24);
    ihdr[1] = (uint8_t)((unsigned)w >> 16);
    ihdr[2] = (uint8_t)((unsigned)w >> 8);
    ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)((unsigned)h >> 24);
    ihdr[5] = (uint8_t)((unsigned)h >> 16);
    ihdr[6] = (uint8_t)((unsigned)h >> 8);
    ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;    /* bit depth   */
    ihdr[9] = 6;    /* colour type: truecolour with alpha */
    ihdr[10] = 0;   /* deflate     */
    ihdr[11] = 0;   /* adaptive filtering */
    ihdr[12] = 0;   /* no interlace */

    if (fwrite(sig, 1, 8, f) == 8 &&
        png_chunk(f, "IHDR", ihdr, sizeof(ihdr)) &&
        png_chunk(f, "IDAT", zs, zs_pos) &&
        png_chunk(f, "IEND", NULL, 0))
        ok = 1;

    if (fclose(f) != 0)
        ok = 0;

    free(lines);
    free(zs);

    return ok;
}
