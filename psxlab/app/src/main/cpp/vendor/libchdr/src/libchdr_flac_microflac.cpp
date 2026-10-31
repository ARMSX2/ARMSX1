/* license:BSD-3-Clause
 * copyright-holders:Aaron Giles
 ***************************************************************************

    libchdr_flac_microflac.cpp

    micro-flac backend for the FLAC decoder interface, as an alternative to
    the dr_flac one in libchdr_flac.c. Selected at build time with
    CHDR_FLAC_BACKEND=microflac; exactly one backend is compiled.

    Why it exists: measured on hardware against dr_flac, decoding 10 s of CD
    audio at libchdr's cdfl geometry, micro-flac is 1.46x faster on an
    ESP32-S3 and 1.41x on an ESP32-P4, and its object code is smaller
    (27.7 KB vs 38.6 KB on RV32). Most of that comes from its C, not from its
    Xtensa assembly, so RISC-V targets benefit too.

    It is also a better fit for one thing libchdr needs. dr_flac exposes no
    way to ask how many input bytes it consumed, so flac_decoder_finish() in
    the dr_flac backend reconstructs that by reaching into the bit-reader's
    private cache state - and cdfl uses the answer to locate the subcode that
    follows the audio in the hunk. micro-flac's streaming decode() returns
    bytes_consumed directly. Verified equal on real cdfl blocks: both report
    6462, 6476, 6475, 6571 and 6798 bytes for the first five FLAC hunks of a
    PC Engine CD image, with byte-identical audio.

***************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "../include/libchdr/chdconfig.h"
#include "../include/libchdr/flac.h"
}

#include "micro_flac/flac_decoder.h"

using micro_flac::FLACDecoder;

/* The C struct reserves a fixed, aligned buffer for the C++ object so the
 * codecs can keep embedding flac_decoder by value. If micro-flac grows past
 * it this fails loudly at compile time rather than corrupting the struct. */
static_assert(sizeof(FLACDecoder) <= sizeof(((flac_decoder *)0)->impl),
              "flac_decoder::impl is too small for micro_flac::FLACDecoder");
static_assert(alignof(FLACDecoder) <= alignof(unsigned long long),
              "flac_decoder::impl is not aligned enough for micro_flac::FLACDecoder");

static FLACDecoder *impl_of(flac_decoder *decoder)
{
	return reinterpret_cast<FLACDecoder *>(decoder->impl);
}

/*-------------------------------------------------
 *  flac_decoder_init - construct in place
 *-------------------------------------------------
 */

extern "C" int flac_decoder_init(flac_decoder *decoder)
{
	memset(decoder, 0, sizeof(*decoder));
	new (static_cast<void *>(decoder->impl)) FLACDecoder();
	decoder->constructed = 1;
	/* libchdr verifies the per-hunk CRC itself where it is wanted, and the
	 * frame CRC is pure overhead on top of that. */
	impl_of(decoder)->set_crc_check_enabled(false);
	return 0;
}

/*-------------------------------------------------
 *  flac_decoder_free - destroy in place
 *-------------------------------------------------
 */

extern "C" void flac_decoder_free(flac_decoder *decoder)
{
	if (decoder == NULL)
		return;
	/* chd_close() frees every codec slot, including ones whose init() never
	 * ran - so impl[] may be the zeroed struct rather than a constructed
	 * object. Destroying that is undefined; init() sets the flag last. */
	if (!decoder->constructed)
		return;
	impl_of(decoder)->~FLACDecoder();
	decoder->constructed = 0;
	if (decoder->scratch != NULL) {
		free(decoder->scratch);
		decoder->scratch = NULL;
		decoder->scratch_size = 0;
	}
}

/*-------------------------------------------------
 *  flac_decoder_reset - start a new stream
 *-------------------------------------------------
 */

