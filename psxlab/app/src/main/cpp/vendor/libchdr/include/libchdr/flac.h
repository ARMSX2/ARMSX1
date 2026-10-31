/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
 ***************************************************************************

    flac.h

    FLAC compression wrappers

***************************************************************************/

#pragma once

#ifndef __FLAC_H__
#define __FLAC_H__

#include <stddef.h>
#include <stdint.h>

/***************************************************************************
 *  TYPE DEFINITIONS
 ***************************************************************************
 */

typedef struct _flac_decoder flac_decoder;

#if defined(CHDR_FLAC_BACKEND_MICROFLAC)

/* micro-flac backend. The decoder is a C++ object, but this header is included
 * from C, so it is placement-new'd into an opaque buffer here rather than
 * heap-allocated - the codecs embed flac_decoder by value and an allocation
 * per codec instance is exactly what the dr_flac arena work removed.
 * libchdr_flac_microflac.cpp static_asserts that the object fits. */
struct _flac_decoder {
	uint32_t                sample_rate;
	uint8_t                 channels;
	uint8_t                 bits_per_sample;
	int                     alloc_failed;			/* set when a decode could not allocate */
	const uint8_t *         payload;				/* compressed data after the synthesised header */
	uint32_t                payload_length;
	uint32_t                payload_consumed;		/* what finish() reports */
	int                     header_done;			/* STREAMINFO parsed for this stream */
	uint8_t                 custom_header[0x2a];	/* synthesised STREAMINFO */
	/* micro-flac sizes its output buffer as max_block_size * channels *
	 * bytes_per_sample, and libchdr's synthesised header carries
	 * block_size * channels in max_block_size - so it asks for twice the
	 * bytes a cdfl hunk's audio actually occupies. Decode into this and copy
	 * out. Retained across hunks, like the dr_flac backend's block. */
	int16_t *               scratch;
	uint32_t                scratch_size;
	int                     constructed;			/* impl[] holds a live object */
	unsigned long long      impl[64];				/* opaque, 8-byte aligned */
};

#else

struct _flac_decoder {
		/* output state */
	void *                  decoder;				/* actual encoder */
	uint32_t                sample_rate;			/* decoded sample rate */
	uint8_t                 channels;				/* decoded number of channels */
	uint8_t                 bits_per_sample;		/* decoded bits per sample */
	uint32_t                compressed_offset;		/* current offset in compressed data */
	const uint8_t *         compressed_start;		/* start of compressed data */
	uint32_t                compressed_length;		/* length of compressed data */
	const uint8_t *         compressed2_start;		/* start of compressed data */
	uint32_t                compressed2_length;		/* length of compressed data */
	int16_t *               uncompressed_start[8];	/* pointer to start of uncompressed data (up to 8 streams) */
	uint32_t                uncompressed_offset;	/* current position in uncompressed data */
	uint32_t                uncompressed_length;	/* length of uncompressed data */
	int                    	uncompressed_swap;		/* swap uncompressed sample data */
	int                     alloc_failed;			/* set when the last reset() failed to allocate */
	/* dr_flac takes its decoder, sample buffer and bit-reader cache as one
	 * block, sized from the STREAMINFO block size - identical for every hunk
	 * of a given CHD. reset() runs per hunk, so that block was being allocated
	 * and freed thousands of times; retaining it keeps the heap still. */
	void *                  arena;					/* retained dr_flac working block */
	size_t                  arena_size;				/* its size, 0 if none */
	int                     arena_busy;				/* lent to dr_flac right now */
	uint8_t                 custom_header[0x2a];	/* custom header */
};

#endif /* CHDR_FLAC_BACKEND_MICROFLAC */

/* ======================> flac_decoder */

int 		flac_decoder_init(flac_decoder* decoder);
void 		flac_decoder_free(flac_decoder* decoder);
int 		flac_decoder_reset(flac_decoder* decoder, uint32_t sample_rate, uint8_t num_channels, uint32_t block_size, const void *buffer, uint32_t length);
int 		flac_decoder_decode_interleaved(flac_decoder* decoder, int16_t *samples, uint32_t num_samples, int swap_endian);
uint32_t 	flac_decoder_finish(flac_decoder* decoder);
int			flac_decoder_detect_native_endian(void);

#endif /* __FLAC_H__ */
