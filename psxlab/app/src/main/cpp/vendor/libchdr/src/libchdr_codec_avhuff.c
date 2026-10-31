/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
 *
 * libchdr_codec_avhuff.c
 *
 * AVHuff decompressor for CHDv5 'avhu' hunks and CHDv3/v4 CHDCOMPRESSION_AV
 * hunks. Decompression-only port of MAME's src/lib/util/avhuff.cpp with the
 * encoder, C++ scaffolding, and emucore dependencies removed.
 *
 * Reuses libchdr's existing primitives:
 *   - huffman decoder (src/libchdr_huffman.c)
 *   - bitstream reader (src/libchdr_bitstream.c)
 *   - FLAC via dr_flac wrapper (src/libchdr_flac.c)
 */

#include "codec_avhuff.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../include/libchdr/bitstream.h"
#include "../include/libchdr/huffman.h"

/***************************************************************************
    CONSTANTS
***************************************************************************/

#define AVHUFF_NUMCODES   (256 + 16)  /* 256 byte values + 16 RLE run codes */
#define AVHUFF_MAXBITS    16

/***************************************************************************
    HELPERS
***************************************************************************/

static uint16_t get_u16be(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static int16_t get_s16be(const uint8_t *p)
{
	return (int16_t)get_u16be(p);
}

static void put_u16be(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* RLE run length for an escape code (matches MAME avhuff.cpp code_to_rlecount) */
static int code_to_rlecount(int code)
{
	if (code == 0x00)
		return 1;
	if (code <= 0x107)
		return 8 + (code - 0x100);
	return 16 << (code - 0x108);
}

/* Decode one byte from a delta-RLE huffman stream, maintaining prev + rlecount
 * at the caller's level. */
static uint8_t deltarle_decode_one(struct huffman_decoder *dec, struct bitstream *bitbuf,
                                   uint8_t *prev, int *rlecount)
{
	uint32_t data;

	if (*rlecount != 0)
	{
		(*rlecount)--;
		return *prev;
	}

	data = huffman_decode_one(dec, bitbuf);
	if (data < 0x100)
	{
		*prev = (uint8_t)(*prev + (uint8_t)data);
		return *prev;
	}

	*rlecount = code_to_rlecount((int)data);
	(*rlecount)--;
	return *prev;
}

static void deltarle_reset(uint8_t *prev, int *rlecount)
{
	*prev = 0;
	*rlecount = 0;
}

static void deltarle_flush(int *rlecount)
{
	*rlecount = 0;
}

/***************************************************************************
    CODEC INIT / FREE
***************************************************************************/

chd_error avhuff_codec_init(void *codec, uint32_t hunkbytes)
{
	avhuff_codec_data *avhuff = (avhuff_codec_data *)codec;

	(void)hunkbytes;

	memset(avhuff, 0, sizeof(*avhuff));

	/* Y/Cb/Cr decoders are required for every AVHuff hunk; allocate eagerly.
	 * audiohi/audiolo are only used by the huffman audio sub-codec (treesize
	 * non-zero and non-0xffff). Modern chdman emits FLAC audio (treesize=
	 * 0xffff) almost exclusively, so defer those 256 KiB until first use. */
	avhuff->ycontext  = create_huffman_decoder(AVHUFF_NUMCODES, AVHUFF_MAXBITS);
	avhuff->cbcontext = create_huffman_decoder(AVHUFF_NUMCODES, AVHUFF_MAXBITS);
	avhuff->crcontext = create_huffman_decoder(AVHUFF_NUMCODES, AVHUFF_MAXBITS);

	if (avhuff->ycontext == NULL || avhuff->cbcontext == NULL ||
	    avhuff->crcontext == NULL)
	{
		avhuff_codec_free(codec);
		return CHDERR_OUT_OF_MEMORY;
	}

	if (flac_decoder_init(&avhuff->flac) != 0)
	{
		avhuff_codec_free(codec);
		return CHDERR_OUT_OF_MEMORY;
	}

	return CHDERR_NONE;
}

void avhuff_codec_free(void *codec)
{
	avhuff_codec_data *avhuff = (avhuff_codec_data *)codec;

	if (avhuff->ycontext  != NULL) delete_huffman_decoder(avhuff->ycontext);
	if (avhuff->cbcontext != NULL) delete_huffman_decoder(avhuff->cbcontext);
	if (avhuff->crcontext != NULL) delete_huffman_decoder(avhuff->crcontext);
	if (avhuff->audiohi   != NULL) delete_huffman_decoder(avhuff->audiohi);
	if (avhuff->audiolo   != NULL) delete_huffman_decoder(avhuff->audiolo);
	flac_decoder_free(&avhuff->flac);

	avhuff->ycontext = avhuff->cbcontext = avhuff->crcontext = NULL;
	avhuff->audiohi = avhuff->audiolo = NULL;
}

/***************************************************************************
    AUDIO DECODE
***************************************************************************/

static chd_error decode_audio_flac(avhuff_codec_data *avhuff, uint32_t channels,
                                   uint32_t samples, const uint8_t *source,
                                   uint8_t **audiostart, const uint8_t *sizes)
{
	uint32_t chnum;

	/* CHD raw hunks: destination is always big-endian, dxor = 0.
	 * flac_decoder_decode_interleaved writes in native byte order; pass
	 * swap_endian=1 so the output is byte-swapped to BE on LE hosts.
	 * detect_native_endian() returns 1 on LE, 0 on BE — that's exactly
	 * the swap value we need. */
	int swap_endian = flac_decoder_detect_native_endian();

	for (chnum = 0; chnum < channels; chnum++)
	{
		uint16_t size = get_u16be(&sizes[chnum * 2 + 2]);
		uint8_t *curdest = audiostart[chnum];

		if (curdest != NULL)
		{
			if (!flac_decoder_reset(&avhuff->flac, 48000, 1, samples, source, size))
				return CHDERR_DECOMPRESSION_ERROR;
			if (!flac_decoder_decode_interleaved(&avhuff->flac,
			                                    (int16_t *)curdest,
			                                    samples, swap_endian))
				return CHDERR_DECOMPRESSION_ERROR;
			flac_decoder_finish(&avhuff->flac);
		}

		source += size;
	}

	return CHDERR_NONE;
}

static chd_error decode_audio(avhuff_codec_data *avhuff, uint32_t channels,
                              uint32_t samples, const uint8_t *source,
                              uint8_t **audiostart, const uint8_t *sizes)
{
	uint16_t treesize = get_u16be(&sizes[0]);
	uint32_t chnum, sampnum;
	struct bitstream *bitbuf;

	if (treesize == 0xffff)
		return decode_audio_flac(avhuff, channels, samples, source, audiostart, sizes);

	/* If treesize > 0, import both hi/lo huffman trees from the first
	 * treesize bytes of the audio region. */
	if (treesize != 0)
	{
		enum huffman_error hufferr;

		/* lazy-allocate the audio huffman decoders on first huffman-audio
		 * hunk; reused for the lifetime of the codec instance */
		if (avhuff->audiohi == NULL)
		{
			avhuff->audiohi = create_huffman_decoder(AVHUFF_NUMCODES, AVHUFF_MAXBITS);
			if (avhuff->audiohi == NULL)
				return CHDERR_OUT_OF_MEMORY;
		}
		if (avhuff->audiolo == NULL)
		{
			avhuff->audiolo = create_huffman_decoder(AVHUFF_NUMCODES, AVHUFF_MAXBITS);
			if (avhuff->audiolo == NULL)
				return CHDERR_OUT_OF_MEMORY;
		}

		bitbuf = create_bitstream(source, treesize);
		if (bitbuf == NULL)
			return CHDERR_OUT_OF_MEMORY;
		hufferr = huffman_import_tree_rle(avhuff->audiohi, bitbuf);
		if (hufferr != HUFFERR_NONE) { free(bitbuf); return CHDERR_INVALID_DATA; }
		bitstream_flush(bitbuf);
		hufferr = huffman_import_tree_rle(avhuff->audiolo, bitbuf);
		if (hufferr != HUFFERR_NONE) { free(bitbuf); return CHDERR_INVALID_DATA; }
		if (bitstream_flush(bitbuf) != treesize) { free(bitbuf); return CHDERR_INVALID_DATA; }
		free(bitbuf);

		source += treesize;
	}

	for (chnum = 0; chnum < channels; chnum++)
	{
		uint16_t size = get_u16be(&sizes[chnum * 2 + 2]);
		uint8_t *curdest = audiostart[chnum];

		if (curdest != NULL)
		{
			int16_t prevsample = 0;

			if (treesize == 0)
			{
				/* raw big-endian s16 deltas */
				const uint8_t *cur = source;
				if (size < 2u * samples)  /* read only this channel's bytes */
					return CHDERR_INVALID_DATA;
				for (sampnum = 0; sampnum < samples; sampnum++)
				{
					int16_t delta = get_s16be(cur);
					int16_t newsample;
					cur += 2;
					newsample = (int16_t)(prevsample + delta);
					prevsample = newsample;
					curdest[0] = (uint8_t)(newsample >> 8);
					curdest[1] = (uint8_t)newsample;
					curdest += 2;
				}
			}
			else
			{
				/* huffman-coded deltas, hi/lo byte streams share the same bitbuf */
				bitbuf = create_bitstream(source, size);
				if (bitbuf == NULL)
					return CHDERR_OUT_OF_MEMORY;
				/* Reset deltarle state between channels */
				avhuff->ahi_prev = avhuff->alo_prev = 0;
				avhuff->ahi_rlecount = avhuff->alo_rlecount = 0;
				for (sampnum = 0; sampnum < samples; sampnum++)
				{
					int16_t delta;
					int16_t newsample;
					uint8_t hi = deltarle_decode_one(avhuff->audiohi, bitbuf,
					                                &avhuff->ahi_prev, &avhuff->ahi_rlecount);
					uint8_t lo = deltarle_decode_one(avhuff->audiolo, bitbuf,
					                                &avhuff->alo_prev, &avhuff->alo_rlecount);
					delta = (int16_t)(((uint16_t)hi << 8) | lo);
					newsample = (int16_t)(prevsample + delta);
					prevsample = newsample;
					curdest[0] = (uint8_t)(newsample >> 8);
					curdest[1] = (uint8_t)newsample;
					curdest += 2;
				}
				if (bitstream_overflow(bitbuf))
				{
					free(bitbuf);
					return CHDERR_INVALID_DATA;
				}
				free(bitbuf);
			}
		}

		source += size;
	}

	return CHDERR_NONE;
}

/***************************************************************************
    VIDEO DECODE (lossless only — lossy path rejected)
***************************************************************************/

static chd_error decode_video_lossless(avhuff_codec_data *avhuff,
                                       uint32_t width, uint32_t height,
                                       const uint8_t *source, uint32_t complen,
                                       uint8_t *dest, uint32_t dstride)
{
	struct bitstream *bitbuf;
	enum huffman_error hufferr;
	uint32_t dy, dx;

	bitbuf = create_bitstream(source, complen);
	if (bitbuf == NULL)
		return CHDERR_OUT_OF_MEMORY;

	/* skip the 1-byte flag that gated lossless vs lossy */
	bitstream_read(bitbuf, 8);

	hufferr = huffman_import_tree_rle(avhuff->ycontext, bitbuf);
	if (hufferr != HUFFERR_NONE) { free(bitbuf); return CHDERR_INVALID_DATA; }
	bitstream_flush(bitbuf);
	hufferr = huffman_import_tree_rle(avhuff->cbcontext, bitbuf);
	if (hufferr != HUFFERR_NONE) { free(bitbuf); return CHDERR_INVALID_DATA; }
	bitstream_flush(bitbuf);
	hufferr = huffman_import_tree_rle(avhuff->crcontext, bitbuf);
	if (hufferr != HUFFERR_NONE) { free(bitbuf); return CHDERR_INVALID_DATA; }
	bitstream_flush(bitbuf);

	/* Reset per-plane deltarle state before decoding rows */
	deltarle_reset(&avhuff->y_prev,  &avhuff->y_rlecount);
	deltarle_reset(&avhuff->cb_prev, &avhuff->cb_rlecount);
	deltarle_reset(&avhuff->cr_prev, &avhuff->cr_rlecount);

	for (dy = 0; dy < height; dy++)
	{
		uint8_t *row = dest + dy * dstride;
		for (dx = 0; dx < width / 2; dx++)
		{
			row[0] = deltarle_decode_one(avhuff->ycontext,  bitbuf,
			                             &avhuff->y_prev,  &avhuff->y_rlecount);
			row[1] = deltarle_decode_one(avhuff->cbcontext, bitbuf,
			                             &avhuff->cb_prev, &avhuff->cb_rlecount);
			row[2] = deltarle_decode_one(avhuff->ycontext,  bitbuf,
			                             &avhuff->y_prev,  &avhuff->y_rlecount);
			row[3] = deltarle_decode_one(avhuff->crcontext, bitbuf,
			                             &avhuff->cr_prev, &avhuff->cr_rlecount);
			row += 4;
		}
		/* flush RLE accumulator between rows (matches MAME) */
		deltarle_flush(&avhuff->y_rlecount);
		deltarle_flush(&avhuff->cb_rlecount);
		deltarle_flush(&avhuff->cr_rlecount);
	}

	if (bitstream_overflow(bitbuf) || bitstream_flush(bitbuf) != complen)
	{
		free(bitbuf);
		return CHDERR_INVALID_DATA;
	}
	free(bitbuf);
	return CHDERR_NONE;
}

/***************************************************************************
    TOP-LEVEL DECOMPRESS
***************************************************************************/

chd_error avhuff_codec_decompress(void *codec, const uint8_t *src, uint32_t complen,
                                  uint8_t *dest, uint32_t destlen)
{
	avhuff_codec_data *avhuff = (avhuff_codec_data *)codec;
	uint32_t metasize, channels, samples, width, height;
	uint32_t srcoffs, totalsize, treesize;
	uint8_t *metastart, *videostart;
	uint8_t *audiostart[16];
	uint32_t videostride;
	uint32_t chnum;
	uint64_t header_bytes;   /* 64-bit, 2*w*h wraps 32 bits */
	uint64_t payload_bytes;
	uint64_t written;

	if (complen < 8)
		return CHDERR_INVALID_DATA;

	metasize = src[0];
	channels = src[1];
	samples  = get_u16be(&src[2]);
	width    = get_u16be(&src[4]);
	height   = get_u16be(&src[6]);

	if (channels > 16)
		return CHDERR_INVALID_DATA;
	if (complen < 10u + 2u * channels)
		return CHDERR_INVALID_DATA;

	totalsize = 10u + 2u * channels + metasize;  /* the metadata is copied from src too */
	treesize  = get_u16be(&src[8]);
	if (treesize != 0xffff)
		totalsize += treesize;
	for (chnum = 0; chnum < channels; chnum++)
		totalsize += get_u16be(&src[10 + 2 * chnum]);
	if (totalsize >= complen)
		return CHDERR_INVALID_DATA;

	/* required output size: 12-byte 'chav' header + metadata + audio + video */
	header_bytes  = 12u;
	payload_bytes = (uint64_t)metasize + (uint64_t)2 * channels * samples + (uint64_t)2 * width * height;
	if (header_bytes + payload_bytes > (uint64_t)destlen)
		return CHDERR_DECOMPRESSION_ERROR;

	/* write destination 'chav' header */
	dest[0] = 'c';
	dest[1] = 'h';
	dest[2] = 'a';
	dest[3] = 'v';
	dest[4] = (uint8_t)metasize;
	dest[5] = (uint8_t)channels;
	put_u16be(&dest[6],  (uint16_t)samples);
	put_u16be(&dest[8],  (uint16_t)width);
	put_u16be(&dest[10], (uint16_t)height);

	/* map destination regions */
	metastart = dest + 12;
	{
		uint8_t *p = metastart + metasize;
		for (chnum = 0; chnum < channels; chnum++)
		{
			audiostart[chnum] = p;
			p += 2 * samples;
		}
		for (; chnum < 16; chnum++)
			audiostart[chnum] = NULL;
		videostart  = p;
		videostride = 2 * width;
	}

	srcoffs = 10u + 2u * channels;

	/* metadata: raw copy */
	if (metasize > 0)
	{
		memcpy(metastart, src + srcoffs, metasize);
		srcoffs += metasize;
	}

	/* audio */
	if (channels > 0)
	{
		chd_error err = decode_audio(avhuff, channels, samples, src + srcoffs,
		                             audiostart, &src[8]);
		if (err != CHDERR_NONE)
			return err;

		treesize = get_u16be(&src[8]);
		if (treesize != 0xffff)
			srcoffs += treesize;
		for (chnum = 0; chnum < channels; chnum++)
			srcoffs += get_u16be(&src[10 + 2 * chnum]);
	}

	/* video (lossless only) */
	if (width > 0 && height > 0)
	{
		chd_error err;
		if (srcoffs >= complen)
			return CHDERR_INVALID_DATA;
		/* reject non-lossless (MSB of first byte must be set) */
		if (!(src[srcoffs] & 0x80))
			return CHDERR_DECOMPRESSION_ERROR;
		err = decode_video_lossless(avhuff, width, height,
		                            src + srcoffs, complen - srcoffs,
		                            videostart, videostride);
		if (err != CHDERR_NONE)
			return err;
	}

	/* zero-pad any trailing space to match hunkbytes */
	written = header_bytes + payload_bytes;
	if (written < destlen)
		memset(dest + written, 0, destlen - written);

	return CHDERR_NONE;
}
