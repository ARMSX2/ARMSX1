/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
***************************************************************************

    bitstream.c

    Helper classes for reading/writing at the bit level.

***************************************************************************/

#include <stdlib.h>
#include "../include/libchdr/bitstream.h"

/***************************************************************************
 *  INLINE FUNCTIONS
 ***************************************************************************
 */

int bitstream_overflow(struct bitstream* bitstream) { return ((bitstream->doffset - bitstream->bits / 8) > bitstream->dlength); }

/*-------------------------------------------------
 *  create_bitstream - constructor
 *-------------------------------------------------
 */

struct bitstream* create_bitstream(const void *src, uint32_t srclength)
{
	struct bitstream* bitstream = (struct bitstream*)malloc(sizeof(struct bitstream));
	bitstream->buffer = 0;
	bitstream->bits = 0;
	bitstream->read = (const uint8_t*)src;
	bitstream->doffset = 0;
	bitstream->dlength = srclength;
	return bitstream;
}


/*-----------------------------------------------------
 *  bitstream_peek - fetch the requested number of bits
 *  but don't advance the input pointer
 *-----------------------------------------------------
 */

uint32_t bitstream_peek(struct bitstream* bitstream, int numbits)
{
	if (numbits == 0)
		return 0;

	/* fetch data if we need more */
	if (numbits > bitstream->bits)
	{
		while (bitstream->bits <= 24)
		{
			/* bits goes negative once a stream has been over-consumed, which
			 * malformed input can provoke, and then 24 - bits reaches 32 and
			 * the shift is undefined. A byte shifted that far lands entirely
			 * above bit 31, so contributing nothing is also the arithmetically
			 * correct result - well-formed streams keep bits >= 0 and never
			 * take this branch. */
			const int shift = 24 - bitstream->bits;

			if (bitstream->doffset < bitstream->dlength && shift < 32)
				bitstream->buffer |= (uint32_t)bitstream->read[bitstream->doffset] << shift;
			bitstream->doffset++;
			bitstream->bits += 8;
		}
	}

	/* return the data */
	return bitstream->buffer >> (32 - numbits);
}


/*-----------------------------------------------------
 *  bitstream_remove - advance the input pointer by the
 *  specified number of bits
 *-----------------------------------------------------
 */

void bitstream_remove(struct bitstream* bitstream, int numbits)
{
	/* buffer is 32 bits wide, so shifting by 32 is undefined even though
	 * consuming all 32 is a legitimate request - peek() already returns the
	 * whole buffer for that width. */
	bitstream->buffer = (numbits >= 32) ? 0 : (bitstream->buffer << numbits);
	bitstream->bits -= numbits;
}


/*-----------------------------------------------------
 *  bitstream_read - fetch the requested number of bits
 *-----------------------------------------------------
 */

uint32_t bitstream_read(struct bitstream* bitstream, int numbits)
{
	uint32_t result = bitstream_peek(bitstream, numbits);
	bitstream_remove(bitstream, numbits);
	return result;
}


/*-------------------------------------------------
 *  read_offset - return the current read offset
 *-------------------------------------------------
 */

uint32_t bitstream_read_offset(struct bitstream* bitstream)
{
	uint32_t result = bitstream->doffset;
	int bits = bitstream->bits;
	while (bits >= 8)
	{
		result--;
		bits -= 8;
	}
	return result;
}


/*-------------------------------------------------
 *  flush - flush to the nearest byte
 *-------------------------------------------------
 */

/*-------------------------------------------------
 *  bitstream_position_bits - exact bit-granular
 *  read position (unlike bitstream_read_offset,
 *  which is only meaningful at a byte boundary)
 *-------------------------------------------------
 */

uint64_t bitstream_position_bits(struct bitstream* bitstream)
{
	return (uint64_t)bitstream->doffset * 8 - (uint64_t)bitstream->bits;
}


/*-------------------------------------------------
 *  bitstream_seek_bits - reposition to an exact
 *  bit-granular offset, previously obtained from
 *  bitstream_position_bits on the same buffer
 *-------------------------------------------------
 */

void bitstream_seek_bits(struct bitstream* bitstream, uint64_t bitpos)
{
	int rem = (int)(bitpos % 8);
	bitstream->doffset = (uint32_t)(bitpos / 8);
	bitstream->bits = 0;
	bitstream->buffer = 0;
	if (rem)
		bitstream_read(bitstream, rem);
}


/*-------------------------------------------------
 *  flush - flush to the nearest byte
 *-------------------------------------------------
 */

uint32_t bitstream_flush(struct bitstream* bitstream)
{
	while (bitstream->bits >= 8)
	{
		bitstream->doffset--;
		bitstream->bits -= 8;
	}
	bitstream->bits = bitstream->buffer = 0;
	return bitstream->doffset;
}

