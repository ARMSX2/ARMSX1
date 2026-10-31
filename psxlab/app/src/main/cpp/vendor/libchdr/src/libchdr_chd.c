/***************************************************************************

    chd.c

    MAME Compressed Hunks of Data file format

****************************************************************************

    Copyright Aaron Giles
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

        * Redistributions of source code must retain the above copyright
          notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
          notice, this list of conditions and the following disclaimer in
          the documentation and/or other materials provided with the
          distribution.
        * Neither the name 'MAME' nor the names of its contributors may be
          used to endorse or promote products derived from this software
          without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY AARON GILES ''AS IS'' AND ANY EXPRESS OR
    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL AARON GILES BE LIABLE FOR ANY DIRECT,
    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
    IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

***************************************************************************/

/* Feature test macros: must come before any system header.
 * Ensures fseeko/ftello are declared under strict C99/C11 and
 * that 64-bit file offsets are used on 32-bit platforms (armhf,
 * i386, and other time64/LFS rebuilds). */
#if !defined(_WIN32) && !defined(__PS3__) && !defined(__SWITCH__) && !defined(__vita__)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef __ANDROID__
#    ifndef _FILE_OFFSET_BITS
#      define _FILE_OFFSET_BITS 64
#    endif
#  endif
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include "../include/libchdr/chd.h"
#include "../include/libchdr/cdrom.h"
#include "codec_avhuff.h"
#include "codec_cdfl.h"
#include "codec_cdlz.h"
#include "codec_cdzl.h"
#include "codec_cdzs.h"
#include "codec_flac.h"
#include "codec_huff.h"
#include "codec_lzma.h"
#include "codec_zlib.h"
#include "codec_zstd.h"
#include "../include/libchdr/huffman.h"
#include "../include/libchdr/macros.h"

#include "../deps/lzma-26.02/include/LzmaDec.h"

#undef TRUE
#undef FALSE
#define TRUE 1
#define FALSE 0

#define SHA1_DIGEST_SIZE 20

/***************************************************************************
    CONSTANTS
***************************************************************************/

#define MAP_STACK_ENTRIES			512			/* max number of entries to use on the stack */
#define MAP_ENTRY_SIZE				16			/* V3 and later */
#define OLD_MAP_ENTRY_SIZE			8			/* V1-V2 */
#define METADATA_HEADER_SIZE		16			/* metadata header size */

#define MAP_ENTRY_FLAG_TYPE_MASK	0x0f		/* what type of hunk */
#define MAP_ENTRY_FLAG_NO_CRC		0x10		/* no CRC is present */

#define CHD_V1_SECTOR_SIZE			512			/* size of a "sector" in the V1 header */

#define CHD_MAX_HUNK_SIZE				(128 * 1024 * 1024) /* hunk size probably shouldn't be more than 128MB */

/* Sanity cap on logical (decompressed) CHD size. Large enough to cover
 * any realistic disc image: BD50 PS3 ISOs (50 GB), arcade laserdisc
 * CHDs, and future-proofing for larger optical formats. 1 TB is well
 * under any integer-overflow threshold, and actual allocation safety
 * is further bounded by CHD_MAX_HUNK_SIZE and the totalhunks check in
 * header_read. Previously 10 GB, which rejected legitimate PS3 ISOs
 * (#147). */
#define CHD_MAX_FILE_SIZE				(1024ULL * 1024 * 1024 * 1024)

/* Ceiling on a materialized CHDv5 hunk map. Not a format limit - it only
 * bounds one allocation. LOWRAM_TARGET never materializes the map at all. */
#define CHD_MAX_MAP_SIZE				(256ULL * 1024 * 1024)

#define COOKIE_VALUE				0xbaadf00d

#define END_OF_LIST_COOKIE			"EndOfListCookie"

#define NO_MATCH					(~0)

/* V3-V4 entry types */
enum
{
	V34_MAP_ENTRY_TYPE_INVALID = 0,             /* invalid type */
	V34_MAP_ENTRY_TYPE_COMPRESSED = 1,          /* standard compression */
	V34_MAP_ENTRY_TYPE_UNCOMPRESSED = 2,        /* uncompressed data */
	V34_MAP_ENTRY_TYPE_MINI = 3,                /* mini: use offset as raw data */
	V34_MAP_ENTRY_TYPE_SELF_HUNK = 4,           /* same as another hunk in this file */
	V34_MAP_ENTRY_TYPE_PARENT_HUNK = 5,         /* same as a hunk in the parent file */
	V34_MAP_ENTRY_TYPE_2ND_COMPRESSED = 6       /* compressed with secondary algorithm (usually FLAC CDDA) */
};

/* V5 compression types */
enum
{
	/* codec #0
	 * these types are live when running */
	COMPRESSION_TYPE_0 = 0,
	/* codec #1 */
	COMPRESSION_TYPE_1 = 1,
	/* codec #2 */
	COMPRESSION_TYPE_2 = 2,
	/* codec #3 */
	COMPRESSION_TYPE_3 = 3,
	/* no compression; implicit length = hunkbytes */
	COMPRESSION_NONE = 4,
	/* same as another block in this chd */
	COMPRESSION_SELF = 5,
	/* same as a hunk's worth of units in the parent chd */
	COMPRESSION_PARENT = 6,

	/* start of small RLE run (4-bit length)
	 * these additional pseudo-types are used for compressed encodings: */
	COMPRESSION_RLE_SMALL,
	/* start of large RLE run (8-bit length) */
	COMPRESSION_RLE_LARGE,
	/* same as the last COMPRESSION_SELF block */
	COMPRESSION_SELF_0,
	/* same as the last COMPRESSION_SELF block + 1 */
	COMPRESSION_SELF_1,
	/* same block in the parent */
	COMPRESSION_PARENT_SELF,
	/* same as the last COMPRESSION_PARENT block */
	COMPRESSION_PARENT_0,
	/* same as the last COMPRESSION_PARENT block + 1 */
	COMPRESSION_PARENT_1
};

/***************************************************************************
    MACROS
***************************************************************************/

#define EARLY_EXIT(x)				do { (void)(x); goto cleanup; } while (0)

/***************************************************************************
    TYPE DEFINITIONS
***************************************************************************/

/* interface to a codec */
typedef struct _codec_interface codec_interface;
struct _codec_interface
{
	uint32_t		compression;								/* type of compression */
	const char *compname;									/* name of the algorithm */
	uint8_t		lossy;										/* is this a lossy algorithm? */
	chd_error	(*init)(void *codec, uint32_t hunkbytes);		/* codec initialize */
	void		(*free)(void *codec);						/* codec free */
	chd_codec_interface_decompress	decompress; /* decompress data */
	chd_error	(*config)(void *codec, int param, void *config); /* configure */
};

/* a single map entry */
typedef struct _map_entry map_entry;
struct _map_entry
{
	uint64_t					offset;			/* offset within the file of the data */
	uint32_t					crc;			/* 32-bit CRC of the data */
	uint32_t					length;			/* length of the data */
	uint8_t					flags;			/* misc flags */
};

/* a single metadata entry */
typedef struct _metadata_entry metadata_entry;
struct _metadata_entry
{
	uint64_t					offset;			/* offset within the file of the header */
	uint64_t					next;			/* offset within the file of the next header */
	uint64_t					prev;			/* offset within the file of the previous header */
	uint32_t					length;			/* length of the metadata */
	uint32_t					metatag;		/* metadata tag */
	uint8_t					flags;			/* flag bits */
};

#if LOWRAM_TARGET
/* one v5-map checkpoint: everything needed to resume both decode passes
 * (pass 1: per-hunk compression-type byte, Huffman+RLE; pass 2: per-hunk
 * length/offset/crc, fixed-width fields) at hunk `hunknum` without having
 * decoded any of hunks [0, hunknum). See LOWRAM_TARGET in chdconfig.h. */
typedef struct _v5_map_checkpoint v5_map_checkpoint;
struct _v5_map_checkpoint
{
	uint32_t	hunknum;
	uint64_t	pass1_bitpos;	/* bit position to resume compression-type decode */
	uint8_t		lastcomp;		/* pass 1 RLE state */
	int32_t		repcount;		/* pass 1 RLE state */
	uint64_t	pass2_bitpos;	/* bit position to resume length/offset/crc decode */
	uint64_t	curoffset;		/* pass 2 cumulative offset state */
	uint32_t	last_self;		/* pass 2 COMPRESSION_SELF_0/1 state */
	uint64_t	last_parent;	/* pass 2 COMPRESSION_PARENT_0/1 state */
};

/* one cached compressed-map window: the exact byte range covering a single
 * checkpoint bucket in one of the two decode passes, fetched from the CHD
 * file on demand and kept only until a lookup needs a different bucket.
 * Byte range is computed exactly from adjacent checkpoints' bit positions -
 * no fixed/guessed buffer size, no risk of an undersized window. */
typedef struct _v5_map_window v5_map_window;
struct _v5_map_window
{
	uint8_t *	data;				/* malloc'd, exactly byte_len long; NULL if nothing cached */
	uint32_t	byte_start;			/* offset within the compressed blob (relative to mapoffset+16) */
	uint32_t	byte_len;
	uint32_t	checkpoint_idx;		/* which checkpoint this window covers; UINT32_MAX if none cached */
};

/* v5_map_get_entry() normally resumes from the nearest checkpoint on every
 * call - O(stride/2) huffman+RLE work per lookup on average. For sequential
 * access (the common case: playback reading hunk N, N+1, N+2, ...) that's
 * wasted work, since the previous call already decoded up to hunknum-1 and
 * left bs1/bs2 sitting exactly where hunknum's own decode needs to start.
 * Caching that in-flight state turns the sequential case into O(1) per
 * lookup instead. bs1/bs2's `read` pointers alias pass1_window/pass2_window
 * - safe to cache as long as nothing reallocates those buffers in between,
 * which holds precisely because this fast path never calls
 * ensure_v5_map_window() itself (see v5_map_get_entry()). Fixed-size, no
 * growth - a few dozen bytes, independent of file size or hunk count.
 *
 * `next_boundary` exists because pass1_window/pass2_window are each sized
 * to exactly one checkpoint bucket's byte range - continuing to read past
 * that range without refetching would silently read past bitstream->dlength
 * (bitstream_peek's bounds check just stops supplying real bytes at that
 * point, it doesn't error), corrupting the decode right at every bucket
 * crossing. Forcing hunknum == next_boundary back onto the slow path -
 * which correctly refetches both windows for the new bucket - is what
 * keeps this fast path safe. */
typedef struct _v5_resume_cache v5_resume_cache;
struct _v5_resume_cache
{
	uint8_t				valid;			/* 0 until the first successful decode populates this */
	uint32_t			cur_hunk;		/* next hunknum this state can resume into */
	uint32_t			next_boundary;	/* first hunknum outside the current checkpoint bucket */
	struct bitstream	bs1, bs2;
	uint8_t				lastcomp;
	int32_t				repcount;
	uint64_t			curoffset;
	uint32_t			last_self;
	uint64_t			last_parent;
};

/* resident state for lazily re-deriving v5 map entries. Replaces the fully
 * materialized `header->rawmap` (totalhunks*12 bytes) with a sparse
 * checkpoint table plus, per lookup, a small on-demand window of the
 * compressed on-disk map re-read from the file - not the whole blob (which
 * itself is only ~20-30% of the fully materialized size, but still large:
 * hundreds of KB on real GD-ROM/UMD discs). See v5_map_get_entry(). */
typedef struct _v5_lowram_map v5_lowram_map;
struct _v5_lowram_map
{
	uint32_t				mapbytes;
	uint64_t				firstoffs;
	uint8_t					lengthbits, selfbits, parentbits;
	struct huffman_decoder *decoder;		/* compression-type huffman tree, built once */
	v5_map_checkpoint *		checkpoints;
	uint32_t				checkpoint_count;
	uint64_t				pass1_end_bitpos;	/* where pass 1 ends / pass 2 begins in the blob -
							 * a single fixed point for the whole file, not per-checkpoint */
	v5_map_window			pass1_window;
	v5_map_window			pass2_window;
	v5_resume_cache			resume;			/* sequential-access fast path, see v5_resume_cache */
};
#endif

/* internal representation of an open CHD file */
struct _chd_file
{
	uint32_t					cookie;			/* cookie, should equal COOKIE_VALUE */

	core_file_callbacks_and_argp	file;			/* handle to the open core file */
	uint64_t				file_size;		/* size of the core file */
	chd_header				header;			/* header, extracted from file */

	chd_file *				parent;			/* pointer to parent file, or NULL */

	map_entry *				map;			/* array of map entries */

	uint8_t *					compressed;		/* pointer to buffer for compressed data */
	const codec_interface *	codecintf[4];	/* interface to the codec */

	struct
	{
		zlib_codec_data			zlib;		/* zlib codec data */
		lzma_codec_data			lzma;		/* lzma codec data */
		huff_codec_data			huff;		/* huff codec data */
		flac_codec_data			flac;		/* flac codec data */
		zstd_codec_data			zstd;		/* zstd codec data */
		cdzl_codec_data			cdzl;		/* cdzl codec data */
		cdlz_codec_data			cdlz;		/* cdlz codec data */
		cdfl_codec_data			cdfl;		/* cdfl codec data */
		cdzs_codec_data			cdzs;		/* cdzs codec data */
		avhuff_codec_data		avhuff;		/* avhuff codec data */
	} codec_data;

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
	/* One inflater for every CD codec's subcode stream. cdzl, cdlz and cdfl
	 * each used to allocate their own, but a hunk is decoded by exactly one
	 * CD codec and zlib_codec_decompress() runs tinfl_init() on entry, so
	 * nothing carries from one to the next - three tinfl_decompressor
	 * instances where one does. At CD geometry that is 16752 bytes back on a
	 * file using all three, 9.3% of its peak. cdzs is not here: its subcode
	 * goes through zstd. */
	zlib_codec_data				subcode_shared;
#endif

	uint8_t *					file_cache;		/* cache of underlying file */

	/* Compressed read-ahead. libchdr issues one seek+read per hunk, and
	 * compressed hunks are small - measured across a 14-CHD sample, 426 to
	 * 15223 bytes, averaging a few KB - so a large title costs hundreds of
	 * thousands of transactions whose fixed per-call cost (VFS dispatch,
	 * filesystem bookkeeping, controller command setup, DMA, interrupt) has
	 * nothing to do with their size. On an ESP32-P4 reading from SD that
	 * showed up as under 15% of the available bus bandwidth.
	 *
	 * This is only worth doing because hunk payloads turn out to be laid out
	 * strictly sequentially: across that same sample, 100% of hunks that
	 * touch the file begin exactly where the previous one ended, in a single
	 * run spanning the whole file. So one larger read serves many hunks.
	 *
	 * Off unless the caller sets a budget - see chd_set_cache_budget(). The
	 * library deliberately does not size this itself: how much memory is
	 * available is a property of the embedding system, not of libchdr. */
	uint8_t *					ra_buf;			/* NULL = disabled */
	size_t						ra_capacity;
	uint64_t					ra_off;			/* file offset of ra_buf[0] */
	size_t						ra_valid;		/* bytes currently held */
	uint64_t					ra_hits, ra_misses;

	/* Decoded-hunk cache, used only by COMPRESSION_SELF back-references.
	 * A self-referencing hunk means "identical to hunk N", and the read path
	 * previously recursed into a full re-read *and* re-decode of hunk N every
	 * time. Measured across a 14-CHD corpus: 100% of self-references point
	 * backwards, so on FAT-backed storage each one also forces a filesystem
	 * seek that restarts its cluster-chain walk.
	 *
	 * Sized as a byte budget rather than an entry count on purpose: hunkbytes
	 * varies 8x across real content (2448 for a raw-sector CD image, 19584 for
	 * a normal CD, 4096 for a hard disk), so a fixed entry count would mean
	 * 4KB on one file and 1.2MB on another. Simulation over that corpus showed
	 * a single entry already captures 97-100% of self-references wherever they
	 * are clustered at all, and further entries buy tenths of a percent, so
	 * the default budget is deliberately small.
	 *
	 * Allocated lazily on the first self-reference actually encountered - CHDs
	 * with none (3 of the 14 measured) never pay for this. */
	uint8_t *					selfcache_data;		/* selfcache_entries * hunkbytes */
	uint32_t *					selfcache_hunk;		/* hunk in each slot, ~0 = empty */
	uint32_t					selfcache_entries;
	uint32_t					selfcache_next;		/* round-robin victim */

#if LOWRAM_TARGET
	v5_lowram_map				lowram_map;		/* CHDv5 compressed-map lazy-decode state */

	/* CHDv5 can list up to 4 alternate codecs (header.compression[]); chdman
	 * picks whichever compresses best per hunk, so a normal build must
	 * init()/allocate all of them up front just in case a hunk needs one.
	 * Under LOWRAM_TARGET, codecintf[] is still resolved eagerly (cheap - just
	 * matching a tag to a codec_interface pointer) but init() itself is
	 * deferred to the first hunk_read_into_memory() call that actually needs
	 * that slot - see ensure_codec_ready(). */
	uint8_t						codec_lazy_initialized[4];

	/* `compressed` is grown on demand to the largest per-hunk compressed
	 * length actually read (map entries carry that length before the read
	 * happens), instead of being preallocated to the worst case
	 * (header.hunkbytes) at open() - see hunk_read_compressed(). Real
	 * compressed hunks are typically well under hunkbytes, and for a
	 * session that only touches part of a file (e.g. sequential playback)
	 * peak usage tracks what was actually touched, not the whole file. */
	uint32_t					compressed_capacity;
#endif
};


/***************************************************************************
    GLOBAL VARIABLES
***************************************************************************/

static const uint8_t nullmd5[CHD_MD5_BYTES] = { 0 };
static const uint8_t nullsha1[CHD_SHA1_BYTES] = { 0 };

/***************************************************************************
    PROTOTYPES
***************************************************************************/

/* core_file_callbacks wrappers over stdio */
static void *core_stdio_fopen(char const *path);
static uint64_t core_stdio_fsize(void *file);
static size_t core_stdio_fread(void *ptr, size_t size, size_t nmemb, void *file);
static int core_stdio_fclose(void *file);
static int core_stdio_fclose_nonowner(void *file); /* alternate fclose used by chd_open_file */
static int core_stdio_fseek(void* file, int64_t offset, int whence);

/* Legacy core_file wrappers */
static uint64_t core_legacy_fsize(void *file);
static size_t core_legacy_fread(void *ptr, size_t size, size_t nmemb, void *file);
static int core_legacy_fclose(void *file);
static int core_legacy_fseek(void* file, int64_t offset, int whence);

