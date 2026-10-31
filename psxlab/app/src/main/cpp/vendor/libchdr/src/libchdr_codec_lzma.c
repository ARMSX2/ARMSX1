#include "codec_lzma.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/***************************************************************************
 *  LZMA ALLOCATOR HELPER
 ***************************************************************************
 */

static void *lzma_fast_alloc(ISzAllocPtr p, size_t size);
static void lzma_fast_free(ISzAllocPtr p, void *address);

/*-------------------------------------------------
 *  lzma_allocator_init
 *-------------------------------------------------
 */

static void lzma_allocator_init(void* p)
{
	lzma_allocator *codec = (lzma_allocator *)(p);

	/* reset pointer list */
	memset(codec->allocptr, 0, sizeof(codec->allocptr));
	memset(codec->allocptr2, 0, sizeof(codec->allocptr2));
	codec->base.Alloc = lzma_fast_alloc;
	codec->base.Free = lzma_fast_free;
}

/*-------------------------------------------------
 *  lzma_allocator_free
 *-------------------------------------------------
 */

static void lzma_allocator_free(void* p )
{
	int i;
	lzma_allocator *codec = (lzma_allocator *)(p);

	/* free our memory */
	for (i = 0 ; i < MAX_LZMA_ALLOCS ; i++)
	{
		if (codec->allocptr[i] != NULL)
			free(codec->allocptr[i]);
	}
}

/*-------------------------------------------------
 *  lzma_fast_alloc - fast malloc for lzma, which
 *  allocates and frees memory frequently
 *-------------------------------------------------
 */

/* Huge alignment values for possible SIMD optimization by compiler (NEON, SSE, AVX) */
#define LZMA_MIN_ALIGNMENT_BITS 512
#define LZMA_MIN_ALIGNMENT_BYTES (LZMA_MIN_ALIGNMENT_BITS / 8)

static void *lzma_fast_alloc(ISzAllocPtr p, size_t size)
{
	int scan;
	uint32_t *addr        = NULL;
	lzma_allocator *codec = (lzma_allocator *)(p);
	uintptr_t vaddr = 0;

	/* compute the size, rounding to the nearest 1k */
	size = (size + 0x3ff) & ~0x3ff;

	/* reuse a hunk if we can */
	for (scan = 0; scan < MAX_LZMA_ALLOCS; scan++)
	{
		uint32_t *ptr = codec->allocptr[scan];
		if (ptr != NULL && size == *ptr)
		{
			/* set the low bit of the size so we don't match next time */
			*ptr |= 1;

			/* return aligned address of the block */
			return codec->allocptr2[scan];
		}
	}

	/* alloc a new one and put it into the list */
	addr = (uint32_t *)malloc(size + sizeof(uint32_t) + LZMA_MIN_ALIGNMENT_BYTES);
	if (addr==NULL)
		return NULL;
	for (scan = 0; scan < MAX_LZMA_ALLOCS; scan++)
	{
		if (codec->allocptr[scan] == NULL)
		{
			/* store block address */
			codec->allocptr[scan] = addr;

			/* compute aligned address, store it */
			vaddr = (uintptr_t)addr;
			vaddr = (vaddr + sizeof(uint32_t) + (LZMA_MIN_ALIGNMENT_BYTES-1)) & (~(LZMA_MIN_ALIGNMENT_BYTES-1));
			codec->allocptr2[scan] = (uint32_t*)vaddr;
			break;
		}
	}

	/* set the low bit of the size so we don't match next time */
	*addr = size | 1;

	/* return aligned address */
	return (void*)vaddr;
}

/*-------------------------------------------------
 *  lzma_fast_free - fast free for lzma, which
 *  allocates and frees memory frequently
 *-------------------------------------------------
 */

static void lzma_fast_free(ISzAllocPtr p, void *address)
{
	int scan;
	uint32_t *ptr = NULL;
	lzma_allocator *codec = NULL;

	if (address == NULL)
		return;

	codec = (lzma_allocator *)(p);

	/* find the hunk */
	ptr = (uint32_t *)address;
	for (scan = 0; scan < MAX_LZMA_ALLOCS; scan++)
	{
		if (ptr == codec->allocptr2[scan])
		{
			/* clear the low bit of the size to allow matches */
			*codec->allocptr[scan] &= ~1;
			return;
		}
	}
}

/***************************************************************************
 *  LZMA DECOMPRESSOR
 ***************************************************************************
 */

/*-------------------------------------------------
 *  lzma_compute_aligned_dictionary_size
 *  Based on LzmaEncProps_Normalize, LzmaEnc_SetProps, LzmaEnc_WriteProperties.
 *-------------------------------------------------
 */

