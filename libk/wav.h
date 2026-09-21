/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        libk/wav.h
 *
 * A RIFF/WAVE reader for the kernel (the boot chime) and for user programs
 * (playsound), and the conversion to what the audio device plays.
 *
 * Freestanding on purpose: it needs nothing but <stdint.h> and <stddef.h>, no
 * allocation, no libc and no floating-point instructions (the kernel and user
 * programs are built without them), so the same file builds natively for the
 * host test, which runs it under ASan and UBSan against truncated, mutated
 * and hostile input (tools/audio/test_host.sh).
 *
 * The parser never trusts a size in the file. It finds the format and data
 * chunks by walking the chunks it can actually see, skips everything else
 * (LIST, fact, cue, ...), honours the pad byte after an odd-sized chunk,
 * accepts a RIFF size that is wrong (streamed and recorded files often have 0
 * or 0xFFFFFFFF there) and a data chunk that claims more than the file holds,
 * and reports what is malformed. The returned data pointer points into the
 * caller's buffer.
 */

#ifndef NXU_LIBK_WAV_H
#define NXU_LIBK_WAV_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	WAV_OK = 0,
	WAV_ERR_ARGUMENT,
	WAV_ERR_TOO_SHORT, /* smaller than a RIFF header */
	WAV_ERR_NOT_RIFF,
	WAV_ERR_NOT_WAVE,
	WAV_ERR_NO_FORMAT, /* no fmt chunk before the end of the file */
	WAV_ERR_NO_DATA, /* no data chunk */
	WAV_ERR_BAD_FORMAT, /* a fmt chunk that does not describe a consistent stream */
	WAV_ERR_UNSUPPORTED /* a valid stream of a kind this reader does not handle (ADPCM, MP3, 12-bit, ...) */
} wav_status_t;

typedef enum {
	WAV_ENCODING_PCM = 1, /* integers: unsigned for 8 bits, signed two's complement above */
	WAV_ENCODING_FLOAT = 3 /* IEEE 754 */
} wav_encoding_t;

/* Sanity limits: a stream outside them is reported as a bad format, not played. */
#define WAV_CHANNELS_MAX 8U
#define WAV_SAMPLE_RATE_MAX 768000U

typedef struct {
	wav_encoding_t encoding;
	uint32_t channels;
	uint32_t sample_rate;
	uint32_t bits_per_sample; /* width of one sample in the file: 8, 16, 24 or 32 (float: 32 or 64) */
	uint32_t valid_bits; /* WAVE_FORMAT_EXTENSIBLE only: the significant bits, else the same as bits_per_sample */
	uint32_t block_align; /* bytes in one frame */
	uint32_t channel_mask; /* WAVE_FORMAT_EXTENSIBLE only, else 0 */
	int extensible;

	const uint8_t *data; /* the samples, inside the input */
	uint64_t data_bytes; /* whole frames only */
	uint64_t frames;
	int data_truncated; /* the data chunk claimed more than the file holds; what is there is used */
} wav_info_t;

/*
 * wav_parse:
 *
 * Read the header of the WAVE file in bytes[0..size). On WAV_OK *info describes
 * the stream and points at its samples. On any other status *info is zeroed.
 */
wav_status_t wav_parse(const uint8_t *bytes, size_t size, wav_info_t *info);

const char *wav_status_name(wav_status_t status);

/* "PCM" or "float", and the words the log wants: "16-bit PCM", "32-bit float". */
const char *wav_encoding_name(wav_encoding_t encoding);

/*
 * wav_convert_s16_stereo:
 *
 * Convert up to out_frames frames of the stream, from frame first_frame on,
 * to 16-bit little-endian stereo at the stream's own rate, into out
 * (out_frames * 2 samples). Every encoding wav_parse accepts is handled:
 * 8-bit unsigned, 16, 24 and 32-bit signed, 32 and 64-bit float (clamped, NaN
 * is silence). A mono stream is put on both channels; a stream with more
 * than two channels keeps its first two. Returns the number of frames written,
 * 0 past the end of the data. The sample rate is not changed: the device
 * takes what the format negotiation gives, and a rate it lacks is a matter
 * for the caller.
 */
size_t wav_convert_s16_stereo(const wav_info_t *info, uint64_t first_frame, int16_t *out, size_t out_frames);

#endif