/* internal header operations */
static chd_error header_read(chd_file *chd);

/* internal hunk read/write */
/* Byte budget for the decoded-hunk cache described in struct _chd_file.
 * Entries = max(1, budget / hunkbytes). LOWRAM targets get a single entry,
 * which the measurements show is where nearly all of the benefit already is. */
#ifndef CHDR_SELF_CACHE_BYTES
#if LOWRAM_TARGET
#define CHDR_SELF_CACHE_BYTES 0
#else
#define CHDR_SELF_CACHE_BYTES 65536
#endif
#endif

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
static chd_error cd_lend_subcode_inflater(chd_file *chd, uint32_t tag, void *codec);
#endif
static chd_error hunk_read_into_memory(chd_file *chd, uint32_t hunknum, uint8_t *dest);

/*-------------------------------------------------
    selfcache_* - decoded-hunk cache for
    COMPRESSION_SELF back-references
-------------------------------------------------*/

static void selfcache_free(chd_file *chd)
{
	if (chd->selfcache_data != NULL) { free(chd->selfcache_data); chd->selfcache_data = NULL; }
	if (chd->selfcache_hunk != NULL) { free(chd->selfcache_hunk); chd->selfcache_hunk = NULL; }
	chd->selfcache_entries = 0;
	chd->selfcache_next = 0;
}

/* returns 1 once a cache exists (or already existed), 0 if it could not be
 * allocated - callers must treat failure as "just don't cache", never fatal */
static int selfcache_ensure(chd_file *chd)
{
	uint32_t n, i;

	if (chd->selfcache_entries != 0)
		return 1;
	if (chd->header.hunkbytes == 0)
		return 0;

	n = CHDR_SELF_CACHE_BYTES / chd->header.hunkbytes;
	if (n == 0)
		n = 1;

	chd->selfcache_data = (uint8_t *)malloc((size_t)n * chd->header.hunkbytes);
	chd->selfcache_hunk = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
	if (chd->selfcache_data == NULL || chd->selfcache_hunk == NULL) {
		selfcache_free(chd);
		return 0;
	}
	for (i = 0; i < n; i++)
		chd->selfcache_hunk[i] = (uint32_t)~0;
	chd->selfcache_entries = n;
	chd->selfcache_next = 0;
	return 1;
}

static int selfcache_lookup(chd_file *chd, uint32_t hunknum, uint8_t *dest)
{
	uint32_t i;
	for (i = 0; i < chd->selfcache_entries; i++) {
		if (chd->selfcache_hunk[i] == hunknum) {
			memcpy(dest, chd->selfcache_data + (size_t)i * chd->header.hunkbytes,
				chd->header.hunkbytes);
			return 1;
		}
	}
	return 0;
}

static void selfcache_store(chd_file *chd, uint32_t hunknum, const uint8_t *src)
{
	uint32_t i, slot;

	if (chd->selfcache_entries == 0)
		return;
	for (i = 0; i < chd->selfcache_entries; i++)
		if (chd->selfcache_hunk[i] == hunknum)
			return;             /* already resident */
	slot = chd->selfcache_next;
	chd->selfcache_next = (slot + 1) % chd->selfcache_entries;
	memcpy(chd->selfcache_data + (size_t)slot * chd->header.hunkbytes, src,
		chd->header.hunkbytes);
	chd->selfcache_hunk[slot] = hunknum;
}

/* internal map access */
static chd_error map_read(chd_file *chd);
#if LOWRAM_TARGET
static chd_error map_read_one_legacy(chd_file *chd, uint32_t hunknum, map_entry *entry);
static chd_error build_v5_map_checkpoints(chd_file *chd, chd_header *header, uint64_t file_base, uint64_t mapbytes,
	uint64_t firstoffs, uint8_t lengthbits, uint8_t selfbits, uint8_t parentbits, uint16_t mapcrc);
static chd_error ensure_v5_map_window(chd_file *chd, v5_map_window *win, uint64_t byte_start, uint64_t byte_end, uint32_t cpidx);
static chd_error v5_map_get_entry(chd_file *chd, uint32_t hunknum, uint8_t out[12]);
static chd_error ensure_codec_ready(chd_file *chd, size_t slot, void *codec);
#endif

/* metadata management */
static chd_error metadata_find_entry(chd_file *chd, uint32_t metatag, uint32_t metaindex, metadata_entry *metaentry);


/***************************************************************************
    CODEC INTERFACES
***************************************************************************/

static const codec_interface codec_interfaces[] =
{
	/* "none" or no compression */
	{
		CHDCOMPRESSION_NONE,
		"none",
		FALSE,
		NULL,
		NULL,
		NULL,
		NULL
	},

	/* standard zlib compression */
	{
		CHDCOMPRESSION_ZLIB,
		"zlib",
		FALSE,
		zlib_codec_init,
		zlib_codec_free,
		zlib_codec_decompress,
		NULL
	},

	/* zlib+ compression */
	{
		CHDCOMPRESSION_ZLIB_PLUS,
		"zlib+",
		FALSE,
		zlib_codec_init,
		zlib_codec_free,
		zlib_codec_decompress,
		NULL
	},

	/* V5 zlib compression */
	{
		CHD_CODEC_ZLIB,
		"zlib (Deflate)",
		FALSE,
		zlib_codec_init,
		zlib_codec_free,
		zlib_codec_decompress,
		NULL
	},

	/* V5 lzma compression */
	{
		CHD_CODEC_LZMA,
		"lzma (LZMA)",
		FALSE,
		lzma_codec_init,
		lzma_codec_free,
		lzma_codec_decompress,
		NULL
	},

	/* V5 huffman compression */
	{
		CHD_CODEC_HUFFMAN,
		"Huffman",
		FALSE,
		huff_codec_init,
		huff_codec_free,
		huff_codec_decompress,
		NULL
	},

	/* V5 flac compression */
	{
		CHD_CODEC_FLAC,
		"flac (FLAC)",
		FALSE,
		flac_codec_init,
		flac_codec_free,
		flac_codec_decompress,
		NULL
	},
	/* V5 zstd compression */
	{
		CHD_CODEC_ZSTD,
		"ZStandard",
		FALSE,
		zstd_codec_init,
		zstd_codec_free,
		zstd_codec_decompress,
		NULL
	},

	/* V5 CD zlib compression */
	{
		CHD_CODEC_CD_ZLIB,
		"cdzl (CD Deflate)",
		FALSE,
		cdzl_codec_init,
		cdzl_codec_free,
		cdzl_codec_decompress,
		NULL
	},

	/* V5 CD lzma compression */
	{
		CHD_CODEC_CD_LZMA,
		"cdlz (CD LZMA)",
		FALSE,
		cdlz_codec_init,
		cdlz_codec_free,
		cdlz_codec_decompress,
		NULL
	},

	/* V5 CD flac compression */
	{
		CHD_CODEC_CD_FLAC,
		"cdfl (CD FLAC)",
		FALSE,
		cdfl_codec_init,
		cdfl_codec_free,
		cdfl_codec_decompress,
		NULL
	},
	/* V5 CD zstd compression */
	{
		CHD_CODEC_CD_ZSTD,
		"cdzs (CD ZStandard)",
		FALSE,
		cdzs_codec_init,
		cdzs_codec_free,
		cdzs_codec_decompress,
		NULL
	},
	/* V5 A/V Huffman (laserdisc) */
	{
		CHD_CODEC_AVHUFF,
		"A/V Huffman",
		FALSE,
		avhuff_codec_init,
		avhuff_codec_free,
		avhuff_codec_decompress,
		NULL
	},
	/* V3/V4 A/V Huffman (laserdisc) — CHDCOMPRESSION_AV */
	{
		CHDCOMPRESSION_AV,
		"A/V Huffman (v3/v4)",
		FALSE,
		avhuff_codec_init,
		avhuff_codec_free,
		avhuff_codec_decompress,
		NULL
	}
};

/***************************************************************************
    INLINE FUNCTIONS
***************************************************************************/

/*-------------------------------------------------
    seek_and_read - read data from file at
	specified position
-------------------------------------------------*/

static CHDR_INLINE int seek_and_read(chd_file *chd, uint64_t position, void *buffer, size_t total_bytes)
{
	if (core_fseek(&chd->file, position, SEEK_SET) != 0)
		return FALSE;
	if (core_fread(&chd->file, buffer, total_bytes) != total_bytes)
		return FALSE;

	return TRUE;
}

/*-------------------------------------------------
    get_bigendian_uint64_t - fetch a uint64_t from
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE uint64_t get_bigendian_uint64_t(const uint8_t *base)
{
	return ((uint64_t)base[0] << 56) | ((uint64_t)base[1] << 48) | ((uint64_t)base[2] << 40) | ((uint64_t)base[3] << 32) |
			((uint64_t)base[4] << 24) | ((uint64_t)base[5] << 16) | ((uint64_t)base[6] << 8) | (uint64_t)base[7];
}

/*-------------------------------------------------
    put_bigendian_uint64_t - write a uint64_t to
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE void put_bigendian_uint64_t(uint8_t *base, uint64_t value)
{
	base[0] = value >> 56;
	base[1] = value >> 48;
	base[2] = value >> 40;
	base[3] = value >> 32;
	base[4] = value >> 24;
	base[5] = value >> 16;
	base[6] = value >> 8;
	base[7] = value;
}

/*-------------------------------------------------
    get_bigendian_uint48 - fetch a UINT48 from
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE uint64_t get_bigendian_uint48(const uint8_t *base)
{
	return  ((uint64_t)base[0] << 40) | ((uint64_t)base[1] << 32) |
			((uint64_t)base[2] << 24) | ((uint64_t)base[3] << 16) | ((uint64_t)base[4] << 8) | (uint64_t)base[5];
}

/*-------------------------------------------------
    put_bigendian_uint48 - write a UINT48 to
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE void put_bigendian_uint48(uint8_t *base, uint64_t value)
{
	value &= 0xffffffffffff;
	base[0] = value >> 40;
	base[1] = value >> 32;
	base[2] = value >> 24;
	base[3] = value >> 16;
	base[4] = value >> 8;
	base[5] = value;
}
/*-------------------------------------------------
    get_bigendian_uint32_t - fetch a uint32_t from
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE uint32_t get_bigendian_uint32_t(const uint8_t *base)
{
	/* Cast before shifting: base[0] promotes to int, so base[0] << 24 is
	 * signed overflow - undefined - for any byte >= 0x80. Real files never hit
	 * it because every field read through here holds either a small count or a
	 * four-character codec tag, and those are ASCII, but a malformed file only
	 * has to set one high bit. The uint48 and uint64 readers above already
	 * cast for the same reason. */
	return ((uint32_t)base[0] << 24) | ((uint32_t)base[1] << 16) |
	       ((uint32_t)base[2] << 8)  |  (uint32_t)base[3];
}

/*-------------------------------------------------
    put_bigendian_uint32_t - write a uint32_t to
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE void put_bigendian_uint32_t(uint8_t *base, uint32_t value)
{
	base[0] = value >> 24;
	base[1] = value >> 16;
	base[2] = value >> 8;
	base[3] = value;
}

/*-------------------------------------------------
    put_bigendian_uint24 - write a UINT24 to
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE void put_bigendian_uint24(uint8_t *base, uint32_t value)
{
	value &= 0xffffff;
	base[0] = value >> 16;
	base[1] = value >> 8;
	base[2] = value;
}

/*-------------------------------------------------
    get_bigendian_uint24 - fetch a UINT24 from
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE uint32_t get_bigendian_uint24(const uint8_t *base)
{
	return (base[0] << 16) | (base[1] << 8) | base[2];
}

/*-------------------------------------------------
    get_bigendian_uint16 - fetch a uint16_t from
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE uint16_t get_bigendian_uint16(const uint8_t *base)
{
	return (base[0] << 8) | base[1];
}

/*-------------------------------------------------
    put_bigendian_uint16 - write a uint16_t to
    the data stream in bigendian order
-------------------------------------------------*/

static CHDR_INLINE void put_bigendian_uint16(uint8_t *base, uint16_t value)
{
	base[0] = value >> 8;
	base[1] = value;
}

/*-------------------------------------------------
    map_extract - extract a single map
    entry from the datastream
-------------------------------------------------*/

static CHDR_INLINE void map_extract(const uint8_t *base, map_entry *entry)
{
	entry->offset = get_bigendian_uint64_t(&base[0]);
	entry->crc = get_bigendian_uint32_t(&base[8]);
	entry->length = get_bigendian_uint16(&base[12]) | (base[14] << 16);
	entry->flags = base[15];
}

/*-------------------------------------------------
    map_size_v5 - calculate CHDv5 map size
-------------------------------------------------*/
static CHDR_INLINE int map_size_v5(chd_header* header, size_t *size)
{
	/* Avoid overflow due to corrupted data. */
	const size_t max_hunkcount = ((size_t)-1 / header->mapentrybytes);
	uint64_t want;

	if (header->hunkcount > max_hunkcount)
		return FALSE;

	/* A compressed v5 map is entropy-coded, so its on-disk size says nothing
	 * about how many hunks it holds and cannot be used to bound this. Cap the
	 * decompressed map directly: CHD_MAX_MAP_SIZE covers every image size
	 * this library claims to support (over 400GB at CD geometry, ~89GB at
	 * 4KB hunks) while keeping a malformed hunkcount from asking for more
	 * memory than any host would hand out. */
	want = (uint64_t)header->hunkcount * header->mapentrybytes;
	if (want > CHD_MAX_MAP_SIZE)
		return FALSE;

	*size = (size_t)want;
	return TRUE;
}

