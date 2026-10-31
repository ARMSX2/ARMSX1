#ifndef LIBCHDR_CODEC_CDZS_H
#define LIBCHDR_CODEC_CDZS_H

#include <stdint.h>

#include "../include/libchdr/chd.h"
#include "../include/libchdr/chdconfig.h"
#include "codec_zstd.h"

/* codec-private data for the CDZS codec */
typedef struct _cdzs_codec_data cdzs_codec_data;
struct _cdzs_codec_data
{
	/* One decompression context serves both the sector-data and subcode
	 * streams under LOWRAM_TARGET, halving the codec's context memory.
	 *
	 * Safe because the two streams are decoded strictly in sequence by
	 * cd_codec_decompress() - base to completion, error returns early, then
	 * subcode - never nested and never concurrently, into disjoint halves of
	 * `buffer`; and because the decompress entry point re-initialises the
	 * context on entry, so no state carries from one stream to the other.
	 *
	 * It is not free: alternating two differently-shaped streams through one
	 * context costs measurably more CPU than giving each its own, because the
	 * context's working set is rebuilt each way: measured on x86-64 over a
	 * whole file, +3.9%. A ZSTD_DCtx is ~94KB, so on a memory-constrained
	 * part that is a good trade and on a desktop it is not - hence
	 * LOWRAM_TARGET, which exists to make exactly this choice. A default
	 * build keeps two contexts and its previous speed.
	 *
	 * The same sharing was tried for cdzl and reverted: an inflate context is
	 * only ~8KB once the unused 32KB dictionary is gone, so paying 2.6% CPU
	 * for it is not worth it.
	 *
	 * NOTE for anyone adding threading (see the pread work in PR #162):
	 * codec state is already shared across concurrent chd_read() calls, but
	 * this removes even the accidental separation between base and subcode.
	 * Per-thread codec state has to mean per-thread codec instances. */
#if LOWRAM_TARGET
	zstd_codec_data base_decompressor;
#define cdzs_subcode_ctx(c) (&(c)->base_decompressor)
#else
	zstd_codec_data base_decompressor;
#if WANT_SUBCODE
	zstd_codec_data subcode_decompressor;
#endif
#define cdzs_subcode_ctx(c) (&(c)->subcode_decompressor)
#endif
	uint8_t*				buffer;
};

/* cdlz compression codec */
chd_error cdzs_codec_init(void *codec, uint32_t hunkbytes);
void cdzs_codec_free(void *codec);
chd_error cdzs_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen);

#endif /* LIBCHDR_CODEC_CDZS_H */
