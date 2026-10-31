#include "codec_cdzl.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/cdrom.h"

/* Undefined, this evaluates to 0 below and silently selects the in-place
 * path, which sizes the codec scratch differently. Fail loudly instead. */
#ifndef CHDR_CD_SCRATCH_BUFFER
#error "chdconfig.h must be included before this file"
#endif

chd_error cdzl_codec_init(void *codec, uint32_t hunkbytes)
{
	chd_error ret;
	cdzl_codec_data* cdzl = (cdzl_codec_data*)codec;

	/* make sure the CHD's hunk size is an even multiple of the frame size */
	if (hunkbytes % CD_FRAME_SIZE != 0)
		return CHDERR_CODEC_ERROR;

	/* Only the subcode needs a scratch buffer now: the sector data is decoded
	 * straight into the caller's hunk. That is hunkbytes/25.5 instead of
	 * hunkbytes - 768 bytes rather than 19584 at CD geometry. */
#if CHDR_CD_SCRATCH_BUFFER
	cdzl->buffer = (uint8_t*)malloc(sizeof(uint8_t) * hunkbytes);
#else
	/* subcode staging plus one frame of bounce for the in-place spread */
	cdzl->buffer = (uint8_t*)malloc(sizeof(uint8_t) * ((hunkbytes / CD_FRAME_SIZE) * CD_MAX_SUBCODE_DATA + CD_MAX_SECTOR_DATA));
#endif
	if (cdzl->buffer == NULL)
		return CHDERR_OUT_OF_MEMORY;

	ret = zlib_codec_init(&cdzl->base_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SECTOR_DATA);
	if (ret != CHDERR_NONE)
		return ret;

#if WANT_SUBCODE
	ret = zlib_codec_init(&cdzl->subcode_decompressor, (hunkbytes / CD_FRAME_SIZE) * CD_MAX_SUBCODE_DATA);
	if (ret != CHDERR_NONE)
		return ret;
#endif

	return CHDERR_NONE;
}

void cdzl_codec_free(void *codec)
{
	cdzl_codec_data* cdzl = (cdzl_codec_data*)codec;
	zlib_codec_free(&cdzl->base_decompressor);
#if WANT_SUBCODE
	zlib_codec_free(&cdzl->subcode_decompressor);
#endif
	free(cdzl->buffer);
}

chd_error cdzl_codec_decompress(void *codec, const uint8_t *src, uint32_t complen, uint8_t *dest, uint32_t destlen)
{
	cdzl_codec_data* cdzl = (cdzl_codec_data*)codec;

	return cd_codec_decompress(cdzl->buffer,
		&cdzl->base_decompressor, zlib_codec_decompress,
#if WANT_SUBCODE
		&cdzl->subcode_decompressor, zlib_codec_decompress,
#else
		NULL, NULL,
#endif
		src, complen, dest, destlen
	);
}
