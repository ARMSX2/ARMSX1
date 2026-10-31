#ifndef LIBCHDR_CODEC_ZLIB_H
#define LIBCHDR_CODEC_ZLIB_H

#include <stdint.h>

#if defined(__PS3__) || defined(__PSL1GHT__)
#define __MACTYPES__
#endif
#ifdef CHDR_SYSTEM_ZLIB
#include <zlib.h>
typedef uInt zlib_alloc_size;
#else
#include "../deps/miniz-3.1.2/miniz.h"
typedef size_t zlib_alloc_size;
#endif

#include "../include/libchdr/chd.h"

/* codec-private data for the ZLIB codec */
#define MAX_ZLIB_ALLOCS				64

typedef struct _zlib_allocator zlib_allocator;
struct _zlib_allocator
{
	uint32_t *				allocptr[MAX_ZLIB_ALLOCS];
	uint32_t *				allocptr2[MAX_ZLIB_ALLOCS];
};

typedef struct _zlib_codec_data zlib_codec_data;
struct _zlib_codec_data
{
#ifdef CHDR_SYSTEM_ZLIB
	z_stream				inflater;
	zlib_allocator			allocator;
#else
	/* With bundled miniz we drive tinfl directly rather than going through
	 * the mz_inflate*() wrappers. Those allocate miniz's inflate_state, which
	 * embeds a fixed 32KB LZ dictionary (m_dict) - 41168 bytes per instance
	 * against tinfl_decompressor's 8376.
	 *
	 * The dictionary is dead weight here. libchdr always decompresses a hunk
	 * as one complete stream into a buffer large enough to hold all of it, so
	 * mz_inflate() was already passing TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF
	 * and tinfl was using the caller's output buffer as its own dictionary;
	 * m_dict was allocated and never touched. miniz also refuses any window
	 * size other than +/-15, so the wrappers give no way to ask for less.
	 *
	 * A CD-flavoured CHD instantiates this three or four times (cdzl needs one
	 * for sector data and one for subcode; cdlz and cdfl each need one for
	 * subcode), so dropping the dictionary saves ~32KB apiece - measured
	 * against a 254KB peak for a three-codec CD file. Heap-allocated rather
	 * than inline because chd_file embeds every codec's state by value. */
	tinfl_decompressor *	inflater;
	/* Set when `inflater` is owned by the chd_file rather than by this codec.
	 * The CD codecs' subcode inflaters are one shared object - a hunk is
	 * decoded by exactly one CD codec, and tinfl_init() runs on entry to
	 * every decompress, so nothing carries across - and only the owner
	 * frees it. */
	int						borrowed;
#endif
};

/* zlib compression codec */
chd_error zlib_codec_init(void *codec, uint32_t hunkbytes);
void zlib_codec_free(void *codec);
#ifndef CHDR_SYSTEM_ZLIB
void zlib_codec_lend(void *codec, void *owner);
#endif
chd_error zlib_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen);

#endif /* LIBCHDR_CODEC_ZLIB_H */
