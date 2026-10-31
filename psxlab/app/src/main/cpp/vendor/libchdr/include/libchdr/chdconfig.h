#ifndef __CHDCONFIG_H__
#define __CHDCONFIG_H__

/* Configure CHDR features by defining these beforehand. */

#ifndef WANT_RAW_DATA_SECTOR
#define WANT_RAW_DATA_SECTOR    1
#endif

#ifndef WANT_SUBCODE
#define WANT_SUBCODE            1
#endif

#ifndef VERIFY_BLOCK_CRC
#define VERIFY_BLOCK_CRC        1
#endif

#ifndef LOWRAM_TARGET
#define LOWRAM_TARGET              0
#endif

/* 1 = CD codecs keep a private hunk-sized scratch buffer and write the
 * caller's buffer once at the end; 0 = decode into the caller's buffer and
 * spread the sectors out in place, which needs only a frame-sized scratch.
 *
 * The in-place spread cuts peak heap on an open CD image by roughly 40%, and
 * because what it stops allocating is a hunk-sized block per codec it also
 * leaves the largest free block substantially bigger, which is what decides
 * whether a later allocation of that size succeeds. It costs a few percent of
 * decode time.
 *
 * So it is off wherever RAM is not the binding constraint, and on under
 * LOWRAM_TARGET, where it is - the same trade the other LOWRAM_TARGET levers
 * make. Set it explicitly to override either default.
 *
 * Decoded sector data is identical either way. The one observable difference
 * is with WANT_SUBCODE 0, where neither setting writes the 96-byte subcode
 * area of each frame and so leaves it holding unrelated bytes: the caller's
 * previous buffer contents with a private scratch, remnants of the packed
 * decode without one. Those bytes have never been meaningful in that
 * configuration; do not read them.
 *
 * See chd_read() in chd.h for what it asks of the caller's buffer. */
#ifndef CHDR_CD_SCRATCH_BUFFER
#define CHDR_CD_SCRATCH_BUFFER  (!LOWRAM_TARGET)
#endif

/* Trade CPU for RAM across several independent levers, for targets with a
 * few hundred KB of RAM rather than gigabytes:
 *
 * 1. Per-hunk map: instead of fully materializing it at chd_open()
 *    (12 bytes/hunk for CHDv5, ~24 bytes/hunk for legacy v1-v4 - scales
 *    with total hunk count, independent of codec/hunkbytes choice, and
 *    can reach multiple MB on full-size CD/GD-ROM/UMD images), keep only
 *    a sparse checkpoint index and re-derive individual entries on
 *    demand. See LOWRAM_TARGET_CHECKPOINT_STRIDE to tune the RAM/CPU
 *    tradeoff.
 * 2. Each huffman_decoder's lookup table: a two-level table instead of
 *    a full 2^maxbits direct table - see LOWRAM_TARGET_HUFFMAN_L1BITS
 *    below.
 * 3. The compressed-hunk scratch buffer (chd->compressed,
 *    src/libchdr_chd.c): grown on demand to the largest hunk actually
 *    read instead of preallocated to header.hunkbytes at chd_open().
 */

#ifndef LOWRAM_TARGET_CHECKPOINT_STRIDE
#define LOWRAM_TARGET_CHECKPOINT_STRIDE 2048
#endif

/* Under LOWRAM_TARGET, also replace each huffman_decoder's full 2^maxbits
 * direct-lookup table (e.g. 128 KiB at maxbits=16, as used by AVHuff's
 * Y/Cb/Cr contexts and the CHD huffman codec) with a two-level table: a
 * 2^L1BITS first-level table, escaping to small per-prefix subtables only
 * for the rare codes longer than L1BITS. Huffman assigns short codes to
 * common symbols by construction, so the fast (non-escaping) path is
 * unchanged; only long, rare codes pay an extra indirection. Total RAM
 * drops to a few KiB per decoder regardless of maxbits.
 */
#ifndef LOWRAM_TARGET_HUFFMAN_L1BITS
#define LOWRAM_TARGET_HUFFMAN_L1BITS 10
#endif

#endif