/*-------------------------------------------------
    crc16_update - calculate CRC16 (from
    hashing.cpp), continuing from a prior partial
    result - lets LOWRAM_TARGET verify the map CRC
    across chunks without materializing the whole
    buffer at once
-------------------------------------------------*/
static uint16_t crc16_update(uint16_t crc, const void *data, uint32_t length)
{
	static const uint16_t s_table[256] =
	{
		0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
		0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
		0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
		0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
		0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
		0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
		0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
		0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
		0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
		0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
		0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
		0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
		0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
		0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
		0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
		0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
		0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
		0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
		0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
		0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
		0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
		0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
		0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
		0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
		0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
		0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
		0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
		0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
		0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
		0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
		0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
		0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
	};

	static const uint16_t s_table1[256] =
	{
		0x0000, 0x3331, 0x6662, 0x5553, 0xccc4, 0xfff5, 0xaaa6, 0x9997,
		0x89a9, 0xba98, 0xefcb, 0xdcfa, 0x456d, 0x765c, 0x230f, 0x103e,
		0x0373, 0x3042, 0x6511, 0x5620, 0xcfb7, 0xfc86, 0xa9d5, 0x9ae4,
		0x8ada, 0xb9eb, 0xecb8, 0xdf89, 0x461e, 0x752f, 0x207c, 0x134d,
		0x06e6, 0x35d7, 0x6084, 0x53b5, 0xca22, 0xf913, 0xac40, 0x9f71,
		0x8f4f, 0xbc7e, 0xe92d, 0xda1c, 0x438b, 0x70ba, 0x25e9, 0x16d8,
		0x0595, 0x36a4, 0x63f7, 0x50c6, 0xc951, 0xfa60, 0xaf33, 0x9c02,
		0x8c3c, 0xbf0d, 0xea5e, 0xd96f, 0x40f8, 0x73c9, 0x269a, 0x15ab,
		0x0dcc, 0x3efd, 0x6bae, 0x589f, 0xc108, 0xf239, 0xa76a, 0x945b,
		0x8465, 0xb754, 0xe207, 0xd136, 0x48a1, 0x7b90, 0x2ec3, 0x1df2,
		0x0ebf, 0x3d8e, 0x68dd, 0x5bec, 0xc27b, 0xf14a, 0xa419, 0x9728,
		0x8716, 0xb427, 0xe174, 0xd245, 0x4bd2, 0x78e3, 0x2db0, 0x1e81,
		0x0b2a, 0x381b, 0x6d48, 0x5e79, 0xc7ee, 0xf4df, 0xa18c, 0x92bd,
		0x8283, 0xb1b2, 0xe4e1, 0xd7d0, 0x4e47, 0x7d76, 0x2825, 0x1b14,
		0x0859, 0x3b68, 0x6e3b, 0x5d0a, 0xc49d, 0xf7ac, 0xa2ff, 0x91ce,
		0x81f0, 0xb2c1, 0xe792, 0xd4a3, 0x4d34, 0x7e05, 0x2b56, 0x1867,
		0x1b98, 0x28a9, 0x7dfa, 0x4ecb, 0xd75c, 0xe46d, 0xb13e, 0x820f,
		0x9231, 0xa100, 0xf453, 0xc762, 0x5ef5, 0x6dc4, 0x3897, 0x0ba6,
		0x18eb, 0x2bda, 0x7e89, 0x4db8, 0xd42f, 0xe71e, 0xb24d, 0x817c,
		0x9142, 0xa273, 0xf720, 0xc411, 0x5d86, 0x6eb7, 0x3be4, 0x08d5,
		0x1d7e, 0x2e4f, 0x7b1c, 0x482d, 0xd1ba, 0xe28b, 0xb7d8, 0x84e9,
		0x94d7, 0xa7e6, 0xf2b5, 0xc184, 0x5813, 0x6b22, 0x3e71, 0x0d40,
		0x1e0d, 0x2d3c, 0x786f, 0x4b5e, 0xd2c9, 0xe1f8, 0xb4ab, 0x879a,
		0x97a4, 0xa495, 0xf1c6, 0xc2f7, 0x5b60, 0x6851, 0x3d02, 0x0e33,
		0x1654, 0x2565, 0x7036, 0x4307, 0xda90, 0xe9a1, 0xbcf2, 0x8fc3,
		0x9ffd, 0xaccc, 0xf99f, 0xcaae, 0x5339, 0x6008, 0x355b, 0x066a,
		0x1527, 0x2616, 0x7345, 0x4074, 0xd9e3, 0xead2, 0xbf81, 0x8cb0,
		0x9c8e, 0xafbf, 0xfaec, 0xc9dd, 0x504a, 0x637b, 0x3628, 0x0519,
		0x10b2, 0x2383, 0x76d0, 0x45e1, 0xdc76, 0xef47, 0xba14, 0x8925,
		0x991b, 0xaa2a, 0xff79, 0xcc48, 0x55df, 0x66ee, 0x33bd, 0x008c,
		0x13c1, 0x20f0, 0x75a3, 0x4692, 0xdf05, 0xec34, 0xb967, 0x8a56,
		0x9a68, 0xa959, 0xfc0a, 0xcf3b, 0x56ac, 0x659d, 0x30ce, 0x03ff
	};
	static const uint16_t s_table2[256] =
	{
		0x0000, 0x3730, 0x6e60, 0x5950, 0xdcc0, 0xebf0, 0xb2a0, 0x8590,
		0xa9a1, 0x9e91, 0xc7c1, 0xf0f1, 0x7561, 0x4251, 0x1b01, 0x2c31,
		0x4363, 0x7453, 0x2d03, 0x1a33, 0x9fa3, 0xa893, 0xf1c3, 0xc6f3,
		0xeac2, 0xddf2, 0x84a2, 0xb392, 0x3602, 0x0132, 0x5862, 0x6f52,
		0x86c6, 0xb1f6, 0xe8a6, 0xdf96, 0x5a06, 0x6d36, 0x3466, 0x0356,
		0x2f67, 0x1857, 0x4107, 0x7637, 0xf3a7, 0xc497, 0x9dc7, 0xaaf7,
		0xc5a5, 0xf295, 0xabc5, 0x9cf5, 0x1965, 0x2e55, 0x7705, 0x4035,
		0x6c04, 0x5b34, 0x0264, 0x3554, 0xb0c4, 0x87f4, 0xdea4, 0xe994,
		0x1dad, 0x2a9d, 0x73cd, 0x44fd, 0xc16d, 0xf65d, 0xaf0d, 0x983d,
		0xb40c, 0x833c, 0xda6c, 0xed5c, 0x68cc, 0x5ffc, 0x06ac, 0x319c,
		0x5ece, 0x69fe, 0x30ae, 0x079e, 0x820e, 0xb53e, 0xec6e, 0xdb5e,
		0xf76f, 0xc05f, 0x990f, 0xae3f, 0x2baf, 0x1c9f, 0x45cf, 0x72ff,
		0x9b6b, 0xac5b, 0xf50b, 0xc23b, 0x47ab, 0x709b, 0x29cb, 0x1efb,
		0x32ca, 0x05fa, 0x5caa, 0x6b9a, 0xee0a, 0xd93a, 0x806a, 0xb75a,
		0xd808, 0xef38, 0xb668, 0x8158, 0x04c8, 0x33f8, 0x6aa8, 0x5d98,
		0x71a9, 0x4699, 0x1fc9, 0x28f9, 0xad69, 0x9a59, 0xc309, 0xf439,
		0x3b5a, 0x0c6a, 0x553a, 0x620a, 0xe79a, 0xd0aa, 0x89fa, 0xbeca,
		0x92fb, 0xa5cb, 0xfc9b, 0xcbab, 0x4e3b, 0x790b, 0x205b, 0x176b,
		0x7839, 0x4f09, 0x1659, 0x2169, 0xa4f9, 0x93c9, 0xca99, 0xfda9,
		0xd198, 0xe6a8, 0xbff8, 0x88c8, 0x0d58, 0x3a68, 0x6338, 0x5408,
		0xbd9c, 0x8aac, 0xd3fc, 0xe4cc, 0x615c, 0x566c, 0x0f3c, 0x380c,
		0x143d, 0x230d, 0x7a5d, 0x4d6d, 0xc8fd, 0xffcd, 0xa69d, 0x91ad,
		0xfeff, 0xc9cf, 0x909f, 0xa7af, 0x223f, 0x150f, 0x4c5f, 0x7b6f,
		0x575e, 0x606e, 0x393e, 0x0e0e, 0x8b9e, 0xbcae, 0xe5fe, 0xd2ce,
		0x26f7, 0x11c7, 0x4897, 0x7fa7, 0xfa37, 0xcd07, 0x9457, 0xa367,
		0x8f56, 0xb866, 0xe136, 0xd606, 0x5396, 0x64a6, 0x3df6, 0x0ac6,
		0x6594, 0x52a4, 0x0bf4, 0x3cc4, 0xb954, 0x8e64, 0xd734, 0xe004,
		0xcc35, 0xfb05, 0xa255, 0x9565, 0x10f5, 0x27c5, 0x7e95, 0x49a5,
		0xa031, 0x9701, 0xce51, 0xf961, 0x7cf1, 0x4bc1, 0x1291, 0x25a1,
		0x0990, 0x3ea0, 0x67f0, 0x50c0, 0xd550, 0xe260, 0xbb30, 0x8c00,
		0xe352, 0xd462, 0x8d32, 0xba02, 0x3f92, 0x08a2, 0x51f2, 0x66c2,
		0x4af3, 0x7dc3, 0x2493, 0x13a3, 0x9633, 0xa103, 0xf853, 0xcf63
	};
	static const uint16_t s_table3[256] =
	{
		0x0000, 0x76b4, 0xed68, 0x9bdc, 0xcaf1, 0xbc45, 0x2799, 0x512d,
		0x85c3, 0xf377, 0x68ab, 0x1e1f, 0x4f32, 0x3986, 0xa25a, 0xd4ee,
		0x1ba7, 0x6d13, 0xf6cf, 0x807b, 0xd156, 0xa7e2, 0x3c3e, 0x4a8a,
		0x9e64, 0xe8d0, 0x730c, 0x05b8, 0x5495, 0x2221, 0xb9fd, 0xcf49,
		0x374e, 0x41fa, 0xda26, 0xac92, 0xfdbf, 0x8b0b, 0x10d7, 0x6663,
		0xb28d, 0xc439, 0x5fe5, 0x2951, 0x787c, 0x0ec8, 0x9514, 0xe3a0,
		0x2ce9, 0x5a5d, 0xc181, 0xb735, 0xe618, 0x90ac, 0x0b70, 0x7dc4,
		0xa92a, 0xdf9e, 0x4442, 0x32f6, 0x63db, 0x156f, 0x8eb3, 0xf807,
		0x6e9c, 0x1828, 0x83f4, 0xf540, 0xa46d, 0xd2d9, 0x4905, 0x3fb1,
		0xeb5f, 0x9deb, 0x0637, 0x7083, 0x21ae, 0x571a, 0xccc6, 0xba72,
		0x753b, 0x038f, 0x9853, 0xeee7, 0xbfca, 0xc97e, 0x52a2, 0x2416,
		0xf0f8, 0x864c, 0x1d90, 0x6b24, 0x3a09, 0x4cbd, 0xd761, 0xa1d5,
		0x59d2, 0x2f66, 0xb4ba, 0xc20e, 0x9323, 0xe597, 0x7e4b, 0x08ff,
		0xdc11, 0xaaa5, 0x3179, 0x47cd, 0x16e0, 0x6054, 0xfb88, 0x8d3c,
		0x4275, 0x34c1, 0xaf1d, 0xd9a9, 0x8884, 0xfe30, 0x65ec, 0x1358,
		0xc7b6, 0xb102, 0x2ade, 0x5c6a, 0x0d47, 0x7bf3, 0xe02f, 0x969b,
		0xdd38, 0xab8c, 0x3050, 0x46e4, 0x17c9, 0x617d, 0xfaa1, 0x8c15,
		0x58fb, 0x2e4f, 0xb593, 0xc327, 0x920a, 0xe4be, 0x7f62, 0x09d6,
		0xc69f, 0xb02b, 0x2bf7, 0x5d43, 0x0c6e, 0x7ada, 0xe106, 0x97b2,
		0x435c, 0x35e8, 0xae34, 0xd880, 0x89ad, 0xff19, 0x64c5, 0x1271,
		0xea76, 0x9cc2, 0x071e, 0x71aa, 0x2087, 0x5633, 0xcdef, 0xbb5b,
		0x6fb5, 0x1901, 0x82dd, 0xf469, 0xa544, 0xd3f0, 0x482c, 0x3e98,
		0xf1d1, 0x8765, 0x1cb9, 0x6a0d, 0x3b20, 0x4d94, 0xd648, 0xa0fc,
		0x7412, 0x02a6, 0x997a, 0xefce, 0xbee3, 0xc857, 0x538b, 0x253f,
		0xb3a4, 0xc510, 0x5ecc, 0x2878, 0x7955, 0x0fe1, 0x943d, 0xe289,
		0x3667, 0x40d3, 0xdb0f, 0xadbb, 0xfc96, 0x8a22, 0x11fe, 0x674a,
		0xa803, 0xdeb7, 0x456b, 0x33df, 0x62f2, 0x1446, 0x8f9a, 0xf92e,
		0x2dc0, 0x5b74, 0xc0a8, 0xb61c, 0xe731, 0x9185, 0x0a59, 0x7ced,
		0x84ea, 0xf25e, 0x6982, 0x1f36, 0x4e1b, 0x38af, 0xa373, 0xd5c7,
		0x0129, 0x779d, 0xec41, 0x9af5, 0xcbd8, 0xbd6c, 0x26b0, 0x5004,
		0x9f4d, 0xe9f9, 0x7225, 0x0491, 0x55bc, 0x2308, 0xb8d4, 0xce60,
		0x1a8e, 0x6c3a, 0xf7e6, 0x8152, 0xd07f, 0xa6cb, 0x3d17, 0x4ba3
	};

	const uint8_t *src = (uint8_t*)data;

	/* Slice-by-4. The byte-at-a-time form is 12 instructions per byte on RV32
	 * (two of them just truncating back to uint16_t), and it runs over every
	 * decoded hunk under VERIFY_BLOCK_CRC, so it costs about 0.76 ms per
	 * 19584-byte hunk on an ESP32-P4 - more than the zstd decode it is
	 * checking. Folding four bytes per iteration takes it to 6.25.
	 *
	 * s_table1/2/3 are s_table advanced by one, two and three byte positions,
	 * so the four lookups can be XORed together. Verified identical to the
	 * byte-at-a-time result for every length 0..4096 and 256 starting CRCs. */
	while (length >= 4)
	{
		const uint16_t x = (uint16_t)((crc >> 8) ^ src[0]);
		const uint16_t y = (uint16_t)((crc & 0xff) ^ src[1]);

		crc = (uint16_t)(s_table3[x] ^ s_table2[y] ^ s_table1[src[2]] ^ s_table[src[3]]);
		src += 4;
		length -= 4;
	}

	/* rip through the source data */
	while (length-- != 0)
		crc = (crc << 8) ^ s_table[(crc >> 8) ^ *src++];
	return crc;
}

/*-------------------------------------------------
    chd_crc16 - calculate CRC16 (from hashing.cpp)
-------------------------------------------------*/
uint16_t chd_crc16(const void *data, uint32_t length)
{
	return crc16_update(0xffff, data, length);
}

#if VERIFY_BLOCK_CRC
/*-------------------------------------------------
    chd_crc32 - calculate CRC32 of a decoded
    v1-v4 hunk
-------------------------------------------------*/

static uint32_t chd_crc32(const void *data, uint32_t length)
{
#ifdef CHDR_SYSTEM_ZLIB
	return (uint32_t)crc32(0, (const Bytef *)data, length);
#else
	return (uint32_t)mz_crc32(MZ_CRC32_INIT, (const mz_uint8 *)data, length);
#endif
}
#endif

/*-------------------------------------------------
	compressed - test if CHD file is compressed
+-------------------------------------------------*/
static CHDR_INLINE int chd_compressed(chd_header* header) {
	return header->compression[0] != CHD_CODEC_NONE;
}

/*-------------------------------------------------
	decompress_v5_map - decompress the v5 map
-------------------------------------------------*/

static chd_error decompress_v5_map(chd_file* chd, chd_header* header)
{
#if !LOWRAM_TARGET
	uint32_t hunknum;
	int repcount = 0;
	uint8_t lastcomp = 0;
	uint32_t last_self = 0;
	uint64_t last_parent = 0;
	uint64_t curoffset;
	struct bitstream* bitbuf;
	uint8_t *compressed_ptr;
	struct huffman_decoder* decoder;
	enum huffman_error err;
#endif
	uint32_t mapbytes;
	uint64_t firstoffs;
	uint16_t mapcrc;
	uint8_t lengthbits;
	uint8_t selfbits;
	uint8_t parentbits;
	uint8_t rawbuf[16];
	size_t rawmapsize;

	if (!map_size_v5(header, &rawmapsize))
		return CHDERR_INVALID_FILE;

	if (!chd_compressed(header))
	{
		if ((header->mapoffset + rawmapsize) >= chd->file_size || (header->mapoffset + rawmapsize) < header->mapoffset)
			return CHDERR_INVALID_FILE;

#if LOWRAM_TARGET
		/* not entropy-coded - each entry is independently seekable, so there's
		 * nothing to materialize. v5_map_get_entry() reads it lazily. */
		return CHDERR_NONE;
#else
		header->rawmap = (uint8_t*)malloc(rawmapsize);
		if (header->rawmap == NULL)
			return CHDERR_OUT_OF_MEMORY;
		if (!seek_and_read(chd, header->mapoffset, header->rawmap, rawmapsize))
			return CHDERR_READ_ERROR;
		return CHDERR_NONE;
#endif
	}

	/* read the header */
	if (!seek_and_read(chd, header->mapoffset, rawbuf, sizeof(rawbuf)))
		return CHDERR_READ_ERROR;
	mapbytes = get_bigendian_uint32_t(&rawbuf[0]);
	firstoffs = get_bigendian_uint48(&rawbuf[4]);
	mapcrc = get_bigendian_uint16(&rawbuf[10]);
	lengthbits = rawbuf[12];
	selfbits = rawbuf[13];
	parentbits = rawbuf[14];

	/* These three are raw bytes from the file and they become the bit width
	 * passed to bitstream_read() for every map entry. Anything above 32 makes
	 * bitstream_peek() shift by a negative amount, so a malformed file could
	 * reach undefined behaviour before any other check ran. chdman derives
	 * them from hunkbytes and the hunk count, so they never legitimately
	 * exceed 32. */
	if (lengthbits > 32 || selfbits > 32 || parentbits > 32)
		return CHDERR_INVALID_FILE;

	/* now read the map */
	if ((header->mapoffset + mapbytes) < header->mapoffset || (header->mapoffset + mapbytes) >= chd->file_size)
		return CHDERR_INVALID_FILE;

#if LOWRAM_TARGET
	/* build_v5_map_checkpoints() owns reading the compressed blob (in small
	 * rolling chunks, not all mapbytes at once) and creating the huffman
	 * decoder itself - nothing to set up here. */
	{
		chd_error cperr = build_v5_map_checkpoints(chd, header, header->mapoffset + 16, mapbytes,
			firstoffs, lengthbits, selfbits, parentbits, mapcrc);
		if (cperr != CHDERR_NONE)
			return cperr;
		chd->lowram_map.mapbytes = mapbytes;
		chd->lowram_map.firstoffs = firstoffs;
		chd->lowram_map.lengthbits = lengthbits;
		chd->lowram_map.selfbits = selfbits;
		chd->lowram_map.parentbits = parentbits;
		chd->lowram_map.pass1_window.checkpoint_idx = 0xFFFFFFFFu;
		chd->lowram_map.pass2_window.checkpoint_idx = 0xFFFFFFFFu;
		return CHDERR_NONE;
	}
#else
	compressed_ptr = (uint8_t*)malloc(sizeof(uint8_t) * mapbytes);
	if (compressed_ptr == NULL)
		return CHDERR_OUT_OF_MEMORY;
	if (!seek_and_read(chd, header->mapoffset + 16, compressed_ptr, mapbytes))
	{
		free(compressed_ptr);
		return CHDERR_READ_ERROR;
	}
	bitbuf = create_bitstream(compressed_ptr, sizeof(uint8_t) * mapbytes);

	/* first decode the compression types */
	decoder = create_huffman_decoder(16, 8);
	if (decoder == NULL)
	{
		free(compressed_ptr);
		free(bitbuf);
		return CHDERR_OUT_OF_MEMORY;
	}

	err = huffman_import_tree_rle(decoder, bitbuf);
	if (err != HUFFERR_NONE)
	{
		free(compressed_ptr);
		free(bitbuf);
		delete_huffman_decoder(decoder);
		return CHDERR_DECOMPRESSION_ERROR;
	}

	header->rawmap = (uint8_t*)malloc(rawmapsize);
	if (header->rawmap == NULL)
	{
		free(compressed_ptr);
		free(bitbuf);
		delete_huffman_decoder(decoder);
		return CHDERR_OUT_OF_MEMORY;
	}

	for (hunknum = 0; hunknum < header->hunkcount; hunknum++)
	{
		uint8_t *rawmap = header->rawmap + (hunknum * 12);
		if (repcount > 0)
			rawmap[0] = lastcomp, repcount--;
		else
		{
			uint8_t val;
			if (bitstream_overflow(bitbuf))
			{
				free(compressed_ptr);
				free(bitbuf);
				delete_huffman_decoder(decoder);
				return CHDERR_DECOMPRESSION_ERROR;
			}

			val = huffman_decode_one(decoder, bitbuf);
			if (val == COMPRESSION_RLE_SMALL)
				rawmap[0] = lastcomp, repcount = 2 + huffman_decode_one(decoder, bitbuf);
			else if (val == COMPRESSION_RLE_LARGE)
				rawmap[0] = lastcomp, repcount = 2 + 16 + (huffman_decode_one(decoder, bitbuf) << 4), repcount += huffman_decode_one(decoder, bitbuf);
			else
				rawmap[0] = lastcomp = val;
		}
	}

	/* then iterate through the hunks and extract the needed data */
	curoffset = firstoffs;
	for (hunknum = 0; hunknum < header->hunkcount; hunknum++)
	{
		uint8_t *rawmap = header->rawmap + (hunknum * 12);
		uint64_t offset = curoffset;
		uint32_t length = 0;
		uint16_t crc = 0;
		switch (rawmap[0])
		{
			/* base types */
			case COMPRESSION_TYPE_0:
			case COMPRESSION_TYPE_1:
			case COMPRESSION_TYPE_2:
			case COMPRESSION_TYPE_3:
				curoffset += length = bitstream_read(bitbuf, lengthbits);
				crc = bitstream_read(bitbuf, 16);
				break;

			case COMPRESSION_NONE:
				curoffset += length = header->hunkbytes;
				crc = bitstream_read(bitbuf, 16);
				break;

			case COMPRESSION_SELF:
				last_self = offset = bitstream_read(bitbuf, selfbits);
				break;

			case COMPRESSION_PARENT:
				offset = bitstream_read(bitbuf, parentbits);
				last_parent = offset;
				break;

			/* pseudo-types; convert into base types */
			case COMPRESSION_SELF_1:
				last_self++;
				/* Fallthrough */
			case COMPRESSION_SELF_0:
				rawmap[0] = COMPRESSION_SELF;
				offset = last_self;
				break;

			case COMPRESSION_PARENT_SELF:
				rawmap[0] = COMPRESSION_PARENT;
				last_parent = offset = ( ((uint64_t)hunknum) * ((uint64_t)header->hunkbytes) ) / header->unitbytes;
				break;

			case COMPRESSION_PARENT_1:
				last_parent += header->hunkbytes / header->unitbytes;
				/* Fallthrough */
			case COMPRESSION_PARENT_0:
				rawmap[0] = COMPRESSION_PARENT;
				offset = last_parent;
				break;
		}
		/* UINT24 length */
		put_bigendian_uint24(&rawmap[1], length);

		/* UINT48 offset */
		put_bigendian_uint48(&rawmap[4], offset);

		/* crc16 */
		put_bigendian_uint16(&rawmap[10], crc);
	}

	/* free memory */
	free(compressed_ptr);
	free(bitbuf);
	delete_huffman_decoder(decoder);

	/* verify the final CRC */
	if (chd_crc16(&header->rawmap[0], header->hunkcount * 12) != mapcrc)
		return CHDERR_DECOMPRESSION_ERROR;

	return CHDERR_NONE;
#endif
}

