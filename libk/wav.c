#include <libk/wav.h>

#include <stddef.h>
#include <stdint.h>

/*
 * See libk/wav.h. Everything is little endian on disk; bytes are combined by
 * hand so the code is right on any host and never reads unaligned.
 */

#define WAV_FORMAT_PCM 0x0001U
#define WAV_FORMAT_IEEE_FLOAT 0x0003U
#define WAV_FORMAT_EXTENSIBLE 0xFFFEU

/* The tail of the KSDATAFORMAT_SUBTYPE_* GUIDs: {tag, 0x0000, 0x0000-0010-8000-00AA00389B71}. */
static const uint8_t g_wav_subtype_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

static uint32_t wav_u16(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U);
}

static uint32_t wav_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static int wav_tag(const uint8_t *bytes, const char *tag)
{
	return bytes[0] == (uint8_t)tag[0] && bytes[1] == (uint8_t)tag[1] && bytes[2] == (uint8_t)tag[2] && bytes[3] == (uint8_t)tag[3];
}

const char *wav_status_name(wav_status_t status)
{
	switch (status) {
	case WAV_OK: return "ok";
	case WAV_ERR_ARGUMENT: return "bad argument";
	case WAV_ERR_TOO_SHORT: return "too short to be a WAV file";
	case WAV_ERR_NOT_RIFF: return "not a RIFF file";
	case WAV_ERR_NOT_WAVE: return "a RIFF file, but not WAVE";
	case WAV_ERR_NO_FORMAT: return "no fmt chunk";
	case WAV_ERR_NO_DATA: return "no data chunk";
	case WAV_ERR_BAD_FORMAT: return "inconsistent fmt chunk";
	case WAV_ERR_UNSUPPORTED: return "unsupported sample format";
	default: return "?";
	}
}

const char *wav_encoding_name(wav_encoding_t encoding)
{
	switch (encoding) {
	case WAV_ENCODING_PCM: return "PCM";
	case WAV_ENCODING_FLOAT: return "float";
	default: return "?";
	}
}

/*
 * wav_read_format:
 *
 * Decode a fmt chunk of `size` bytes at `chunk` (the bytes are all inside the
 * caller's buffer) into *info, and check that it describes a stream this
 * reader can play and that its numbers agree with each other.
 */
static wav_status_t wav_read_format(const uint8_t *chunk, uint64_t size, wav_info_t *info)
{
	if (size < 16U) return WAV_ERR_BAD_FORMAT;

	uint32_t tag = wav_u16(chunk);

	info->channels = wav_u16(chunk + 2U);
	info->sample_rate = wav_u32(chunk + 4U);
	info->block_align = wav_u16(chunk + 12U);
	info->bits_per_sample = wav_u16(chunk + 14U);
	info->valid_bits = info->bits_per_sample;
	info->channel_mask = 0U;
	info->extensible = 0;

	if (tag == WAV_FORMAT_EXTENSIBLE) {
		/* cbSize (22), the valid bits, the speaker mask and the sub-format GUID follow the basic 16 bytes. */
		if (size < 40U || wav_u16(chunk + 16U) < 22U) return WAV_ERR_BAD_FORMAT;

		info->extensible = 1;
		info->valid_bits = wav_u16(chunk + 18U);
		info->channel_mask = wav_u32(chunk + 20U);
		tag = wav_u16(chunk + 24U);

		for (uint32_t index = 0U; index < sizeof(g_wav_subtype_tail); index++) {
			if (chunk[26U + index] != g_wav_subtype_tail[index]) return WAV_ERR_UNSUPPORTED;
		}

		if (info->valid_bits == 0U || info->valid_bits > info->bits_per_sample) return WAV_ERR_BAD_FORMAT;
	}

	if (tag == WAV_FORMAT_PCM) {
		info->encoding = WAV_ENCODING_PCM;
	} else if (tag == WAV_FORMAT_IEEE_FLOAT) {
		info->encoding = WAV_ENCODING_FLOAT;
	} else {
		return WAV_ERR_UNSUPPORTED;
	}

	if (info->channels == 0U || info->channels > WAV_CHANNELS_MAX) return WAV_ERR_BAD_FORMAT;
	if (info->sample_rate == 0U || info->sample_rate > WAV_SAMPLE_RATE_MAX) return WAV_ERR_BAD_FORMAT;

	if (info->encoding == WAV_ENCODING_PCM) {
		if (info->bits_per_sample != 8U && info->bits_per_sample != 16U && info->bits_per_sample != 24U && info->bits_per_sample != 32U) return WAV_ERR_UNSUPPORTED;
	} else if (info->bits_per_sample != 32U && info->bits_per_sample != 64U) {
		return WAV_ERR_UNSUPPORTED;
	}

	if (info->block_align != info->channels * (info->bits_per_sample / 8U)) return WAV_ERR_BAD_FORMAT;

	return WAV_OK;
}

