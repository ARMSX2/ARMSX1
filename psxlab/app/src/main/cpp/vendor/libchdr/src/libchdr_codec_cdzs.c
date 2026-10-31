#include "codec_cdzs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/cdrom.h"

/* Undefined, this evaluates to 0 below and silently selects the in-place
 * path, which sizes the codec scratch differently. Fail loudly instead. */
#ifndef CHDR_CD_SCRATCH_BUFFER
#error "chdconfig.h must be included before this file"
#endif

chd_error cdzs_codec_init(void* codec, uint32_t hunkbytes)
{
	chd_error ret;
	cdzs_codec_data* cdzs = (cdzs_codec_data*) codec;

	/* Only the subcode needs a scratch buffer now: the sector data is decoded
	 * straight into the caller's hunk. That is hunkbytes/25.5 instead of
	 * hunkbytes - 768 bytes rather than 19584 at CD geometry. */
#if CHDR_CD_SCRATCH_BUFFER
	cdzs->buffer = (uint8_t*)malloc(sizeof(uint8_t) * hunkbytes);
#else
	/* subcode staging plus one frame of bounce for the in-place spread */
	cdzs->buffer = (uint8_t*)malloc(sizeof(uint8_t) * ((hunkbytes / CD_FRAME_SIZE) * CD_MAX_SUBCODE_DATA + CD_MAX_SECTOR_DATA));
#endif
	if (cdzs->buffer == NULL)
		return CHDERR_OUT_OF_MEMORY;

	/* make sure the CHD's hunk size is an even multiple of the frame size */
	ret = zstd_codec_init(&cdzs->base_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SECTOR_DATA);
	if (ret != CHDERR_NONE)
		return ret;

#if WANT_SUBCODE && !LOWRAM_TARGET
	ret = zstd_codec_init(&cdzs->subcode_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SUBCODE_DATA);
#endif
	if (ret != CHDERR_NONE)
		return ret;

	if (hunkbytes % CD_FRAME_SIZE != 0)
		return CHDERR_CODEC_ERROR;

	return CHDERR_NONE;
}

void cdzs_codec_free(void* codec)
{
	cdzs_codec_data* cdzs = (cdzs_codec_data*) codec;
	free(cdzs->buffer);
	zstd_codec_free(&cdzs->base_decompressor);
#if WANT_SUBCODE && !LOWRAM_TARGET
	zstd_codec_free(&cdzs->subcode_decompressor);
#endif
}

chd_error cdzs_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	cdzs_codec_data* cdzs = (cdzs_codec_data*)codec;

	return cd_codec_decompress(cdzs->buffer,
		&cdzs->base_decompressor, zstd_codec_decompress,
#if WANT_SUBCODE
		cdzs_subcode_ctx(cdzs), zstd_codec_decompress,
#else
		NULL, NULL,
#endif
		src, complen, dest, destlen
	);
}