#if LOWRAM_TARGET
/*-------------------------------------------------
    build_v5_map_checkpoints - run both v5 map
    decode passes once (same as decompress_v5_map's
    non-LOWRAM_TARGET path), but instead of writing a
    12-byte entry per hunk into a fully materialized
    buffer, record a resumable checkpoint every
    LOWRAM_TARGET_CHECKPOINT_STRIDE hunks. Verifies the
    same map CRC as the normal path, computed
    incrementally instead of over one big buffer.

    Reads the compressed map from the file in small
    rolling chunks (v5_build_stream) instead of one
    mapbytes-sized buffer, and never materializes a
    per-hunk compression-type array either: pass 1 is
    decoded twice (once to find where it ends / pass 2
    begins, again as a "mirror" alongside pass 2 to
    regenerate each hunk's type on the fly) rather than
    stored - a second linear scan is a one-time,
    bounded CPU cost, in exchange for the resident
    footprint during this whole function being a
    couple of small chunk buffers instead of two
    buffers sized off total hunk count / map size.
-------------------------------------------------*/

#define V5_BUILD_STREAM_CHUNK  4096u
#define V5_BUILD_STREAM_MARGIN 16u

typedef struct _v5_build_stream v5_build_stream;
struct _v5_build_stream
{
	chd_file *			chd;
	uint64_t			file_base;	/* absolute file offset of the compressed blob's start */
	uint64_t			mapbytes;	/* total blob size, for bounds */
	uint64_t			window_off;	/* blob-relative byte offset that buf[0] corresponds to */
	uint32_t			buf_valid;	/* valid bytes currently in buf */
	struct bitstream	bs;
	uint8_t				buf[V5_BUILD_STREAM_CHUNK];
};

static chd_error v5_build_stream_fill(v5_build_stream *s, uint64_t blob_byte_off)
{
	uint64_t remain = (blob_byte_off < s->mapbytes) ? (s->mapbytes - blob_byte_off) : 0;
	uint32_t want = (remain < V5_BUILD_STREAM_CHUNK) ? (uint32_t)remain : V5_BUILD_STREAM_CHUNK;

	if (want != 0 && !seek_and_read(s->chd, s->file_base + blob_byte_off, s->buf, want))
		return CHDERR_READ_ERROR;
	s->window_off = blob_byte_off;
	s->buf_valid = want;
	s->bs.buffer = 0; s->bs.bits = 0; s->bs.read = s->buf; s->bs.doffset = 0; s->bs.dlength = want;
	return CHDERR_NONE;
}

static chd_error v5_build_stream_init(v5_build_stream *s, chd_file *chd, uint64_t file_base, uint64_t mapbytes, uint64_t start_byte)
{
	s->chd = chd;
	s->file_base = file_base;
	s->mapbytes = mapbytes;
	return v5_build_stream_fill(s, start_byte);
}

/* call before decoding each hunk: makes sure at least V5_BUILD_STREAM_MARGIN
 * bytes remain available past the current position (comfortably more than
 * one hunk's worth of pass1/pass2 fields ever needs), refilling from the
 * file if not. No-op the common case - only refills once per chunk. */
static chd_error v5_build_stream_ensure(v5_build_stream *s)
{
	uint64_t local_byte = bitstream_position_bits(&s->bs) / 8;
	uint64_t global_byte = s->window_off + local_byte;

	if (local_byte < s->buf_valid && (s->buf_valid - local_byte) >= V5_BUILD_STREAM_MARGIN)
		return CHDERR_NONE;
	if (global_byte + V5_BUILD_STREAM_MARGIN >= s->mapbytes)
		return CHDERR_NONE; /* near end of blob - bitstream_overflow() catches true EOF */

	{
		unsigned rem_bits = (unsigned)(bitstream_position_bits(&s->bs) % 8);
		chd_error err = v5_build_stream_fill(s, global_byte);
		if (err != CHDERR_NONE)
			return err;
		if (rem_bits)
			bitstream_read(&s->bs, rem_bits);
	}
	return CHDERR_NONE;
}

static uint64_t v5_build_stream_position_bits(v5_build_stream *s)
{
	return s->window_off * 8 + bitstream_position_bits(&s->bs);
}

static chd_error build_v5_map_checkpoints(chd_file *chd, chd_header *header, uint64_t file_base, uint64_t mapbytes,
	uint64_t firstoffs, uint8_t lengthbits, uint8_t selfbits, uint8_t parentbits, uint16_t mapcrc)
{
	uint32_t hunknum;
	int repcount;
	uint8_t lastcomp;
	uint32_t last_self;
	uint64_t last_parent;
	uint64_t curoffset;
	uint16_t running_crc = 0xffff;
	uint32_t checkpoint_capacity;
	v5_map_checkpoint *checkpoints;
	uint32_t checkpoint_count;
	struct huffman_decoder *decoder;
	enum huffman_error herr;
	v5_build_stream pass1, pass1_mirror, pass2;
	uint64_t pass1_symbols_start_bits;
	chd_error err;

	checkpoint_capacity = header->hunkcount / LOWRAM_TARGET_CHECKPOINT_STRIDE + 1;
	checkpoints = (v5_map_checkpoint*)malloc(sizeof(v5_map_checkpoint) * checkpoint_capacity);
	if (checkpoints == NULL)
		return CHDERR_OUT_OF_MEMORY;

	decoder = create_huffman_decoder(16, 8);
	if (decoder == NULL)
	{
		free(checkpoints);
		return CHDERR_OUT_OF_MEMORY;
	}

	if ((err = v5_build_stream_init(&pass1, chd, file_base, mapbytes, 0)) != CHDERR_NONE ||
		(herr = huffman_import_tree_rle(decoder, &pass1.bs)) != HUFFERR_NONE)
	{
		free(checkpoints);
		delete_huffman_decoder(decoder);
		return (err != CHDERR_NONE) ? err : CHDERR_DECOMPRESSION_ERROR;
	}
	pass1_symbols_start_bits = v5_build_stream_position_bits(&pass1);

	/* pass 1, first traversal: find checkpoint pass1_bitpos/lastcomp/repcount
	 * and where pass 1 ends (== where pass 2 begins) */
	repcount = 0;
	lastcomp = 0;
	checkpoint_count = 0;
	for (hunknum = 0; hunknum < header->hunkcount; hunknum++)
	{
		if (hunknum % LOWRAM_TARGET_CHECKPOINT_STRIDE == 0)
		{
			checkpoints[checkpoint_count].hunknum = hunknum;
			checkpoints[checkpoint_count].pass1_bitpos = v5_build_stream_position_bits(&pass1);
			checkpoints[checkpoint_count].lastcomp = lastcomp;
			checkpoints[checkpoint_count].repcount = repcount;
			checkpoint_count++;
		}

		if ((err = v5_build_stream_ensure(&pass1)) != CHDERR_NONE)
		{
			free(checkpoints);
			delete_huffman_decoder(decoder);
			return err;
		}

		if (repcount > 0)
			repcount--;
		else
		{
			uint8_t val;
			if (bitstream_overflow(&pass1.bs))
			{
				free(checkpoints);
				delete_huffman_decoder(decoder);
				return CHDERR_DECOMPRESSION_ERROR;
			}

			val = huffman_decode_one(decoder, &pass1.bs);
			if (val == COMPRESSION_RLE_SMALL)
				repcount = 2 + huffman_decode_one(decoder, &pass1.bs);
			else if (val == COMPRESSION_RLE_LARGE)
				repcount = 2 + 16 + (huffman_decode_one(decoder, &pass1.bs) << 4), repcount += huffman_decode_one(decoder, &pass1.bs);
			else
				lastcomp = val;
		}
	}
	chd->lowram_map.pass1_end_bitpos = v5_build_stream_position_bits(&pass1);

	/* second traversal: real pass 2, plus a pass-1 "mirror" run alongside it
	 * (same algorithm, fresh state) to regenerate each hunk's type without
	 * having stored it */
	if ((err = v5_build_stream_init(&pass1_mirror, chd, file_base, mapbytes, pass1_symbols_start_bits / 8)) != CHDERR_NONE)
	{
		free(checkpoints);
		delete_huffman_decoder(decoder);
		return err;
	}
	if (pass1_symbols_start_bits % 8)
		bitstream_read(&pass1_mirror.bs, (int)(pass1_symbols_start_bits % 8));

	if ((err = v5_build_stream_init(&pass2, chd, file_base, mapbytes, chd->lowram_map.pass1_end_bitpos / 8)) != CHDERR_NONE)
	{
		free(checkpoints);
		delete_huffman_decoder(decoder);
		return err;
	}
	if (chd->lowram_map.pass1_end_bitpos % 8)
		bitstream_read(&pass2.bs, (int)(chd->lowram_map.pass1_end_bitpos % 8));

	repcount = 0;
	lastcomp = 0;
	curoffset = firstoffs;
	last_self = 0;
	last_parent = 0;
	checkpoint_count = 0;
	for (hunknum = 0; hunknum < header->hunkcount; hunknum++)
	{
		uint8_t type;
		uint64_t offset;
		uint32_t length = 0;
		uint16_t crc = 0;
		uint8_t entry[12];

		if (hunknum % LOWRAM_TARGET_CHECKPOINT_STRIDE == 0)
		{
			checkpoints[checkpoint_count].pass2_bitpos = v5_build_stream_position_bits(&pass2);
			checkpoints[checkpoint_count].curoffset = curoffset;
			checkpoints[checkpoint_count].last_self = last_self;
			checkpoints[checkpoint_count].last_parent = last_parent;
			checkpoint_count++;
		}

		if ((err = v5_build_stream_ensure(&pass1_mirror)) != CHDERR_NONE || (err = v5_build_stream_ensure(&pass2)) != CHDERR_NONE)
		{
			free(checkpoints);
			delete_huffman_decoder(decoder);
			return err;
		}

		/* pass-1 mirror: regenerate this hunk's type */
		if (repcount > 0)
			type = lastcomp, repcount--;
		else
		{
			uint8_t val;
			if (bitstream_overflow(&pass1_mirror.bs))
			{
				free(checkpoints);
				delete_huffman_decoder(decoder);
				return CHDERR_DECOMPRESSION_ERROR;
			}

			val = huffman_decode_one(decoder, &pass1_mirror.bs);
			if (val == COMPRESSION_RLE_SMALL)
				type = lastcomp, repcount = 2 + huffman_decode_one(decoder, &pass1_mirror.bs);
			else if (val == COMPRESSION_RLE_LARGE)
				type = lastcomp, repcount = 2 + 16 + (huffman_decode_one(decoder, &pass1_mirror.bs) << 4), repcount += huffman_decode_one(decoder, &pass1_mirror.bs);
			else
				type = lastcomp = val;
		}

		/* pass 2: this hunk's length/offset/crc */
		offset = curoffset;
		switch (type)
		{
			case COMPRESSION_TYPE_0:
			case COMPRESSION_TYPE_1:
			case COMPRESSION_TYPE_2:
			case COMPRESSION_TYPE_3:
				curoffset += length = bitstream_read(&pass2.bs, lengthbits);
				crc = bitstream_read(&pass2.bs, 16);
				break;

			case COMPRESSION_NONE:
				curoffset += length = header->hunkbytes;
				crc = bitstream_read(&pass2.bs, 16);
				break;

			case COMPRESSION_SELF:
				last_self = offset = bitstream_read(&pass2.bs, selfbits);
				break;

			case COMPRESSION_PARENT:
				offset = bitstream_read(&pass2.bs, parentbits);
				last_parent = offset;
				break;

			case COMPRESSION_SELF_1:
				last_self++;
				/* Fallthrough */
			case COMPRESSION_SELF_0:
				type = COMPRESSION_SELF;
				offset = last_self;
				break;

			case COMPRESSION_PARENT_SELF:
				type = COMPRESSION_PARENT;
				last_parent = offset = ( ((uint64_t)hunknum) * ((uint64_t)header->hunkbytes) ) / header->unitbytes;
				break;

			case COMPRESSION_PARENT_1:
				last_parent += header->hunkbytes / header->unitbytes;
				/* Fallthrough */
			case COMPRESSION_PARENT_0:
				type = COMPRESSION_PARENT;
				offset = last_parent;
				break;
		}

		entry[0] = type;
		put_bigendian_uint24(&entry[1], length);
		put_bigendian_uint48(&entry[4], offset);
		put_bigendian_uint16(&entry[10], crc);
		running_crc = crc16_update(running_crc, entry, 12);
	}

	if (running_crc != mapcrc)
	{
		free(checkpoints);
		delete_huffman_decoder(decoder);
		return CHDERR_DECOMPRESSION_ERROR;
	}

	chd->lowram_map.checkpoints = checkpoints;
	chd->lowram_map.checkpoint_count = checkpoint_count;
	chd->lowram_map.decoder = decoder;
	return CHDERR_NONE;
}

/*-------------------------------------------------
    ensure_v5_map_window - make sure `win` holds the
    compressed-map bytes [byte_start, byte_end) for
    checkpoint `cpidx`. A no-op if it already does
    (the common case for sequential access - most
    lookups stay within the same checkpoint bucket,
    up to LOWRAM_TARGET_CHECKPOINT_STRIDE hunks); re-reads
    just that small range from the file otherwise.
-------------------------------------------------*/

static chd_error ensure_v5_map_window(chd_file *chd, v5_map_window *win, uint64_t byte_start, uint64_t byte_end, uint32_t cpidx)
{
	uint32_t byte_len;
	uint8_t *data;

	if (win->checkpoint_idx == cpidx && win->data != NULL)
		return CHDERR_NONE;

	if (byte_end > chd->lowram_map.mapbytes)
		byte_end = chd->lowram_map.mapbytes;
	if (byte_end < byte_start)
		byte_end = byte_start;
	byte_len = (uint32_t)(byte_end - byte_start);

	data = (uint8_t*)malloc(byte_len ? byte_len : 1);
	if (data == NULL)
		return CHDERR_OUT_OF_MEMORY;

	if (byte_len != 0 && !seek_and_read(chd, chd->header.mapoffset + 16 + byte_start, data, byte_len))
	{
		free(data);
		return CHDERR_READ_ERROR;
	}

	if (win->data != NULL)
		free(win->data);
	win->data = data;
	win->byte_start = (uint32_t)byte_start;
	win->byte_len = byte_len;
	win->checkpoint_idx = cpidx;
	return CHDERR_NONE;
}

/*-------------------------------------------------
    v5_map_get_entry - re-derive a single v5 map
    entry on demand from the nearest checkpoint,
    mirroring decompress_v5_map's algorithm exactly
    (just resumed instead of starting at hunk 0)
-------------------------------------------------*/