static uint32_t lzma_compute_aligned_dictionary_size(uint32_t hunkbytes)
{
	/* MAME's chd_lzma_compressor::configure_properties() (src/lib/util/chdcodec.cpp) uses
	 * level = 6 and reduceSize = hunkbytes, then calls LzmaEncProps_Normalize(). The decoder
	 * has no way to read back what the encoder actually used (see MAME's own FIXME comment on
	 * chd_lzma_decompressor's constructor), so it must independently re-derive the exact same
	 * dictionary size from hunkbytes alone. Mirror LzmaEncProps_Normalize() and
	 * LzmaEnc_WriteProperties() from the LZMA SDK verbatim - do not "simplify" this, any
	 * deviation that yields a *smaller* dictionary than the real encoder used will silently
	 * corrupt decoded CHDs (dictionary too small to hold the encoder's back-references).
	 */
	const unsigned int level = 6;
	const uint32_t reduceSize = hunkbytes;

	uint32_t dictSize, alignedDictSize;

	/* LzmaEncProps_Normalize: level-based default dictionary size */
	dictSize = level <= 4 ?
		(uint32_t)1 << (level * 2 + 16) :
		level <= sizeof(size_t) / 2 + 4 ?
			(uint32_t)1 << (level + 20) :
			(uint32_t)1 << (sizeof(size_t) / 2 + 24);

	/* LzmaEncProps_Normalize: clamp dictSize down toward reduceSize (hunkbytes), never below
	 * kReduceMin. This must be MIN(dictSize, ...), not MAX - a MAX here is a no-op that leaves
	 * dictSize at its full level-based default (67108864 bytes at level 6) regardless of
	 * hunkbytes, which is the bug this function used to have. Spelled out with the MIN/MAX
	 * macros rather than manual if-statements on purpose: that's what let the original bug
	 * hide in the first place. */
	if (dictSize > reduceSize)
	{
		const uint32_t kReduceMin = (uint32_t)1 << 12;
		dictSize = MIN(dictSize, MAX(reduceSize, kReduceMin));
	}

	/* LzmaEnc_WriteProperties: dictionary size actually written to (here, re-derived for) the
	 * stream properties - rounds up to the nearest representable LZMA dictionary size. */
	if (dictSize >= ((uint32_t)1 << 21))
	{
		const uint32_t kDictMask = ((uint32_t)1 << 20) - 1;

		alignedDictSize = (dictSize + kDictMask) & ~kDictMask;
		if (alignedDictSize < dictSize)
			alignedDictSize = dictSize;
	}
	else
	{
		unsigned int i = 11 * 2;

		do
		{
			alignedDictSize = (uint32_t)(2 + (i & 1)) << (i >> 1);
			i++;
		}
		while (alignedDictSize < dictSize);
	}

	return alignedDictSize;
}

/*-------------------------------------------------
 *  lzma_codec_init - constructor
 *-------------------------------------------------
 */

chd_error lzma_codec_init(void* codec, uint32_t hunkbytes)
{
	lzma_codec_data* lzma_codec = (lzma_codec_data*) codec;
	lzma_allocator* alloc = &lzma_codec->allocator;
	const uint32_t alignedDictSize = lzma_compute_aligned_dictionary_size(hunkbytes);

	unsigned int i;
	Byte decoder_props[LZMA_PROPS_SIZE];

	decoder_props[0] = 93;
	for (i = 0; i < LZMA_PROPS_SIZE - 1; ++i)
		decoder_props[1 + i] = (alignedDictSize >> (8 * i)) & 0xFF;

	lzma_allocator_init(alloc);

	/* construct the decoder */
	LzmaDec_Construct(&lzma_codec->decoder);

	/* do memory allocations */
	/* Probs only, no dictionary: the dictionary is pointed at the caller's
	 * output buffer per call instead, which is what the SDK's own one-call
	 * LzmaDecode() does. Saves a hunk-sized allocation and the memcpy that
	 * LzmaDec_DecodeToBuf would make out of a private dictionary. */
	if (LzmaDec_AllocateProbs(&lzma_codec->decoder, decoder_props, LZMA_PROPS_SIZE, &alloc->base) != SZ_OK)
		return CHDERR_DECOMPRESSION_ERROR;

	/* Okay */
	return CHDERR_NONE;
}

/*-------------------------------------------------
 *  lzma_codec_free
 *-------------------------------------------------
 */

void lzma_codec_free(void* codec)
{
	lzma_codec_data* lzma_codec = (lzma_codec_data*) codec;

	/* free memory */
	LzmaDec_FreeProbs(&lzma_codec->decoder, &lzma_codec->allocator.base);
	lzma_allocator_free(&lzma_codec->allocator);
}

/*-------------------------------------------------
 *  decompress - decompress data using the LZMA
 *  codec
 *-------------------------------------------------
 */

chd_error lzma_codec_decompress(void* codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	ELzmaStatus status;
	SRes res;
	SizeT consumedlen, decodedlen;
	/* initialize */
	lzma_codec_data* lzma_codec = (lzma_codec_data*) codec;

	/* Decode straight into the caller's buffer by using it as the dictionary.
	 * Each hunk is an independent stream, so back-references can never reach
	 * before its start and destlen is a sufficient window. */
	lzma_codec->decoder.dic = dest;
	lzma_codec->decoder.dicBufSize = destlen;
	LzmaDec_Init(&lzma_codec->decoder);

	/* decode */
	consumedlen = complen;
	res = LzmaDec_DecodeToDic(&lzma_codec->decoder, destlen, src, &consumedlen, LZMA_FINISH_END, &status);
	decodedlen = lzma_codec->decoder.dicPos;
	lzma_codec->decoder.dic = NULL;   /* never let LzmaDec_Free touch it */
	lzma_codec->decoder.dicBufSize = 0;
	if ((res != SZ_OK && res != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK) || consumedlen != complen || decodedlen != destlen)
		return CHDERR_DECOMPRESSION_ERROR;
	return CHDERR_NONE;
}