extern "C" int flac_decoder_reset(flac_decoder *decoder, uint32_t sample_rate,
	uint8_t num_channels, uint32_t block_size, const void *buffer, uint32_t length)
{
	/* A CHD stores raw FLAC frames with no header, so one is manufactured per
	 * hunk. The dr_flac backend writes block_size * num_channels into the
	 * block-size fields; this one writes block_size, which is what the format
	 * actually specifies - inter-channel samples, not interleaved ones.
	 *
	 * This is not cosmetic in both directions. micro-flac sizes its buffers
	 * from this field and re-checks the caller's output buffer against it on
	 * every call, so the inflated value costs twice the memory and forces
	 * every hunk through a scratch copy. But it also *rejects* any frame
	 * whose block size exceeds the field, so writing the smaller value
	 * narrows what this backend accepts: a stream whose frames are larger
	 * than the block size libchdr computes now fails where dr_flac would
	 * have decoded it.
	 *
	 * That is safe against what chdman writes, because libchdr derives the
	 * field from the same geometry chdman encoded to and both halve until
	 * they are under the same cap. It is not a margin. A third-party encoder
	 * emitting larger frames than the hunk geometry implies would decode
	 * under dr_flac and fail here.
	 *
	 * Verified byte-identical against the dr_flac backend on the whole
	 * corpus, in both settings of CHDR_CD_SCRATCH_BUFFER. */
	static const uint8_t s_header_template[0x2a] =
	{
		0x66, 0x4C, 0x61, 0x43,                         /* +00: 'fLaC' */
		0x80,                                           /* +04: STREAMINFO, last block */
		0x00, 0x00, 0x22,                               /* +05: length 0x22 */
		0x00, 0x00,                                     /* +08: minimum block size */
		0x00, 0x00,                                     /* +0A: maximum block size */
		0x00, 0x00, 0x00,                               /* +0C: minimum frame size */
		0x00, 0x00, 0x00,                               /* +0F: maximum frame size */
		0x0A, 0xC4, 0x42, 0xF0, 0x00, 0x00, 0x00, 0x00, /* +12: rate/channels/depth */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* +1A: MD5 (none) */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* +2A: stream data starts */
	};

	FLACDecoder *dec = impl_of(decoder);
	size_t consumed = 0, decoded = 0;

	memcpy(decoder->custom_header, s_header_template, sizeof(s_header_template));
	decoder->custom_header[0x08] = decoder->custom_header[0x0a] = (uint8_t)((block_size) >> 8);
	decoder->custom_header[0x09] = decoder->custom_header[0x0b] = (uint8_t)((block_size) & 0xff);
	decoder->custom_header[0x12] = (uint8_t)(sample_rate >> 12);
	decoder->custom_header[0x13] = (uint8_t)(sample_rate >> 4);
	decoder->custom_header[0x14] = (uint8_t)((sample_rate << 4) | ((num_channels - 1) << 1));

	decoder->payload = (const uint8_t *)buffer;
	decoder->payload_length = length;
	decoder->payload_consumed = 0;
	decoder->header_done = 0;
	decoder->alloc_failed = 0;

	dec->reset();
	dec->set_crc_check_enabled(false);

	/* Feed the header on its own. It yields HEADER_READY without needing an
	 * output buffer, which is why the header and the payload do not have to
	 * be stitched into one contiguous block first. */
	{
		const uint8_t *in = decoder->custom_header;
		size_t left = sizeof(decoder->custom_header);
		while (left > 0) {
			micro_flac::FLACDecoderResult r =
				dec->decode(in, left, (uint8_t *)NULL, (size_t)0, consumed, decoded);
			in += consumed;
			left -= consumed;
			if (r == micro_flac::FLAC_DECODER_HEADER_READY) {
				const micro_flac::FLACStreamInfo &si = dec->get_stream_info();
				decoder->sample_rate = si.sample_rate();
				decoder->channels = (uint8_t)si.num_channels();
				decoder->bits_per_sample = (uint8_t)si.bits_per_sample();
				decoder->header_done = 1;
				break;
			}
			if (r == micro_flac::FLAC_DECODER_ERROR_MEMORY_ALLOCATION) {
				decoder->alloc_failed = 1;
				return 0;
			}
			if (r != micro_flac::FLAC_DECODER_NEED_MORE_DATA && consumed == 0)
				return 0;
		}
	}

	return decoder->header_done;
}

/*-------------------------------------------------
 *  flac_decoder_decode_interleaved - decode a hunk
 *-------------------------------------------------
 */