static chd_error v5_map_get_entry(chd_file *chd, uint32_t hunknum, uint8_t out[12])
{
	v5_lowram_map *lm = &chd->lowram_map;
	chd_header *header = &chd->header;
	struct bitstream bs1, bs2;
	uint32_t cur_hunk, start_hunk, next_boundary;
	uint8_t lastcomp;
	int32_t repcount;
	uint64_t curoffset;
	uint32_t last_self;
	uint64_t last_parent;
	uint32_t cpidx, i;

	if (!chd_compressed(header))
	{
		/* uncompressed v5 map: fixed-width entries, but only mapentrybytes (4)
		 * wide here, not 12 - see map_size_v5()/header_read()'s
		 * `mapentrybytes = chd_compressed(header) ? 12 : 4`. */
		if (!seek_and_read(chd, header->mapoffset + (uint64_t)hunknum * header->mapentrybytes, out, header->mapentrybytes))
			return CHDERR_READ_ERROR;
		return CHDERR_NONE;
	}

	if (hunknum >= header->hunkcount || lm->checkpoint_count == 0)
		return CHDERR_INVALID_PARAMETER;

	if (lm->resume.valid && lm->resume.cur_hunk == hunknum && hunknum < lm->resume.next_boundary)
	{
		/* fast path: the previous call already decoded up to hunknum-1 and
		 * left this exact state ready to continue from hunknum - skip the
		 * checkpoint lookup and window setup entirely. Safe to reuse
		 * bs1/bs2 as-is: nothing reallocates pass1_window/pass2_window
		 * between one call finishing and the next one starting, since this
		 * path never calls ensure_v5_map_window(). */
		bs1 = lm->resume.bs1;
		bs2 = lm->resume.bs2;
		lastcomp = lm->resume.lastcomp;
		repcount = lm->resume.repcount;
		curoffset = lm->resume.curoffset;
		last_self = lm->resume.last_self;
		last_parent = lm->resume.last_parent;
		start_hunk = hunknum;
		next_boundary = lm->resume.next_boundary;
	}
	else
	{
		/* checkpoints are hunknum-ascending; find the nearest one <= hunknum */
		cpidx = 0;
		for (i = 1; i < lm->checkpoint_count && lm->checkpoints[i].hunknum <= hunknum; i++)
			cpidx = i;

		{
			uint64_t pass1_start_bit = lm->checkpoints[cpidx].pass1_bitpos;
			uint64_t pass1_end_bit = (cpidx + 1 < lm->checkpoint_count) ?
				lm->checkpoints[cpidx + 1].pass1_bitpos : lm->pass1_end_bitpos;
			uint64_t pass2_start_bit = lm->checkpoints[cpidx].pass2_bitpos;
			uint64_t pass2_end_bit = (cpidx + 1 < lm->checkpoint_count) ?
				lm->checkpoints[cpidx + 1].pass2_bitpos : (uint64_t)lm->mapbytes * 8;
			chd_error werr;

			if ((werr = ensure_v5_map_window(chd, &lm->pass1_window, pass1_start_bit / 8,
					(pass1_end_bit + 7) / 8, cpidx)) != CHDERR_NONE)
				return werr;
			if ((werr = ensure_v5_map_window(chd, &lm->pass2_window, pass2_start_bit / 8,
					(pass2_end_bit + 7) / 8, cpidx)) != CHDERR_NONE)
				return werr;

			bs1.buffer = 0; bs1.bits = 0; bs1.read = lm->pass1_window.data; bs1.doffset = 0;
			bs1.dlength = lm->pass1_window.byte_len;
			bitstream_seek_bits(&bs1, pass1_start_bit - (uint64_t)lm->pass1_window.byte_start * 8);

			bs2.buffer = 0; bs2.bits = 0; bs2.read = lm->pass2_window.data; bs2.doffset = 0;
			bs2.dlength = lm->pass2_window.byte_len;
			bitstream_seek_bits(&bs2, pass2_start_bit - (uint64_t)lm->pass2_window.byte_start * 8);
		}

		lastcomp = lm->checkpoints[cpidx].lastcomp;
		repcount = lm->checkpoints[cpidx].repcount;
		curoffset = lm->checkpoints[cpidx].curoffset;
		last_self = lm->checkpoints[cpidx].last_self;
		last_parent = lm->checkpoints[cpidx].last_parent;
		start_hunk = lm->checkpoints[cpidx].hunknum;
		next_boundary = (cpidx + 1 < lm->checkpoint_count) ? lm->checkpoints[cpidx + 1].hunknum : header->hunkcount;
	}

	for (cur_hunk = start_hunk; cur_hunk <= hunknum; cur_hunk++)
	{
		uint8_t type;
		uint64_t offset;
		uint32_t length = 0;
		uint16_t crc = 0;

		/* pass 1: this hunk's compression-type byte */
		if (repcount > 0)
			type = lastcomp, repcount--;
		else
		{
			uint8_t val;
			if (bitstream_overflow(&bs1))
				return CHDERR_DECOMPRESSION_ERROR;

			val = huffman_decode_one(lm->decoder, &bs1);
			if (val == COMPRESSION_RLE_SMALL)
				type = lastcomp, repcount = 2 + huffman_decode_one(lm->decoder, &bs1);
			else if (val == COMPRESSION_RLE_LARGE)
				type = lastcomp, repcount = 2 + 16 + (huffman_decode_one(lm->decoder, &bs1) << 4), repcount += huffman_decode_one(lm->decoder, &bs1);
			else
				type = lastcomp = val;
		}

		/* pass 2: this hunk's length/offset/crc */
		offset = curoffset;
		switch (type)
		{
			case COMPRESSION_TYPE_0:
			case COMPRESSION_TYPE_1:
			case COMPRESSION_TYPE_2:
			case COMPRESSION_TYPE_3:
				curoffset += length = bitstream_read(&bs2, lm->lengthbits);
				crc = bitstream_read(&bs2, 16);
				break;

			case COMPRESSION_NONE:
				curoffset += length = header->hunkbytes;
				crc = bitstream_read(&bs2, 16);
				break;

			case COMPRESSION_SELF:
				last_self = offset = bitstream_read(&bs2, lm->selfbits);
				break;

			case COMPRESSION_PARENT:
				offset = bitstream_read(&bs2, lm->parentbits);
				last_parent = offset;
				break;

			case COMPRESSION_SELF_1:
				last_self++;
				/* Fallthrough */
			case COMPRESSION_SELF_0:
				type = COMPRESSION_SELF;
				offset = last_self;
				break;

			case COMPRESSION_PARENT_SELF:
				type = COMPRESSION_PARENT;
				last_parent = offset = ( ((uint64_t)cur_hunk) * ((uint64_t)header->hunkbytes) ) / header->unitbytes;
				break;

			case COMPRESSION_PARENT_1:
				last_parent += header->hunkbytes / header->unitbytes;
				/* Fallthrough */
			case COMPRESSION_PARENT_0:
				type = COMPRESSION_PARENT;
				offset = last_parent;
				break;
		}

		if (cur_hunk == hunknum)
		{
			out[0] = type;
			put_bigendian_uint24(&out[1], length);
			put_bigendian_uint48(&out[4], offset);
			put_bigendian_uint16(&out[10], crc);
		}
	}

	/* cache resumable state for a potential hunknum+1 fast-path call. Only
	 * reached after a fully successful decode - any early error return
	 * above leaves the previous cache entry untouched. */
	lm->resume.valid = 1;
	lm->resume.cur_hunk = hunknum + 1;
	lm->resume.next_boundary = next_boundary;
	lm->resume.bs1 = bs1;
	lm->resume.bs2 = bs2;
	lm->resume.lastcomp = lastcomp;
	lm->resume.repcount = repcount;
	lm->resume.curoffset = curoffset;
	lm->resume.last_self = last_self;
	lm->resume.last_parent = last_parent;

	return CHDERR_NONE;
}

/*-------------------------------------------------
    ensure_codec_ready - lazily init() a CHDv5
    codec slot the first time a hunk actually
    needs it, instead of eagerly init()ing every
    codec the header lists as a candidate. A CHD
    encoded with N alternate codecs (chdman picks
    the best per hunk) would otherwise pay for all
    N codecs' dictionaries/windows simultaneously
    even though any single hunk only ever uses one.
-------------------------------------------------*/

static chd_error ensure_codec_ready(chd_file *chd, size_t slot, void *codec)
{
	size_t j;

	if (chd->codec_lazy_initialized[slot])
		return CHDERR_NONE;

	/* an earlier slot referencing the same codec_interface (and therefore
	 * the same underlying codec_data struct) may already be initialized -
	 * mirrors the dedup check chd_open_core_file_callbacks() does eagerly */
	for (j = 0; j < slot; j++)
	{
		if (chd->codecintf[slot] == chd->codecintf[j] && chd->codec_lazy_initialized[j])
		{
			chd->codec_lazy_initialized[slot] = 1;
			return CHDERR_NONE;
		}
	}

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
	{
		chd_error lerr = cd_lend_subcode_inflater(chd, chd->codecintf[slot]->compression, codec);
		if (lerr != CHDERR_NONE)
			return lerr;
	}
#endif

	if (chd->codecintf[slot]->init != NULL)
	{
		chd_error err = chd->codecintf[slot]->init(codec, chd->header.hunkbytes);
		if (err != CHDERR_NONE)
			return err;
	}

	chd->codec_lazy_initialized[slot] = 1;
	return CHDERR_NONE;
}
#endif /* LOWRAM_TARGET */

/*-------------------------------------------------
    map_extract_old - extract a single map
    entry in old format from the datastream
-------------------------------------------------*/

static CHDR_INLINE void map_extract_old(const uint8_t *base, map_entry *entry, uint32_t hunkbytes)
{
	entry->offset = get_bigendian_uint64_t(&base[0]);
	entry->crc = 0;
	entry->length = entry->offset >> 44;
	entry->flags = MAP_ENTRY_FLAG_NO_CRC | ((entry->length == hunkbytes) ? V34_MAP_ENTRY_TYPE_UNCOMPRESSED : V34_MAP_ENTRY_TYPE_COMPRESSED);
#ifdef __MWERKS__
	entry->offset = entry->offset & 0x00000FFFFFFFFFFFLL;
#else
	entry->offset = (entry->offset << 20) >> 20;
#endif
}

/***************************************************************************
    CHD FILE MANAGEMENT
***************************************************************************/

static const core_file_callbacks core_stdio = {
	core_stdio_fsize,
	core_stdio_fread,
	core_stdio_fclose,
	core_stdio_fseek
};

static const core_file_callbacks core_stdio_nonowner = {
	core_stdio_fsize,
	core_stdio_fread,
	core_stdio_fclose_nonowner,
	core_stdio_fseek
};

static const core_file_callbacks core_legacy = {
	core_legacy_fsize,
	core_legacy_fread,
	core_legacy_fclose,
	core_legacy_fseek
};

/*-------------------------------------------------
    chd_open_file - open a CHD file for access
-------------------------------------------------*/

CHD_EXPORT chd_error chd_open_file(FILE *file, int mode, chd_file *parent, chd_file **chd) {
	return chd_open_core_file_callbacks(&core_stdio_nonowner, file, mode, parent, chd);
}

/*-------------------------------------------------
    chd_open_core_file - open a CHD file for access
-------------------------------------------------*/

CHD_EXPORT chd_error chd_open_core_file(core_file *file, int mode, chd_file *parent, chd_file **chd)
{
	if (file == NULL)
		return CHDERR_INVALID_PARAMETER;

	return chd_open_core_file_callbacks(&core_legacy, file, mode, parent, chd);
}

/*-------------------------------------------------
    chd_open_core_file_callbacks - open a CHD file for access
-------------------------------------------------*/

CHD_EXPORT chd_error chd_open_core_file_callbacks(const core_file_callbacks *callbacks, const void *user_data, int mode, chd_file *parent, chd_file **chd)
{
	chd_file *newchd = NULL;
	chd_error err;

	/* The file and the parent belong to us from here on, whether the open
	 * succeeds or not: every failure closes both, the way chd_close() of the
	 * new handle would. Callers depend on that - they retry a child that
	 * reported CHDERR_REQUIRES_PARENT with a fresh file, and never close the
	 * parent they passed in. The exits before newchd exists have to do it by
	 * hand. */

	/* verify parameters; with no callbacks there is nothing to close it with */
	if (callbacks == NULL)
		return CHDERR_INVALID_PARAMETER;

	/* punt if invalid parent - which is not a chd_file, so not ours to close */
	if (parent != NULL && parent->cookie != COOKIE_VALUE)
	{
		callbacks->fclose((void *)user_data);
		return CHDERR_INVALID_PARAMETER;
	}

	/* allocate memory for the final result */
	newchd = (chd_file *)malloc(sizeof(**chd));
	if (newchd == NULL)
	{
		callbacks->fclose((void *)user_data);
		chd_close(parent);
		return CHDERR_OUT_OF_MEMORY;
	}
	memset(newchd, 0, sizeof(*newchd));
	newchd->cookie = COOKIE_VALUE;
	newchd->parent = parent;
	newchd->file.callbacks = callbacks;
	newchd->file.argp = (void*)user_data;
	newchd->file_size = core_fsize(&newchd->file);
	if ((int64_t)newchd->file_size <= 0)
		EARLY_EXIT(err = CHDERR_INVALID_FILE);

	/* now attempt to read the header */
	err = header_read(newchd);
	if (err != CHDERR_NONE)
		EARLY_EXIT(err);

	/* make sure we don't open a read-only file writeable */
	if (mode == CHD_OPEN_READWRITE && !(newchd->header.flags & CHDFLAGS_IS_WRITEABLE))
		EARLY_EXIT(err = CHDERR_FILE_NOT_WRITEABLE);

	/* also, never open an older version writeable */
	if (mode == CHD_OPEN_READWRITE && newchd->header.version < CHD_HEADER_VERSION)
		EARLY_EXIT(err = CHDERR_UNSUPPORTED_VERSION);

	/* if we need a parent, make sure we have one */
	if (parent == NULL)
	{
		/* Detect parent requirement for versions below 5 */
		if (newchd->header.version < 5 && newchd->header.flags & CHDFLAGS_HAS_PARENT)
			EARLY_EXIT(err = CHDERR_REQUIRES_PARENT);
		/* Detection for version 5 and above - if parentsha1 != 0, we have a parent */
		else if (newchd->header.version >= 5 && memcmp(nullsha1, newchd->header.parentsha1, sizeof(newchd->header.parentsha1)) != 0)
			EARLY_EXIT(err = CHDERR_REQUIRES_PARENT);
	}

	/* make sure we have a valid parent */
	if (parent != NULL)
	{
		/* check MD5 if it isn't empty */
		if (memcmp(nullmd5, newchd->header.parentmd5, sizeof(newchd->header.parentmd5)) != 0 &&
			memcmp(nullmd5, newchd->parent->header.md5, sizeof(newchd->parent->header.md5)) != 0 &&
			memcmp(newchd->parent->header.md5, newchd->header.parentmd5, sizeof(newchd->header.parentmd5)) != 0)
			EARLY_EXIT(err = CHDERR_INVALID_PARENT);

		/* check SHA1 if it isn't empty */
		if (memcmp(nullsha1, newchd->header.parentsha1, sizeof(newchd->header.parentsha1)) != 0 &&
			memcmp(nullsha1, newchd->parent->header.sha1, sizeof(newchd->parent->header.sha1)) != 0 &&
			memcmp(newchd->parent->header.sha1, newchd->header.parentsha1, sizeof(newchd->header.parentsha1)) != 0)
			EARLY_EXIT(err = CHDERR_INVALID_PARENT);
	}

	/* now read the hunk map */
	if (newchd->header.version < 5)
	{
		err = map_read(newchd);
		if (err != CHDERR_NONE)
			EARLY_EXIT(err);
	}
	else
	{
		err = decompress_v5_map(newchd, &(newchd->header));
	}
	if (err != CHDERR_NONE)
		EARLY_EXIT(err);

#if LOWRAM_TARGET
	/* grown on demand in hunk_read_compressed() instead of preallocated to
	 * the worst case (header.hunkbytes) here - see compressed_capacity. */
	newchd->compressed = NULL;
	newchd->compressed_capacity = 0;
#else
	/* allocate the temporary compressed buffer */
	newchd->compressed = (uint8_t *)malloc(newchd->header.hunkbytes);
	if (newchd->compressed == NULL)
		EARLY_EXIT(err = CHDERR_OUT_OF_MEMORY);
#endif

	/* find the codec interface */
	if (newchd->header.version < 5)
	{
		size_t intfnum;
		for (intfnum = 0; intfnum < ARRAY_LENGTH(codec_interfaces); intfnum++)
		{
			if (codec_interfaces[intfnum].compression == newchd->header.compression[0])
			{
				newchd->codecintf[0] = &codec_interfaces[intfnum];
				break;
			}
		}

		if (intfnum == ARRAY_LENGTH(codec_interfaces))
			EARLY_EXIT(err = CHDERR_UNSUPPORTED_FORMAT);

		/* initialize the codec */
		if (newchd->codecintf[0]->init != NULL)
		{
			/* v1-v4 codecs that use their own state blob; zlib is the default */
			void* codec = &newchd->codec_data.zlib;
			if (newchd->header.compression[0] == CHDCOMPRESSION_AV)
				codec = &newchd->codec_data.avhuff;
			err = newchd->codecintf[0]->init(codec, newchd->header.hunkbytes);
			if (err != CHDERR_NONE)
				EARLY_EXIT(err);
		}
	}
	else
	{
		size_t decompnum;
#if !LOWRAM_TARGET
		int needsinit;
#endif

		/* verify the compression types and initialize the codecs */
		for (decompnum = 0; decompnum < ARRAY_LENGTH(newchd->header.compression); decompnum++)
		{
			size_t i;
			for (i = 0 ; i < ARRAY_LENGTH(codec_interfaces) ; i++)
			{
				if (codec_interfaces[i].compression == newchd->header.compression[decompnum])
				{
					newchd->codecintf[decompnum] = &codec_interfaces[i];
					break;
				}
			}

			if (newchd->codecintf[decompnum] == NULL && newchd->header.compression[decompnum] != 0)
				EARLY_EXIT(err = CHDERR_UNSUPPORTED_FORMAT);

#if !LOWRAM_TARGET
			/* ensure we don't try to initialize the same codec twice */
			/* this is "normal" for chds where the user overrides the codecs, it'll have none repeated */
			needsinit = (newchd->codecintf[decompnum]->init != NULL);
			for (i = 0; i < decompnum; i++)
			{
				if (newchd->codecintf[decompnum] == newchd->codecintf[i])
				{
					/* already initialized */
					needsinit = FALSE;
					break;
				}
      }

			/* initialize the codec */
			if (needsinit)
			{
				void* codec = NULL;
				switch (newchd->header.compression[decompnum])
				{
					case CHD_CODEC_ZLIB:
						codec = &newchd->codec_data.zlib;
						break;

					case CHD_CODEC_LZMA:
						codec = &newchd->codec_data.lzma;
						break;

					case CHD_CODEC_HUFFMAN:
						codec = &newchd->codec_data.huff;
						break;

					case CHD_CODEC_FLAC:
						codec = &newchd->codec_data.flac;
						break;

					case CHD_CODEC_ZSTD:
						codec = &newchd->codec_data.zstd;
						break;

					case CHD_CODEC_CD_ZLIB:
						codec = &newchd->codec_data.cdzl;
						break;

					case CHD_CODEC_CD_LZMA:
						codec = &newchd->codec_data.cdlz;
						break;

					case CHD_CODEC_CD_FLAC:
						codec = &newchd->codec_data.cdfl;
						break;

					case CHD_CODEC_CD_ZSTD:
						codec = &newchd->codec_data.cdzs;
						break;

					case CHD_CODEC_AVHUFF:
						codec = &newchd->codec_data.avhuff;
						break;
				}

				if (codec == NULL)
					EARLY_EXIT(err = CHDERR_UNSUPPORTED_FORMAT);

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
				err = cd_lend_subcode_inflater(newchd,
					newchd->header.compression[decompnum], codec);
				if (err != CHDERR_NONE)
					EARLY_EXIT(err);
#endif

				err = newchd->codecintf[decompnum]->init(codec, newchd->header.hunkbytes);
				if (err != CHDERR_NONE)
					EARLY_EXIT(err);
			}
#else
			/* a CHDv5 can list up to 4 alternate codecs; chdman picks the
			 * best per hunk, so any hunk might need any of them. Eagerly
			 * init()ing all of them means paying for every one of their
			 * dictionaries/windows simultaneously, even though a given
			 * hunk only ever uses one. codecintf[decompnum] is still
			 * resolved above (cheap - just a tag lookup); init() itself is
			 * deferred to ensure_codec_ready(), called from
			 * hunk_read_into_memory() the first time a hunk actually
			 * selects this slot. */
#endif
		}
	}

	/* all done */
	*chd = newchd;
	return CHDERR_NONE;

cleanup:
	/* closes the file and the parent along with it */
	chd_close(newchd);
	return err;
}

/*-------------------------------------------------
    chd_set_cache_budget - give libchdr a memory
    budget to spend on internal caching, or 0 to
    disable it (the default)
-------------------------------------------------*/

