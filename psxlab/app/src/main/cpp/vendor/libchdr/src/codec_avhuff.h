/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
 *
 * codec_avhuff.h
 *
 * AVHuff (audio/video) codec decompressor private data.
 * Used by CHDv5 'avhu' codec for laserdisc CHDs.
 */

#ifndef __CODEC_AVHUFF_H__
#define __CODEC_AVHUFF_H__

#include <stdint.h>

#include "../include/libchdr/chd.h"
#include "../include/libchdr/flac.h"

struct huffman_decoder;

/* codec-private data for the AVHuff codec */
typedef struct _avhuff_codec_data avhuff_codec_data;
struct _avhuff_codec_data
{
	/* video delta-RLE decoder state (Y, Cb, Cr) */
	struct huffman_decoder *ycontext;
	struct huffman_decoder *cbcontext;
	struct huffman_decoder *crcontext;
	uint8_t                 y_prev;
	uint8_t                 cb_prev;
	uint8_t                 cr_prev;
	int                     y_rlecount;
	int                     cb_rlecount;
	int                     cr_rlecount;

	/* audio delta-RLE decoder state (hi byte, lo byte) */
	struct huffman_decoder *audiohi;
	struct huffman_decoder *audiolo;
	uint8_t                 ahi_prev;
	uint8_t                 alo_prev;
	int                     ahi_rlecount;
	int                     alo_rlecount;

	/* FLAC decoder (reused per channel when treesize == 0xffff) */
	flac_decoder            flac;
};

chd_error avhuff_codec_init(void *codec, uint32_t hunkbytes);
void      avhuff_codec_free(void *codec);
chd_error avhuff_codec_decompress(void *codec, const uint8_t *src, uint32_t complen,
                                  uint8_t *dest, uint32_t destlen);

#endif /* __CODEC_AVHUFF_H__ */