extern "C" int flac_decoder_decode_interleaved(flac_decoder *decoder, int16_t *samples,
	uint32_t num_frames, int swap_endian)
{
	FLACDecoder *dec = impl_of(decoder);
	const uint8_t *in = decoder->payload + decoder->payload_consumed;
	size_t left = decoder->payload_length - decoder->payload_consumed;
	uint32_t want = num_frames * decoder->channels;   /* interleaved samples */
	uint32_t have = 0;

	if (!decoder->header_done)
		return 0;

	/* micro-flac checks the output buffer against a whole block on every
	 * call - max_block_size * channels * bytes_per_sample - so it will write
	 * where the caller wants only while that much room is left. For a CD hunk
	 * there is, for every block, so nothing is copied and no scratch is
	 * allocated at all. The tail of a stream whose last block would overrun
	 * the caller's buffer, and any geometry whose buffer is smaller than one
	 * block, fall back to decoding a block aside and copying the part that
	 * fits. The scratch is allocated only if that happens, and then kept. */
	uint32_t block_bytes;
	{
		const micro_flac::FLACStreamInfo &si = dec->get_stream_info();
		block_bytes = si.max_block_size() * si.num_channels() * si.bytes_per_sample();
	}

	while (have < want) {
		size_t consumed = 0, decoded = 0;
		size_t room = (size_t)(want - have) * sizeof(int16_t);
		int direct = (room >= block_bytes);
		uint8_t *out;
		size_t outsz;

		if (direct) {
			out = (uint8_t *)(samples + have);
			outsz = room;
		} else {
			if (decoder->scratch_size < block_bytes) {
				int16_t *p = (int16_t *)realloc(decoder->scratch, block_bytes);
				if (p == NULL) {
					decoder->alloc_failed = 1;
					return 0;
				}
				decoder->scratch = p;
				decoder->scratch_size = block_bytes;
			}
			out = (uint8_t *)decoder->scratch;
			outsz = decoder->scratch_size;
		}

		micro_flac::FLACDecoderResult r = dec->decode(in, left, out, outsz, consumed, decoded);

		in += consumed;
		left -= consumed;
		decoder->payload_consumed += (uint32_t)consumed;

		if (r == micro_flac::FLAC_DECODER_SUCCESS) {
			uint32_t take = (uint32_t)decoded;
			if (have + take > want)
				take = want - have;
			if (!direct)
				memcpy(samples + have, decoder->scratch, (size_t)take * sizeof(int16_t));
			have += take;
		} else if (r == micro_flac::FLAC_DECODER_END_OF_STREAM) {
			break;
		} else if (r == micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
			if (left == 0)
				break;
		} else if (r == micro_flac::FLAC_DECODER_ERROR_MEMORY_ALLOCATION) {
			decoder->alloc_failed = 1;
			return 0;
		} else if (r != micro_flac::FLAC_DECODER_HEADER_READY) {
			return 0;
		}
		if (consumed == 0 && decoded == 0 && r != micro_flac::FLAC_DECODER_SUCCESS)
			break;   /* no progress; do not spin */
	}

	if (have != want)
		return 0;

	/* CD audio is stored big-endian in a CHD, so the caller asks for a swap
	 * on a little-endian host. Same transform the dr_flac backend applies in
	 * its write callback. */
	if (swap_endian) {
		uint32_t i;
		for (i = 0; i < want; i++) {
			uint16_t v = (uint16_t)samples[i];
			samples[i] = (int16_t)((uint16_t)(v << 8) | (uint16_t)(v >> 8));
		}
	}
	return 1;
}

/*-------------------------------------------------
 *  flac_decoder_finish - bytes of payload consumed
 *-------------------------------------------------
 */

extern "C" uint32_t flac_decoder_finish(flac_decoder *decoder)
{
	/* cdfl uses this to find the subcode that follows the audio, so it must
	 * be the exact byte count - not a rounded-up frame boundary. */
	return decoder->payload_consumed;
}

/*-------------------------------------------------
 *  flac_decoder_detect_native_endian
 *-------------------------------------------------
 */

extern "C" int flac_decoder_detect_native_endian(void)
{
	uint16_t native_endian = 0;
	*(uint8_t *)(&native_endian) = 1;
	return (native_endian & 1);
}