CHD_EXPORT chd_error chd_set_cache_budget(chd_file *chd, size_t bytes)
{
	uint8_t *buf;

	if (chd == NULL)
		return CHDERR_INVALID_PARAMETER;

	/* releasing is always possible */
	if (chd->ra_buf != NULL)
	{
		free(chd->ra_buf);
		chd->ra_buf = NULL;
		chd->ra_capacity = chd->ra_valid = 0;
		chd->ra_off = 0;
	}
	if (bytes == 0)
		return CHDERR_NONE;

	/* The budget is a ceiling, not a target: never allocate more than the
	 * caller allowed. A window smaller than one hunk cannot serve a read, so
	 * such a budget leaves caching off rather than silently exceeding it -
	 * which matters because hunkbytes varies enormously (19,584 for a CD,
	 * 223,668 for AVHuff), so a fixed budget that rounded up would allocate
	 * over ten times what was asked on some images. chd_get_cache_budget()
	 * reports what was actually taken. */
	if (bytes < chd->header.hunkbytes)
		return CHDERR_NONE;

	buf = (uint8_t *)malloc(bytes);
	if (buf == NULL)
		return CHDERR_OUT_OF_MEMORY;   /* caching off; the file stays usable */

	chd->ra_buf = buf;
	chd->ra_capacity = bytes;
	chd->ra_valid = 0;
	chd->ra_off = 0;
	chd->ra_hits = chd->ra_misses = 0;
	return CHDERR_NONE;
}

CHD_EXPORT size_t chd_get_cache_budget(const chd_file *chd)
{
	return (chd != NULL) ? chd->ra_capacity : 0;
}

CHD_EXPORT void chd_get_cache_stats(const chd_file *chd, uint64_t *hits, uint64_t *misses)
{
	if (hits != NULL)   *hits   = (chd != NULL) ? chd->ra_hits : 0;
	if (misses != NULL) *misses = (chd != NULL) ? chd->ra_misses : 0;
}

/*-------------------------------------------------
    chd_precache - precache underlying file in
    memory
-------------------------------------------------*/

CHD_EXPORT chd_error chd_precache(chd_file *chd)
{
	if (chd->file_cache == NULL)
	{
		chd->file_cache = (uint8_t*)malloc(chd->file_size);
		if (chd->file_cache == NULL)
			return CHDERR_OUT_OF_MEMORY;
		if (!seek_and_read(chd, 0, chd->file_cache, chd->file_size))
		{
			free(chd->file_cache);
			chd->file_cache = NULL;
			return CHDERR_READ_ERROR;
		}
	}

	return CHDERR_NONE;
}

/*-------------------------------------------------
    chd_open - open a CHD file by
    filename
-------------------------------------------------*/

CHD_EXPORT chd_error chd_open(const char *filename, int mode, chd_file *parent, chd_file **chd)
{
	chd_error err;
	void *file = NULL;

	if (filename == NULL)
		EARLY_EXIT(err = CHDERR_INVALID_PARAMETER);

	/* choose the proper mode */
	switch(mode)
	{
		case CHD_OPEN_READ:
			break;

		default:
			EARLY_EXIT(err = CHDERR_INVALID_PARAMETER);
	}

	/* open the file */
	file = core_stdio_fopen(filename);
	if (file == NULL)
		EARLY_EXIT(err = CHDERR_FILE_NOT_FOUND);

	/* now open the CHD */
	return chd_open_core_file_callbacks(&core_stdio, file, mode, parent, chd);

cleanup:
	if ((err != CHDERR_NONE) && (file != NULL))
		core_stdio_fclose(file);
	return err;
}

/*-------------------------------------------------
    chd_close - close a CHD file for access
-------------------------------------------------*/

CHD_EXPORT void chd_close(chd_file *chd)
{
	/* punt if NULL or invalid */
	if (chd == NULL || chd->cookie != COOKIE_VALUE)
		return;

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
	/* The CD codecs only borrowed this one; free it here, once, after they
	 * have all dropped their pointers to it. */
	zlib_codec_free(&chd->subcode_shared);
#endif

	/* deinit the codec */
	if (chd->header.version < 5)
	{
		if (chd->codecintf[0] != NULL && chd->codecintf[0]->free != NULL)
		{
			void *codec = &chd->codec_data.zlib;
			if (chd->header.compression[0] == CHDCOMPRESSION_AV)
				codec = &chd->codec_data.avhuff;
			chd->codecintf[0]->free(codec);
		}
	}
	else
	{
		size_t i;
		/* Free the codecs */
		for (i = 0 ; i < ARRAY_LENGTH(chd->codecintf); i++)
		{
			void* codec = NULL;
			size_t j;
			int needsfree;

			if (chd->codecintf[i] == NULL)
				continue;

			/* only free each codec at max once */
			needsfree = 1;
			for (j = 0; j < i; j++)
			{
				if (chd->codecintf[i] == chd->codecintf[j])
				{
					needsfree = FALSE;
					break;
				}
			}
			if (!needsfree)
				continue;

			switch (chd->codecintf[i]->compression)
			{
				case CHD_CODEC_ZLIB:
					codec = &chd->codec_data.zlib;
					break;

				case CHD_CODEC_LZMA:
					codec = &chd->codec_data.lzma;
					break;

				case CHD_CODEC_HUFFMAN:
					codec = &chd->codec_data.huff;
					break;

				case CHD_CODEC_FLAC:
					codec = &chd->codec_data.flac;
					break;

				case CHD_CODEC_ZSTD:
					codec = &chd->codec_data.zstd;
					break;

				case CHD_CODEC_CD_ZLIB:
					codec = &chd->codec_data.cdzl;
					break;

				case CHD_CODEC_CD_LZMA:
					codec = &chd->codec_data.cdlz;
					break;

				case CHD_CODEC_CD_FLAC:
					codec = &chd->codec_data.cdfl;
					break;

				case CHD_CODEC_CD_ZSTD:
					codec = &chd->codec_data.cdzs;
					break;

				case CHD_CODEC_AVHUFF:
					codec = &chd->codec_data.avhuff;
					break;
			}

			if (codec)
			{
#if LOWRAM_TARGET
				/* never lazily init()ed (no hunk ever selected this slot) -
				 * most codec free() implementations assume init() ran first */
				if (chd->codec_lazy_initialized[i])
#endif
				chd->codecintf[i]->free(codec);
			}
		}

		/* Free the raw map */
		if (chd->header.rawmap != NULL)
			free(chd->header.rawmap);

#if LOWRAM_TARGET
		if (chd->lowram_map.pass1_window.data != NULL)
			free(chd->lowram_map.pass1_window.data);
		if (chd->lowram_map.pass2_window.data != NULL)
			free(chd->lowram_map.pass2_window.data);
		if (chd->lowram_map.checkpoints != NULL)
			free(chd->lowram_map.checkpoints);
		if (chd->lowram_map.decoder != NULL)
			delete_huffman_decoder(chd->lowram_map.decoder);
#endif
	}

	/* free the compressed data buffer */
	if (chd->compressed != NULL)
		free(chd->compressed);

	/* free the hunk map */
	if (chd->map != NULL)
		free(chd->map);

	/* close the file */
	if (chd->file.callbacks != NULL)
		core_fclose(&chd->file);

	selfcache_free(chd);

	if (chd->ra_buf != NULL)
	{
		free(chd->ra_buf);
		chd->ra_buf = NULL;
		chd->ra_capacity = chd->ra_valid = 0;
	}

	if (chd->file_cache)
		free(chd->file_cache);

	if (chd->parent)
		chd_close(chd->parent);

	/* free our memory */
	free(chd);
}

/*-------------------------------------------------
    chd_core_file - return the associated
    core_file
-------------------------------------------------*/

CHD_EXPORT core_file *chd_core_file(chd_file *chd)
{
	if (chd->file.callbacks != &core_legacy)
		return NULL;

	return (core_file*)chd->file.argp;
}

/*-------------------------------------------------
    chd_error_string - return an error string for
    the given CHD error
-------------------------------------------------*/

CHD_EXPORT const char *chd_error_string(chd_error err)
{
	switch (err)
	{
		case CHDERR_NONE:						return "no error";
		case CHDERR_NO_INTERFACE:				return "no drive interface";
		case CHDERR_OUT_OF_MEMORY:				return "out of memory";
		case CHDERR_INVALID_FILE:				return "invalid file";
		case CHDERR_INVALID_PARAMETER:			return "invalid parameter";
		case CHDERR_INVALID_DATA:				return "invalid data";
		case CHDERR_FILE_NOT_FOUND:				return "file not found";
		case CHDERR_REQUIRES_PARENT:			return "requires parent";
		case CHDERR_FILE_NOT_WRITEABLE:			return "file not writeable";
		case CHDERR_READ_ERROR:					return "read error";
		case CHDERR_WRITE_ERROR:				return "write error";
		case CHDERR_CODEC_ERROR:				return "codec error";
		case CHDERR_INVALID_PARENT:				return "invalid parent";
		case CHDERR_HUNK_OUT_OF_RANGE:			return "hunk out of range";
		case CHDERR_DECOMPRESSION_ERROR:		return "decompression error";
		case CHDERR_COMPRESSION_ERROR:			return "compression error";
		case CHDERR_CANT_CREATE_FILE:			return "can't create file";
		case CHDERR_CANT_VERIFY:				return "can't verify file";
		case CHDERR_NOT_SUPPORTED:				return "operation not supported";
		case CHDERR_METADATA_NOT_FOUND:			return "can't find metadata";
		case CHDERR_INVALID_METADATA_SIZE:		return "invalid metadata size";
		case CHDERR_UNSUPPORTED_VERSION:		return "unsupported CHD version";
		case CHDERR_VERIFY_INCOMPLETE:			return "incomplete verify";
		case CHDERR_INVALID_METADATA:			return "invalid metadata";
		case CHDERR_INVALID_STATE:				return "invalid state";
		case CHDERR_OPERATION_PENDING:			return "operation pending";
		case CHDERR_NO_ASYNC_OPERATION:			return "no async operation in progress";
		case CHDERR_UNSUPPORTED_FORMAT:			return "unsupported format";
		default:								return "undocumented error";
	}
}

/***************************************************************************
    CHD HEADER MANAGEMENT
***************************************************************************/

/*-------------------------------------------------
    chd_get_header - return a pointer to the
    extracted header data
-------------------------------------------------*/

CHD_EXPORT const chd_header *chd_get_header(chd_file *chd)
{
	/* punt if NULL or invalid */
	if (chd == NULL || chd->cookie != COOKIE_VALUE)
		return NULL;

	return &chd->header;
}

/*-------------------------------------------------
    chd_read_header_core_file_callbacks - read CHD header data
	from file into the pointed struct
-------------------------------------------------*/

CHD_EXPORT chd_error chd_read_header_core_file_callbacks(const core_file_callbacks *callbacks, const void *user_data, chd_header *header)
{
	chd_error err;
	chd_file chd;

	/* verify parameters */
	if (callbacks == NULL || header == NULL)
		return CHDERR_INVALID_PARAMETER;

	chd.cookie = COOKIE_VALUE;
	chd.file.callbacks = callbacks;
	chd.file.argp = (void*)user_data;
	chd.file_size = core_fsize(&chd.file);
	if ((int64_t)chd.file_size <= 0)
		return CHDERR_INVALID_FILE;

	/* attempt to read the header */
	err = header_read(&chd);
	if (err != CHDERR_NONE)
		return err;
	*header = chd.header;
	return CHDERR_NONE;
}

/*-------------------------------------------------
    chd_read_header_core_file - read CHD header data
	from file into the pointed struct
-------------------------------------------------*/

CHD_EXPORT chd_error chd_read_header_core_file(core_file *file, chd_header *header)
{
	if (file == NULL)
		return CHDERR_INVALID_PARAMETER;

	return chd_read_header_core_file_callbacks(&core_legacy, file, header);
}

/*-------------------------------------------------
    chd_read_header - read CHD header data
	from file into the pointed struct
-------------------------------------------------*/

CHD_EXPORT chd_error chd_read_header_file(FILE *file, chd_header *header)
{
	return chd_read_header_core_file_callbacks(&core_stdio_nonowner, file, header);
}

/*-------------------------------------------------
    chd_read_header - read CHD header data
	from file into the pointed struct
-------------------------------------------------*/

CHD_EXPORT chd_error chd_read_header(const char *filename, chd_header *header)
{
	chd_error err;
	void *file = NULL;

	if (filename == NULL)
		EARLY_EXIT(err = CHDERR_INVALID_PARAMETER);

	/* open the file */
	file = core_stdio_fopen(filename);
	if (file == NULL)
		EARLY_EXIT(err = CHDERR_FILE_NOT_FOUND);

	err = chd_read_header_core_file_callbacks(&core_stdio, file, header);

	cleanup:
	if (file != NULL)
		core_stdio_fclose(file);
	return err;
}

/***************************************************************************
    CORE DATA READ/WRITE
***************************************************************************/

/*-------------------------------------------------
    chd_read - read a single hunk from the CHD
    file
-------------------------------------------------*/

CHD_EXPORT chd_error chd_read(chd_file *chd, uint32_t hunknum, void *buffer)
{
	/* punt if NULL or invalid */
	if (chd == NULL || chd->cookie != COOKIE_VALUE)
		return CHDERR_INVALID_PARAMETER;

	/* if we're past the end, fail */
	if (hunknum >= chd->header.totalhunks)
		return CHDERR_HUNK_OUT_OF_RANGE;

	/* perform the read */
	return hunk_read_into_memory(chd, hunknum, (uint8_t *)buffer);
}

/***************************************************************************
    METADATA MANAGEMENT
***************************************************************************/

/*-------------------------------------------------
    chd_get_metadata - get the indexed metadata
    of the given type
-------------------------------------------------*/

CHD_EXPORT chd_error chd_get_metadata(chd_file *chd, uint32_t searchtag, uint32_t searchindex, void *output, uint32_t outputlen, uint32_t *resultlen, uint32_t *resulttag, uint8_t *resultflags)
{
	metadata_entry metaentry;
	chd_error err;

	/* if we didn't find it, just return */
	err = metadata_find_entry(chd, searchtag, searchindex, &metaentry);
	if (err != CHDERR_NONE)
	{
		/* unless we're an old version and they are requesting hard disk metadata */
		if (chd->header.version < 3 && (searchtag == HARD_DISK_METADATA_TAG || searchtag == CHDMETATAG_WILDCARD) && searchindex == 0)
		{
			char faux_metadata[256];
			uint32_t faux_length;

			/* fill in the faux metadata */
			sprintf(faux_metadata, HARD_DISK_METADATA_FORMAT, (int)chd->header.obsolete_cylinders, (int)chd->header.obsolete_heads, (int)chd->header.obsolete_sectors, (int)((chd->header.obsolete_hunksize != 0) ? (chd->header.hunkbytes / chd->header.obsolete_hunksize) : 0));
			faux_length = (uint32_t)strlen(faux_metadata) + 1;

			/* copy the metadata itself */
			memcpy(output, faux_metadata, MIN(outputlen, faux_length));

			/* return the length of the data and the tag */
			if (resultlen != NULL)
				*resultlen = faux_length;
			if (resulttag != NULL)
				*resulttag = HARD_DISK_METADATA_TAG;
			return CHDERR_NONE;
		}
		return err;
	}

	/* read the metadata */
	outputlen = MIN(outputlen, metaentry.length);
	if (!seek_and_read(chd, metaentry.offset + METADATA_HEADER_SIZE, output, outputlen))
		return CHDERR_READ_ERROR;

	/* return the length of the data and the tag */
	if (resultlen != NULL)
		*resultlen = metaentry.length;
	if (resulttag != NULL)
		*resulttag = metaentry.metatag;
	if (resultflags != NULL)
		*resultflags = metaentry.flags;
	return CHDERR_NONE;
}

/***************************************************************************
    INTERNAL HEADER OPERATIONS
***************************************************************************/

/*-------------------------------------------------
    header_guess_unitbytes - for older CHD formats,
    guess at the bytes/unit based on metadata
-------------------------------------------------*/

