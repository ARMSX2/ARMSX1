#ifndef PSX_TEXREP_PNG_H
#define PSX_TEXREP_PNG_H

/*
    The smallest PNG codec that lets a pack author use an ordinary image editor.

    Why a codec at all: PNG is the only lossless, alpha-carrying format every editor writes
    without being told to, and a texture pack that cannot be opened in GIMP is not a texture
    pack. Why OUR codec: psx/ links nothing (see the layering contract in
    psx/dev/gpu_backend.h) and is built for Android, Windows, macOS, Linux, the Vita and
    wasm; taking a dependency on zlib for a feature that is off by default is not a trade
    worth making. libchdr vendors miniz, but only when USE_CHD=1 and only as a CMake static
    library the core does not otherwise touch.

    Not psx/thumbnail.c's psx_png_encode_rgb(): that one is RGB-only, writes to memory, and
    has no decoder. Alpha is not optional here -- it carries the PlayStation's transparent
    texel and its semi-transparency bit (psx/texrep.h) -- and reading what a pack author saved
    is the entire point of the feature, so a decoder was needed whatever the encoder did.

    ENCODE writes 8-bit RGBA, non-interlaced, with STORED (uncompressed) deflate blocks. The
    zlib container, adler32 and the per-chunk crc32 are all real, so the file is a valid PNG
    that any tool opens; it is simply bigger than a compressed one. A dumped PS1 texture is at
    most 256x256, i.e. 256 KB before container overhead, and dumping is a debugging/authoring
    activity that runs once per distinct texture. Buying a full deflate encoder to make those
    files smaller is not worth the code.

    DECODE is a complete inflate (stored, fixed-Huffman and dynamic-Huffman blocks) plus the
    five PNG row filters, so it reads what editors actually write. Supported: bit depth 8 at
    colour types 0 (grey), 2 (RGB), 3 (indexed), 4 (grey+alpha) and 6 (RGBA), plus indexed at
    1, 2 and 4 bits, with tRNS honoured for types 0, 2 and 3. Interlaced and 16-bit images are
    rejected by name so the author gets told to re-save rather than silently getting nothing.

    Every read is bounds-checked against the input length and every write against the
    allocated output: this parses files a stranger wrote and hands the result to the hottest
    loop in the emulator.
*/

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes `len` bytes of PNG into a freshly malloc'd 8-bit RGBA buffer of *w * *h * 4 bytes,
   which the caller frees. Returns NULL on any failure and, when `why` is non-NULL, points it
   at a static string naming the reason -- suitable for a log line that also names the file. */
uint8_t* psx_png_decode(const uint8_t* data, size_t len, int* w, int* h, const char** why);

/* Writes `rgba` (w*h*4) to `path` as 8-bit RGBA PNG. Returns 1 on success. */
int psx_png_write(const char* path, const uint8_t* rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