wav_status_t wav_parse(const uint8_t *bytes, size_t size, wav_info_t *info)
{
	if (info == 0) return WAV_ERR_ARGUMENT;

	*info = (wav_info_t) { 0 };

	if (bytes == 0) return WAV_ERR_ARGUMENT;
	if (size < 12U) return WAV_ERR_TOO_SHORT;
	if (!wav_tag(bytes, "RIFF")) return WAV_ERR_NOT_RIFF;
	if (!wav_tag(bytes + 8U, "WAVE")) return WAV_ERR_NOT_WAVE;

	wav_info_t found = { 0 };
	int have_format = 0;
	int have_data = 0;
	uint64_t total = size;
	uint64_t offset = 12U;

	/*
	 * The RIFF size is not consulted. Each pass consumes at least the 8-byte
	 * chunk header, so the loop ends; every size is compared against what is
	 * really left before it is used.
	 */
	while (offset + 8U <= total && !(have_format && have_data)) {
		const uint8_t *header = bytes + offset;
		uint64_t chunk_size = wav_u32(header + 4U);
		uint64_t body = offset + 8U;
		uint64_t remaining = total - body;

		if (wav_tag(header, "fmt ") && !have_format) {
			if (chunk_size > remaining) return WAV_ERR_BAD_FORMAT;

			wav_status_t status = wav_read_format(bytes + body, chunk_size, &found);

			if (status != WAV_OK) return status;

			have_format = 1;
		} else if (wav_tag(header, "data") && !have_data) {
			have_data = 1;
			found.data = bytes + body;

			if (chunk_size > remaining) {
				chunk_size = remaining;
				found.data_truncated = 1;
			}

			found.data_bytes = chunk_size;
		}

		/* A chunk's data is padded to an even length; a size that runs past the end ends the walk. */
		uint64_t next = body + chunk_size + (chunk_size & 1U);

		if (chunk_size > remaining || next > total) break;

		offset = next;
	}

	if (!have_format) return WAV_ERR_NO_FORMAT;
	if (!have_data) return WAV_ERR_NO_DATA;

	/* Only whole frames are samples: a data chunk that ends inside a frame loses the fragment. */
	found.frames = found.data_bytes / found.block_align;
	found.data_bytes = found.frames * found.block_align;

	*info = found;
	return WAV_OK;
}

/* ---- conversion -------------------------------------------------------- */

/*
 * wav_float_to_s16:
 *
 * round(v * 32767) for v given as sign, biased exponent and a 24-bit
 * significand with its implicit bit (a float32; a float64 passes its top 24
 * bits), by integer arithmetic only: the kernel and user programs are built
 * without floating-point registers. Magnitudes of 1 and over clamp, zero and
 * denormals are 0, NaN is 0.
 */
static int16_t wav_float_to_s16(uint32_t negative, uint32_t exponent, uint32_t mantissa, uint32_t exponent_max)
{
	uint32_t bias = exponent_max / 2U;

	if (exponent == exponent_max) return mantissa != 0U ? 0 : (negative ? -32768 : 32767);
	if (exponent == 0U) return 0;
	if (exponent >= bias) return negative ? -32768 : 32767;

	uint64_t shift = 23U + (bias - exponent);

	if (shift >= 63U) return 0;

	uint64_t product = ((uint64_t)(0x800000U | mantissa)) * 32767U;
	int64_t rounded = (int64_t)((product + (1ULL << (shift - 1U))) >> shift);

	return (int16_t)(negative ? -rounded : rounded);
}

static int16_t wav_sample_s16(const wav_info_t *info, const uint8_t *sample)
{
	if (info->encoding == WAV_ENCODING_PCM) {
		switch (info->bits_per_sample) {
		case 8U: return (int16_t)(((int32_t)sample[0] - 128) * 256);
		case 16U: return (int16_t)wav_u16(sample);
		case 24U: return (int16_t)wav_u16(sample + 1U);
		default: return (int16_t)wav_u16(sample + 2U);
		}
	}

	if (info->bits_per_sample == 32U) {
		uint32_t bits = wav_u32(sample);

		return wav_float_to_s16(bits >> 31U, (bits >> 23U) & 0xFFU, bits & 0x7FFFFFU, 0xFFU);
	}

	/* float64: 11 exponent bits and 52 of significand; the top 23 of those are all a 16-bit result can use. */
	uint64_t low = wav_u32(sample);
	uint64_t high = wav_u32(sample + 4U);
	uint32_t exponent = (uint32_t)((high >> 20U) & 0x7FFU);
	uint32_t mantissa = (uint32_t)(((high & 0xFFFFFU) << 3U) | (low >> 29U));

	if (exponent == 0x7FFU) return ((high & 0xFFFFFU) | low) != 0ULL ? 0 : ((high >> 31U) ? -32768 : 32767);

	return wav_float_to_s16((uint32_t)(high >> 31U), exponent, mantissa, 0x7FFU);
}

size_t wav_convert_s16_stereo(const wav_info_t *info, uint64_t first_frame, int16_t *out, size_t out_frames)
{
	if (info == 0 || out == 0 || info->data == 0 || info->block_align == 0U || info->channels == 0U) return 0U;
	if (first_frame >= info->frames) return 0U;

	uint64_t available = info->frames - first_frame;
	size_t count = available < out_frames ? (size_t)available : out_frames;
	uint32_t width = info->bits_per_sample / 8U;
	const uint8_t *frame = info->data + first_frame * info->block_align;

	for (size_t index = 0U; index < count; index++, frame += info->block_align) {
		int16_t left = wav_sample_s16(info, frame);
		int16_t right = info->channels > 1U ? wav_sample_s16(info, frame + width) : left;

		out[2U * index] = left;
		out[2U * index + 1U] = right;
	}

	return count;
}