static uint32_t header_guess_unitbytes(chd_file *chd)
{
	/* look for hard disk metadata; if found, then the unit size == sector size */
	char metadata[512];
	int i0, i1, i2, i3;
	if (chd_get_metadata(chd, HARD_DISK_METADATA_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE &&
		sscanf(metadata, HARD_DISK_METADATA_FORMAT, &i0, &i1, &i2, &i3) == 4)
		return i3;

	/* look for CD-ROM metadata; if found, then the unit size == CD frame size */
	if (chd_get_metadata(chd, CDROM_OLD_METADATA_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE ||
		chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE ||
		chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE ||
		chd_get_metadata(chd, GDROM_OLD_METADATA_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE ||
		chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, 0, metadata, sizeof(metadata), NULL, NULL, NULL) == CHDERR_NONE)
		return CD_FRAME_SIZE;

	/* otherwise, just map 1:1 with the hunk size */
	return chd->header.hunkbytes;
}

/*-------------------------------------------------
    header_read - read a CHD header into the
    internal data structure and perform validation
-------------------------------------------------*/

static chd_error header_read(chd_file *chd)
{
	static const uint32_t header_sizes[CHD_HEADER_VERSION] = {
		CHD_V1_HEADER_SIZE,
		CHD_V2_HEADER_SIZE,
		CHD_V3_HEADER_SIZE,
		CHD_V4_HEADER_SIZE,
		CHD_V5_HEADER_SIZE,
	};

	uint8_t rawheader[CHD_MAX_HEADER_SIZE];
	chd_header *header;

	/* punt if NULL */
	if (chd == NULL)
		return CHDERR_INVALID_PARAMETER;

	/* punt if invalid file */
	if (chd->file.callbacks == NULL)
		return CHDERR_INVALID_FILE;

	/* read the start of the header */
	if (!seek_and_read(chd, 0, rawheader, 8 + 4 + 4))
		return CHDERR_READ_ERROR;

	/* verify the tag */
	if (memcmp(rawheader, "MComprHD", 8) != 0)
		return CHDERR_INVALID_DATA;

	/* extract the direct data */
	header = &chd->header;
	memset(header, 0, sizeof(*header));
	header->length  = get_bigendian_uint32_t(&rawheader[8]);
	header->version = get_bigendian_uint32_t(&rawheader[12]);

	/* Unknown version */
	if (header->version == 0 || header->version > ARRAY_LENGTH(header_sizes))
		return CHDERR_UNSUPPORTED_VERSION;

	/* make sure the length is expected */
	if (header->length != header_sizes[header->version - 1])
		return CHDERR_INVALID_DATA;

	/* read the full header, now that we know its size */
	if (!seek_and_read(chd, 0, rawheader, header->length))
		return CHDERR_READ_ERROR;

	switch (header->version)
	{
		default:
			/* Unknown version */
			return CHDERR_UNSUPPORTED_VERSION;

		case 1:
		case 2:
			header->flags              = get_bigendian_uint32_t(&rawheader[16]);
			header->compression[0]     = get_bigendian_uint32_t(&rawheader[20]);
			header->obsolete_hunksize  = get_bigendian_uint32_t(&rawheader[24]);
			header->totalhunks         = get_bigendian_uint32_t(&rawheader[28]);
			header->obsolete_cylinders = get_bigendian_uint32_t(&rawheader[32]);
			header->obsolete_heads     = get_bigendian_uint32_t(&rawheader[36]);
			header->obsolete_sectors   = get_bigendian_uint32_t(&rawheader[40]);
			memcpy(header->md5, &rawheader[44], CHD_MD5_BYTES);
			memcpy(header->parentmd5, &rawheader[60], CHD_MD5_BYTES);
			{
				uint32_t seclen = (header->version == 1) ? CHD_V1_SECTOR_SIZE : get_bigendian_uint32_t(&rawheader[76]);
				uint64_t hunkbytes64 = (uint64_t)seclen * (uint64_t)header->obsolete_hunksize;
				header->logicalbytes = (uint64_t)header->obsolete_cylinders * (uint64_t)header->obsolete_heads * (uint64_t)header->obsolete_sectors * (uint64_t)seclen;
				/* reject malformed headers where hunkbytes would overflow uint32_t or is zero */
				if (hunkbytes64 == 0 || hunkbytes64 > UINT32_MAX)
					return CHDERR_INVALID_DATA;
				header->hunkbytes = (uint32_t)hunkbytes64;
			}
			header->unitbytes          = header_guess_unitbytes(chd);
			if (header->unitbytes == 0)
				return CHDERR_INVALID_DATA;
			header->unitcount          = (header->logicalbytes + header->unitbytes - 1) / header->unitbytes;
			header->metaoffset = 0;

			break;

		case 3:
			header->flags          = get_bigendian_uint32_t(&rawheader[16]);
			header->compression[0] = get_bigendian_uint32_t(&rawheader[20]);
			header->totalhunks     = get_bigendian_uint32_t(&rawheader[24]);
			header->logicalbytes   = get_bigendian_uint64_t(&rawheader[28]);
			header->metaoffset     = get_bigendian_uint64_t(&rawheader[36]);
			memcpy(header->md5, &rawheader[44], CHD_MD5_BYTES);
			memcpy(header->parentmd5, &rawheader[60], CHD_MD5_BYTES);
			header->hunkbytes      = get_bigendian_uint32_t(&rawheader[76]);
			header->unitbytes      = header_guess_unitbytes(chd);
			if (header->unitbytes == 0)
				return CHDERR_INVALID_DATA;
			header->unitcount      = (header->logicalbytes + header->unitbytes - 1) / header->unitbytes;
			memcpy(header->sha1, &rawheader[80], CHD_SHA1_BYTES);
			memcpy(header->parentsha1, &rawheader[100], CHD_SHA1_BYTES);

			break;

		case 4:
			header->flags          = get_bigendian_uint32_t(&rawheader[16]);
			header->compression[0] = get_bigendian_uint32_t(&rawheader[20]);
			header->totalhunks     = get_bigendian_uint32_t(&rawheader[24]);
			header->logicalbytes   = get_bigendian_uint64_t(&rawheader[28]);
			header->metaoffset     = get_bigendian_uint64_t(&rawheader[36]);
			header->hunkbytes      = get_bigendian_uint32_t(&rawheader[44]);
			header->unitbytes      = header_guess_unitbytes(chd);
			if (header->unitbytes == 0)
				return CHDERR_INVALID_DATA;
			header->unitcount      = (header->logicalbytes + header->unitbytes - 1) / header->unitbytes;
			memcpy(header->sha1, &rawheader[48], CHD_SHA1_BYTES);
			memcpy(header->parentsha1, &rawheader[68], CHD_SHA1_BYTES);
			memcpy(header->rawsha1, &rawheader[88], CHD_SHA1_BYTES);

			break;

		case 5:
			header->compression[0] = get_bigendian_uint32_t(&rawheader[16]);
			header->compression[1] = get_bigendian_uint32_t(&rawheader[20]);
			header->compression[2] = get_bigendian_uint32_t(&rawheader[24]);
			header->compression[3] = get_bigendian_uint32_t(&rawheader[28]);
			header->logicalbytes   = get_bigendian_uint64_t(&rawheader[32]);
			header->mapoffset      = get_bigendian_uint64_t(&rawheader[40]);
			header->metaoffset     = get_bigendian_uint64_t(&rawheader[48]);
			header->hunkbytes      = get_bigendian_uint32_t(&rawheader[56]);
			if (header->hunkbytes == 0)
				return CHDERR_INVALID_DATA;
			header->hunkcount      = (header->logicalbytes + header->hunkbytes - 1) / header->hunkbytes;
			header->unitbytes      = get_bigendian_uint32_t(&rawheader[60]);
			if (header->unitbytes == 0)
				return CHDERR_INVALID_DATA;
			header->unitcount      = (header->logicalbytes + header->unitbytes - 1) / header->unitbytes;
			memcpy(header->sha1, &rawheader[84], CHD_SHA1_BYTES);
			memcpy(header->parentsha1, &rawheader[104], CHD_SHA1_BYTES);
			memcpy(header->rawsha1, &rawheader[64], CHD_SHA1_BYTES);

			/* determine properties of map entries */
			header->mapentrybytes  = chd_compressed(header) ? 12 : 4;

			/* hack */
			header->totalhunks     = header->hunkcount;

			break;
	}

	/* Do not validate v5 header */
	if (header->version <= 4)
	{
		size_t intfnum;

		/* require valid flags */
		if (header->flags & CHDFLAGS_UNDEFINED)
			return CHDERR_INVALID_DATA;

		/* require a supported compression mechanism */
		for (intfnum = 0; intfnum < ARRAY_LENGTH(codec_interfaces); intfnum++)
			if (codec_interfaces[intfnum].compression == header->compression[0])
				break;

		if (intfnum == ARRAY_LENGTH(codec_interfaces))
			return CHDERR_INVALID_DATA;

		/* require a valid hunksize */
		if (header->hunkbytes == 0 || header->hunkbytes >= 65536 * 256)
			return CHDERR_INVALID_DATA;

		/* require a valid hunk count */
		if (header->totalhunks == 0)
			return CHDERR_INVALID_DATA;

		/* require a valid MD5 and/or SHA1 if we're using a parent */
		if ((header->flags & CHDFLAGS_HAS_PARENT) && memcmp(header->parentmd5, nullmd5, sizeof(nullmd5)) == 0 && memcmp(header->parentsha1, nullsha1, sizeof(nullsha1)) == 0)
			return CHDERR_INVALID_DATA;

		/* if we're V3 or later, the obsolete fields must be 0 */
		if (header->version >= 3 &&
			(header->obsolete_cylinders != 0 || header->obsolete_sectors != 0 ||
			 header->obsolete_heads != 0 || header->obsolete_hunksize != 0))
			return CHDERR_INVALID_DATA;

		/* if we're pre-V3, the obsolete fields must NOT be 0 */
		if (header->version < 3 &&
			(header->obsolete_cylinders == 0 || header->obsolete_sectors == 0 ||
			 header->obsolete_heads == 0 || header->obsolete_hunksize == 0))
			return CHDERR_INVALID_DATA;
	}

	/* some basic size checks to prevent huge mallocs */
	if (header->hunkbytes >= CHD_MAX_HUNK_SIZE || ((uint64_t)header->hunkbytes * (uint64_t)header->totalhunks) >= CHD_MAX_FILE_SIZE)
		return CHDERR_INVALID_DATA;

	/* totalhunks sizes the map allocation, so a malformed header could
	 * otherwise ask for multi-GB of map[] against a tiny file.
	 *
	 * v1-v4 give an exact bound: their map is a plain array of fixed-size
	 * entries stored right after the header, so it has to fit in the file.
	 * v5 gets no equivalent - its map is run-length and Huffman coded, and a
	 * run of identical, uncompressed or self-referenced hunks costs far less
	 * than one bit each, so a valid sparse image really can carry far more
	 * hunks than its own size in bits. Bound what is actually allocated
	 * instead; see map_size_v5(). */
	if (header->version < 5)
	{
		uint64_t entrysize = (header->version < 3) ? OLD_MAP_ENTRY_SIZE : MAP_ENTRY_SIZE;
		uint64_t mapend = (uint64_t)header->length +
			(uint64_t)header->totalhunks * entrysize;

		if (mapend < (uint64_t)header->length || mapend > chd->file_size)
			return CHDERR_INVALID_DATA;
	}

	/* guess it worked */
	return CHDERR_NONE;
}

/***************************************************************************
    INTERNAL HUNK READ/WRITE
***************************************************************************/

/*-------------------------------------------------
    hunk_read_compressed - read a compressed
    hunk
-------------------------------------------------*/

static uint8_t* hunk_read_compressed(chd_file *chd, uint64_t offset, size_t size)
{
	if (chd->file_cache != NULL)
	{
		if ((offset + size) > chd->file_size || (offset + size) < offset)
			return NULL;
		else
			return chd->file_cache + offset;
	}
	else
	{
		/* make sure it isn't larger than a legitimate hunk could ever be */
		if (size > chd->header.hunkbytes)
			return NULL;

#if LOWRAM_TARGET
		/* grow the scratch buffer to fit this hunk's actual compressed
		 * length instead of always carrying a hunkbytes-sized buffer (see
		 * compressed_capacity). Monotonic: only grows, never shrinks, so a
		 * realloc failure here leaves the existing buffer/capacity intact
		 * and this call simply fails - the chd_file stays consistent. */
		if (size > chd->compressed_capacity)
		{
			uint8_t *grown = (uint8_t*)realloc(chd->compressed, size);
			if (grown == NULL)
				return NULL;
			chd->compressed = grown;
			chd->compressed_capacity = (uint32_t)size;
		}
#endif

		/* Serve from the read-ahead window when the caller has given us a
		 * budget. On a miss, any bytes already held that sit at or after the
		 * requested offset are kept and slid to the front, so the refill
		 * reads only what is genuinely new: sequential access therefore
		 * transfers each byte exactly once, and the window is a pure
		 * reduction in transaction count rather than a trade against
		 * re-reading. A window is never used for a hunk larger than the
		 * window itself. */
		if (chd->ra_buf != NULL && size <= chd->ra_capacity)
		{
			int have = (offset >= chd->ra_off &&
			            offset + size <= chd->ra_off + chd->ra_valid);
			/* Only a forward-progressing miss refills the window. A backward
			 * read - which in practice means a COMPRESSION_SELF reference
			 * reaching back to an earlier hunk - is served directly and
			 * leaves the window untouched, so an excursion cannot throw away
			 * data already prefetched for the sequential stream it will
			 * return to. Without this the window is evicted and refilled
			 * around every self-reference, and the same bytes are fetched
			 * more than once. */
			if (!have && offset < chd->ra_off)
			{
				chd->ra_misses++;
			}
			else if (!have)
			{
				size_t keep = 0;
				size_t want;
				uint64_t fill_at;

				if (offset >= chd->ra_off && offset < chd->ra_off + chd->ra_valid)
				{
					keep = (size_t)(chd->ra_off + chd->ra_valid - offset);
					memmove(chd->ra_buf, chd->ra_buf + (offset - chd->ra_off), keep);
				}
				fill_at = offset + keep;
				want = chd->ra_capacity - keep;
				if (want > 0 && fill_at < chd->file_size)
				{
					uint64_t avail = chd->file_size - fill_at;
					if (avail > (uint64_t)want)
						avail = (uint64_t)want;
					if (seek_and_read(chd, fill_at, chd->ra_buf + keep, (size_t)avail))
					{
						chd->ra_off = offset;
						chd->ra_valid = keep + (size_t)avail;
						chd->ra_misses++;
						have = (offset + size <= chd->ra_off + chd->ra_valid);
					}
					else
					{
						/* a failed refill must not fail the read - drop the
						 * window and fall through to the direct path */
						chd->ra_valid = 0;
						have = 0;
					}
				}
				else
				{
					chd->ra_valid = keep;
					chd->ra_off = offset;
					have = (size <= keep);
				}
			}
			else
				chd->ra_hits++;

			if (have)
			{
				/* copy out rather than handing back a pointer into the
				 * window: the caller holds this across the decompress call,
				 * and a later refill would move the bytes underneath it */
				memcpy(chd->compressed, chd->ra_buf + (offset - chd->ra_off), size);
				return chd->compressed;
			}
		}

		if (!seek_and_read(chd, offset, chd->compressed, size))
			return NULL;
		return chd->compressed;
	}
}

/*-------------------------------------------------
    hunk_read_uncompressed - read an uncompressed
    hunk
-------------------------------------------------*/

static chd_error hunk_read_uncompressed(chd_file *chd, uint64_t offset, size_t size, uint8_t *dest)
{
	if (chd->file_cache != NULL)
	{
		if ((offset + size) > chd->file_size || (offset + size) < offset)
			return CHDERR_READ_ERROR;

		memcpy(dest, chd->file_cache + offset, size);
	}
	else
	{
		/* Uncompressed hunks share the file's sequential layout with the
		 * compressed ones, so they must consume the same read-ahead window -
		 * otherwise the window prefetches their bytes and they then read the
		 * same range again directly, and a file with a meaningful fraction of
		 * uncompressed hunks transfers noticeably more than it needs to. */
		if (chd->ra_buf != NULL && offset >= chd->ra_off &&
		    offset + size <= chd->ra_off + chd->ra_valid)
		{
			memcpy(dest, chd->ra_buf + (offset - chd->ra_off), size);
			chd->ra_hits++;
			return CHDERR_NONE;
		}
		if (!seek_and_read(chd, offset, dest, size))
			return CHDERR_READ_ERROR;
	}
	return CHDERR_NONE;
}

#if WANT_SUBCODE && !defined(CHDR_SYSTEM_ZLIB)
/*-------------------------------------------------
    cd_lend_subcode_inflater - seed a CD codec's
    subcode inflater from the shared one
-------------------------------------------------*/

/* Seeds before the codec's own init runs: zlib_codec_init() keeps an inflater
 * that is already marked borrowed, so nothing is allocated and then thrown
 * away. Ordering-proof, which matters because LOWRAM_TARGET readies codecs
 * lazily and in whatever order the hunks ask for them. */
static chd_error cd_lend_subcode_inflater(chd_file *chd, uint32_t tag, void *codec)
{
	void *sub;

	switch (tag)
	{
		case CHD_CODEC_CD_ZLIB: sub = &((cdzl_codec_data *)codec)->subcode_decompressor; break;
		case CHD_CODEC_CD_LZMA: sub = &((cdlz_codec_data *)codec)->subcode_decompressor; break;
		case CHD_CODEC_CD_FLAC: sub = &((cdfl_codec_data *)codec)->subcode_decompressor; break;
		default: return CHDERR_NONE;
	}

	if (chd->subcode_shared.inflater == NULL)
	{
		chd_error err = zlib_codec_init(&chd->subcode_shared, 0);
		if (err != CHDERR_NONE)
			return err;
	}

	zlib_codec_lend(sub, &chd->subcode_shared);
	return CHDERR_NONE;
}
#endif

/*-------------------------------------------------
    hunk_read_into_memory - read a hunk into
    memory at the given location
-------------------------------------------------*/

static chd_error hunk_read_into_memory(chd_file *chd, uint32_t hunknum, uint8_t *dest)
{
	chd_error err;

	/* punt if no file */
	if (chd->file.callbacks == NULL)
		return CHDERR_INVALID_FILE;

	/* return an error if out of range */
	if (hunknum >= chd->header.totalhunks)
		return CHDERR_HUNK_OUT_OF_RANGE;

	if (dest == NULL)
		return CHDERR_INVALID_PARAMETER;

	if (chd->header.version < 5)
	{
#if LOWRAM_TARGET
		map_entry entry_storage;
		map_entry *entry = &entry_storage;
		if ((err = map_read_one_legacy(chd, hunknum, entry)) != CHDERR_NONE)
			return err;
#else
		map_entry *entry = &chd->map[hunknum];
#endif
		uint32_t bytes;
		uint8_t* compressed_bytes;

		/* v3/v4 map entries carry a CRC32 of the decoded hunk plus a flag to
		 * opt out of it; v1/v2 entries have no room for one and map_extract_old()
		 * sets that flag for them. Checking it is the legacy counterpart of the
		 * CRC16 check the v5 path does below - without it a hunk that decodes
		 * cleanly to the wrong bytes is handed back as valid data. Self- and
		 * parent-referenced entries are covered when the hunk they point at is
		 * read.
		 *
		 * switch off the entry type */
		switch (entry->flags & MAP_ENTRY_FLAG_TYPE_MASK)
		{
			/* compressed data */
			case V34_MAP_ENTRY_TYPE_COMPRESSED:
			{
				void *codec = NULL;

				/* read it into the decompression buffer */
				compressed_bytes = hunk_read_compressed(chd, entry->offset, entry->length);
				if (compressed_bytes == NULL)
					return CHDERR_READ_ERROR;

				/* now decompress using the codec */
				err = CHDERR_NONE;
				codec = &chd->codec_data.zlib;
				if (chd->header.compression[0] == CHDCOMPRESSION_AV)
					codec = &chd->codec_data.avhuff;
				if (chd->codecintf[0]->decompress != NULL)
					err = chd->codecintf[0]->decompress(codec, compressed_bytes, entry->length, dest, chd->header.hunkbytes);
				if (err != CHDERR_NONE)
					return err;
#if VERIFY_BLOCK_CRC
				if (!(entry->flags & MAP_ENTRY_FLAG_NO_CRC) &&
					chd_crc32(dest, chd->header.hunkbytes) != entry->crc)
					return CHDERR_DECOMPRESSION_ERROR;
#endif
				break;
			}

			/* uncompressed data */
			case V34_MAP_ENTRY_TYPE_UNCOMPRESSED:
				err = hunk_read_uncompressed(chd, entry->offset, chd->header.hunkbytes, dest);
				if (err != CHDERR_NONE)
					return err;
#if VERIFY_BLOCK_CRC
				if (!(entry->flags & MAP_ENTRY_FLAG_NO_CRC) &&
					chd_crc32(dest, chd->header.hunkbytes) != entry->crc)
					return CHDERR_DECOMPRESSION_ERROR;
#endif
				break;

			/* mini-compressed data */
			case V34_MAP_ENTRY_TYPE_MINI:
				if (chd->header.hunkbytes < 8)  /* the 8-byte pattern must fit */
					return CHDERR_DECOMPRESSION_ERROR;
				put_bigendian_uint64_t(&dest[0], entry->offset);
				for (bytes = 8; bytes < chd->header.hunkbytes; bytes++)
					dest[bytes] = dest[bytes - 8];
#if VERIFY_BLOCK_CRC
				if (!(entry->flags & MAP_ENTRY_FLAG_NO_CRC) &&
					chd_crc32(dest, chd->header.hunkbytes) != entry->crc)
					return CHDERR_DECOMPRESSION_ERROR;
#endif
				break;

			/* self-referenced data */
			case V34_MAP_ENTRY_TYPE_SELF_HUNK:
				/* only an earlier hunk: a crafted map would recurse forever */
				if (entry->offset >= hunknum)
					return CHDERR_DECOMPRESSION_ERROR;
				return hunk_read_into_memory(chd, entry->offset, dest);

			/* parent-referenced data */
			case V34_MAP_ENTRY_TYPE_PARENT_HUNK:
				if (chd->parent == NULL)  /* as the v5 path checks */
					return CHDERR_REQUIRES_PARENT;
				err = hunk_read_into_memory(chd->parent, entry->offset, dest);
				if (err != CHDERR_NONE)
					return err;
				break;
		}
		return CHDERR_NONE;
	}
	else
	{
		void* codec = NULL;
		/* get a pointer to the map entry */
		uint64_t blockoffs;
		uint32_t blocklen;
#if VERIFY_BLOCK_CRC
		uint16_t blockcrc;
#endif
#if LOWRAM_TARGET
		uint8_t rawmap_storage[12];
		uint8_t *rawmap = rawmap_storage;
		if ((err = v5_map_get_entry(chd, hunknum, rawmap)) != CHDERR_NONE)
			return err;
#else
		uint8_t *rawmap = &chd->header.rawmap[chd->header.mapentrybytes * hunknum];
#endif
		uint8_t* compressed_bytes;

		/* uncompressed case */
		if (!chd_compressed(&chd->header))
		{
			blockoffs = (uint64_t)get_bigendian_uint32_t(rawmap) * (uint64_t)chd->header.hunkbytes;
			if (blockoffs != 0) {
				if (!seek_and_read(chd, blockoffs, dest, chd->header.hunkbytes))
					return CHDERR_READ_ERROR;
			/* TODO
			else if (m_parent_missing)
				throw CHDERR_REQUIRES_PARENT; */
			} else if (chd->parent) {
				err = hunk_read_into_memory(chd->parent, hunknum, dest);
				if (err != CHDERR_NONE)
					return err;
			} else {
				memset(dest, 0, chd->header.hunkbytes);
			}

			return CHDERR_NONE;
		}

		/* compressed case */
		blocklen = get_bigendian_uint24(&rawmap[1]);
		blockoffs = get_bigendian_uint48(&rawmap[4]);
#if VERIFY_BLOCK_CRC
		blockcrc = get_bigendian_uint16(&rawmap[10]);
#endif
		codec = NULL;
		switch (rawmap[0])
		{
			case COMPRESSION_TYPE_0:
			case COMPRESSION_TYPE_1:
			case COMPRESSION_TYPE_2:
			case COMPRESSION_TYPE_3:
				compressed_bytes = hunk_read_compressed(chd, blockoffs, blocklen);
				if (compressed_bytes == NULL)
					return CHDERR_READ_ERROR;
				switch (chd->codecintf[rawmap[0]]->compression)
				{
					case CHD_CODEC_ZLIB:
						codec = &chd->codec_data.zlib;
						break;

					case CHD_CODEC_LZMA:
						codec = &chd->codec_data.lzma;
						break;

					case CHD_CODEC_HUFFMAN:
						codec = &chd->codec_data.huff;
						break;

					case CHD_CODEC_FLAC:
						codec = &chd->codec_data.flac;
						break;

					case CHD_CODEC_ZSTD:
						codec = &chd->codec_data.zstd;
						break;

					case CHD_CODEC_CD_ZLIB:
						codec = &chd->codec_data.cdzl;
						break;

					case CHD_CODEC_CD_LZMA:
						codec = &chd->codec_data.cdlz;
						break;

					case CHD_CODEC_CD_FLAC:
						codec = &chd->codec_data.cdfl;
						break;

					case CHD_CODEC_CD_ZSTD:
						codec = &chd->codec_data.cdzs;
						break;

					case CHD_CODEC_AVHUFF:
						codec = &chd->codec_data.avhuff;
						break;
				}
				if (codec==NULL)
					return CHDERR_CODEC_ERROR;
#if LOWRAM_TARGET
				if ((err = ensure_codec_ready(chd, rawmap[0], codec)) != CHDERR_NONE)
					return err;
#endif
				err = chd->codecintf[rawmap[0]]->decompress(codec, compressed_bytes, blocklen, dest, chd->header.hunkbytes);
				if (err != CHDERR_NONE)
					return err;
#if VERIFY_BLOCK_CRC
				if (chd_crc16(dest, chd->header.hunkbytes) != blockcrc)
					return CHDERR_DECOMPRESSION_ERROR;
#endif
				return CHDERR_NONE;

			case COMPRESSION_NONE:
				err = hunk_read_uncompressed(chd, blockoffs, blocklen, dest);
				if (err != CHDERR_NONE)
					return err;
#if VERIFY_BLOCK_CRC
				if (chd_crc16(dest, chd->header.hunkbytes) != blockcrc)
					return CHDERR_DECOMPRESSION_ERROR;
#endif
				return CHDERR_NONE;

			case COMPRESSION_SELF:
			{
				uint32_t target = (uint32_t)blockoffs;

				/* only an earlier hunk: a crafted map would recurse forever */
				if (target >= hunknum)
					return CHDERR_DECOMPRESSION_ERROR;

				/* the whole point of the cache: a self-reference otherwise
				 * costs a backward seek plus a full re-decode of a hunk that
				 * was very often decoded moments ago */
				if (chd->selfcache_entries != 0 && selfcache_lookup(chd, target, dest))
					return CHDERR_NONE;

				selfcache_ensure(chd);   /* lazy: only files with self-refs pay */

				err = hunk_read_into_memory(chd, target, dest);
				if (err != CHDERR_NONE)
					return err;
				selfcache_store(chd, target, dest);
				return CHDERR_NONE;
			}

			case COMPRESSION_PARENT:
			{
				uint8_t units_in_hunk;

				if (chd->parent == NULL)
					return CHDERR_REQUIRES_PARENT;
				units_in_hunk = chd->header.hunkbytes / chd->header.unitbytes;

				/* blockoffs is aligned to units_in_hunk */
				if (blockoffs % units_in_hunk == 0) {
					return hunk_read_into_memory(chd->parent, blockoffs / units_in_hunk, dest);
				/* blockoffs is not aligned to units_in_hunk */
				} else {
					uint32_t unit_in_hunk = blockoffs % units_in_hunk;
					uint8_t *buf = (uint8_t*)malloc(chd->header.hunkbytes);
					/* Read first half of hunk which contains blockoffs */
					err = hunk_read_into_memory(chd->parent, blockoffs / units_in_hunk, buf);
					if (err != CHDERR_NONE) {
						free(buf);
						return err;
					}
					memcpy(dest, buf + (size_t)unit_in_hunk * chd->header.unitbytes, (size_t)(units_in_hunk - unit_in_hunk) * chd->header.unitbytes);
					/* Read second half of hunk which contains blockoffs */
					err = hunk_read_into_memory(chd->parent, (blockoffs / units_in_hunk) + 1, buf);
					if (err != CHDERR_NONE) {
						free(buf);
						return err;
					}
					memcpy(dest + (size_t)(units_in_hunk - unit_in_hunk) * chd->header.unitbytes, buf, (size_t)unit_in_hunk * chd->header.unitbytes);
					free(buf);
				}
				break;
			}

			default:  /* the map decodes to types 0-6 only */
				return CHDERR_DECOMPRESSION_ERROR;
		}
		return CHDERR_NONE;
	}

	/* We should not reach this code */
	return CHDERR_DECOMPRESSION_ERROR;
}

/***************************************************************************
    INTERNAL MAP ACCESS
***************************************************************************/

/*-------------------------------------------------
    map_read - read the initial sector map
-------------------------------------------------*/

static chd_error map_read(chd_file *chd)
{
	uint32_t entrysize = (chd->header.version < 3) ? OLD_MAP_ENTRY_SIZE : MAP_ENTRY_SIZE;
	uint8_t raw_map_entries[MAP_STACK_ENTRIES * MAP_ENTRY_SIZE];
	uint64_t fileoffset, maxoffset = 0;
	uint8_t cookie[MAP_ENTRY_SIZE];
	chd_error err;
	uint32_t i;
#if LOWRAM_TARGET
	map_entry extracted[MAP_STACK_ENTRIES];
#endif

	/* legacy (v1-v4) map entries are fixed-size and independently seekable -
	 * under LOWRAM_TARGET, don't materialize the whole array, just validate it
	 * (cookie + maxoffset, same as always) and re-read the single entry
	 * needed on each hunk_read_into_memory() call instead. chd->map stays
	 * NULL; hunknum*entrysize + chd->header.length recovers any entry's
	 * file offset without storing anything. */
#if !LOWRAM_TARGET
	chd->map = (map_entry *)malloc(sizeof(chd->map[0]) * chd->header.totalhunks);
	if (!chd->map)
		return CHDERR_OUT_OF_MEMORY;
#endif

	/* read the map entries in in chunks and extract to the map list */
	fileoffset = chd->header.length;
	for (i = 0; i < chd->header.totalhunks; i += MAP_STACK_ENTRIES)
	{
		/* compute how many entries this time */
		int entries = chd->header.totalhunks - i, j;
#if !LOWRAM_TARGET
		map_entry *dest = &chd->map[i];
#else
		map_entry *dest = extracted;
#endif
		if (entries > MAP_STACK_ENTRIES)
			entries = MAP_STACK_ENTRIES;

		/* read that many */
		if (!seek_and_read(chd, fileoffset, raw_map_entries, entries * (size_t)entrysize))
			EARLY_EXIT(err = CHDERR_READ_ERROR);
		fileoffset += entries * (size_t)entrysize;

		/* process that many */
		if (entrysize == MAP_ENTRY_SIZE)
		{
			for (j = 0; j < entries; j++)
				map_extract(&raw_map_entries[j * MAP_ENTRY_SIZE], &dest[j]);
		}
		else
		{
			for (j = 0; j < entries; j++)
				map_extract_old(&raw_map_entries[j * OLD_MAP_ENTRY_SIZE], &dest[j], chd->header.hunkbytes);
		}

		/* track the maximum offset */
		for (j = 0; j < entries; j++)
			if ((dest[j].flags & MAP_ENTRY_FLAG_TYPE_MASK) == V34_MAP_ENTRY_TYPE_COMPRESSED ||
				(dest[j].flags & MAP_ENTRY_FLAG_TYPE_MASK) == V34_MAP_ENTRY_TYPE_UNCOMPRESSED)
				maxoffset = MAX(maxoffset, dest[j].offset + dest[j].length);
	}

	/* verify the cookie */
	if (!seek_and_read(chd, fileoffset, &cookie, entrysize) || memcmp(&cookie, END_OF_LIST_COOKIE, entrysize))
		EARLY_EXIT(err = CHDERR_INVALID_FILE);

	/* verify the length */
	if (maxoffset > chd->file_size)
		EARLY_EXIT(err = CHDERR_INVALID_FILE);
	return CHDERR_NONE;

cleanup:
#if !LOWRAM_TARGET
	if (chd->map)
		free(chd->map);
	chd->map = NULL;
#endif
	return err;
}

#if LOWRAM_TARGET
/*-------------------------------------------------
    map_read_one_legacy - lazily fetch a single
    v1-v4 map entry directly from the file
-------------------------------------------------*/

static chd_error map_read_one_legacy(chd_file *chd, uint32_t hunknum, map_entry *entry)
{
	uint32_t entrysize = (chd->header.version < 3) ? OLD_MAP_ENTRY_SIZE : MAP_ENTRY_SIZE;
	uint8_t raw[MAP_ENTRY_SIZE];
	uint64_t fileoffset = chd->header.length + (uint64_t)hunknum * entrysize;

	if (!seek_and_read(chd, fileoffset, raw, entrysize))
		return CHDERR_READ_ERROR;

	if (entrysize == MAP_ENTRY_SIZE)
		map_extract(raw, entry);
	else
		map_extract_old(raw, entry, chd->header.hunkbytes);
	return CHDERR_NONE;
}
#endif

/***************************************************************************
    INTERNAL METADATA ACCESS
***************************************************************************/

/*-------------------------------------------------
    metadata_find_entry - find a metadata entry
-------------------------------------------------*/

static chd_error metadata_find_entry(chd_file *chd, uint32_t metatag, uint32_t metaindex, metadata_entry *metaentry)
{
	/* cap traversal to guard against malformed CHDs with cyclic metadata chains */
	uint32_t iter = 0;
	#define CHD_MAX_METADATA_ENTRIES 65536

	/* start at the beginning */
	metaentry->offset = chd->header.metaoffset;
	metaentry->prev = 0;

	/* loop until we run out of options */
	while (metaentry->offset != 0)
	{
		uint8_t	raw_meta_header[METADATA_HEADER_SIZE];

		if (++iter > CHD_MAX_METADATA_ENTRIES)
			return CHDERR_INVALID_DATA;

		/* read the raw header */
		if (!seek_and_read(chd, metaentry->offset, raw_meta_header, sizeof(raw_meta_header)))
			break;

		/* extract the data */
		metaentry->metatag = get_bigendian_uint32_t(&raw_meta_header[0]);
		metaentry->length = get_bigendian_uint32_t(&raw_meta_header[4]);
		metaentry->next = get_bigendian_uint64_t(&raw_meta_header[8]);

		/* flags are encoded in the high byte of length */
		metaentry->flags = metaentry->length >> 24;
		metaentry->length &= 0x00ffffff;

		/* if we got a match, proceed */
		if (metatag == CHDMETATAG_WILDCARD || metaentry->metatag == metatag)
			if (metaindex-- == 0)
				return CHDERR_NONE;

		/* no match, fetch the next link */
		metaentry->prev = metaentry->offset;
		metaentry->offset = metaentry->next;
	}

	/* if we get here, we didn't find it */
	return CHDERR_METADATA_NOT_FOUND;
}

/***************************************************************************
    CORE FILE
***************************************************************************/

/*-------------------------------------------------
	core_stdio_fopen - core_file wrapper over fopen
-------------------------------------------------*/
static void *core_stdio_fopen(char const *path) {
	return fopen(path, "rb");
}

/*-------------------------------------------------
	core_stdio_fsize - core_file function for
	getting file size with stdio
-------------------------------------------------*/
static uint64_t core_stdio_fsize(void *file) {
#if defined USE_LIBRETRO_VFS
	/* The libretro VFS fseek returns the new file position, so we adapt it as needed. */
	#define core_stdio_fseek_impl(file,offset,whence) (fseek(file, offset, whence) < 0 ? -1 : 0)
	#define core_stdio_ftell_impl ftell
#elif defined(__WIN32__) || defined(_WIN32) || defined(WIN32) || defined(__WIN64__)
	#define core_stdio_fseek_impl _fseeki64
	#define core_stdio_ftell_impl _ftelli64
#elif defined(__PS3__) && !defined(__PSL1GHT__) || defined(__SWITCH__) || defined(__vita__)
	#define core_stdio_fseek_impl(x,y,z) fseek(x,(off_t)y,z)
	#define core_stdio_ftell_impl(x) (off_t)ftell(x)
#else
	/* With _FILE_OFFSET_BITS=64 defined at the top of this TU, glibc
	 * aliases fseeko/ftello to the 64-bit variants, so no *64 suffix
	 * is needed. This is also the time64-safe spelling on armhf. */
	#define core_stdio_fseek_impl fseeko
	#define core_stdio_ftell_impl ftello
#endif
	FILE *fp;
	uint64_t p, rv;
	fp = (FILE*)file;

	p = core_stdio_ftell_impl(fp);
	core_stdio_fseek_impl(fp, 0, SEEK_END);
	rv = core_stdio_ftell_impl(fp);
	core_stdio_fseek_impl(fp, p, SEEK_SET);
	return rv;
}

/*-------------------------------------------------
	core_stdio_fread - core_file wrapper over fread
-------------------------------------------------*/
static size_t core_stdio_fread(void *ptr, size_t size, size_t nmemb, void *file) {
	return fread(ptr, size, nmemb, (FILE*)file);
}

/*-------------------------------------------------
	core_stdio_fclose - core_file wrapper over fclose
-------------------------------------------------*/
static int core_stdio_fclose(void *file) {
	return fclose((FILE*)file);
}

/*-------------------------------------------------
	core_stdio_fclose_nonowner - don't call fclose because
		we don't own the underlying file.
-------------------------------------------------*/
static int core_stdio_fclose_nonowner(void *file) {
	(void)file;
	return 0;
}

/*-------------------------------------------------
	core_stdio_fseek - core_file wrapper over fclose
-------------------------------------------------*/
static int core_stdio_fseek(void* file, int64_t offset, int whence) {
	return core_stdio_fseek_impl((FILE*)file, offset, whence);
}

/*-------------------------------------------------
	core_legacy_fsize - legacy core_file wrapper
-------------------------------------------------*/
static uint64_t core_legacy_fsize(void *file) {
	core_file* const core = (core_file*)file;
	return core->fsize(core);
}

/*-------------------------------------------------
	core_legacy_fread - legacy core_file wrapper
-------------------------------------------------*/
static size_t core_legacy_fread(void *ptr, size_t size, size_t nmemb, void *file) {
	core_file* const core = (core_file*)file;
	return core->fread(ptr, size, nmemb, core);
}

/*-------------------------------------------------
	core_legacy_fclose - legacy core_file wrapper
-------------------------------------------------*/
static int core_legacy_fclose(void *file) {
	core_file* const core = (core_file*)file;
	return core->fclose(core);
}

/*-------------------------------------------------
	core_legacy_fseek - legacy core_file wrapper
-------------------------------------------------*/
static int core_legacy_fseek(void* file, int64_t offset, int whence) {
	core_file* const core = (core_file*)file;
	return core->fseek(core, offset, whence);
}
