/*
 * Host test for the WAV reader and converter (libk/wav.c): well-formed files
 * of every kind it accepts, files it must refuse and say why, every prefix of a
 * good file, and a seeded mutation sweep. Built and run natively with ASan and
 * UBSan by tools/audio/test_host.sh, so an out-of-bounds read, an overflow or
 * an unbounded loop stops the run. The input buffers are heap allocations of
 * exactly the size handed to the parser, so ASan sees any read past the end.
 */

#include <libk/wav.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_checks;
static unsigned int g_failures;

#define CHECK(condition) \
	do { \
		g_checks++; \
		if (!(condition)) { \
			g_failures++; \
			printf("FAIL  %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

/* ---- building files ---------------------------------------------------- */

typedef struct {
	uint8_t *bytes;
	size_t size;
	size_t capacity;
} builder_t;

static void put(builder_t *b, const void *data, size_t size)
{
	if (b->size + size > b->capacity) {
		b->capacity = (b->size + size) * 2U + 64U;
		b->bytes = realloc(b->bytes, b->capacity);
	}

	memcpy(b->bytes + b->size, data, size);
	b->size += size;
}

static void put16(builder_t *b, uint32_t value)
{
	uint8_t bytes[2] = { (uint8_t)value, (uint8_t)(value >> 8U) };

	put(b, bytes, 2U);
}

static void put32(builder_t *b, uint32_t value)
{
	uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8U), (uint8_t)(value >> 16U), (uint8_t)(value >> 24U) };

	put(b, bytes, 4U);
}

static void put_tag(builder_t *b, const char *tag)
{
	put(b, tag, 4U);
}

static void patch32(builder_t *b, size_t offset, uint32_t value)
{
	b->bytes[offset] = (uint8_t)value;
	b->bytes[offset + 1U] = (uint8_t)(value >> 8U);
	b->bytes[offset + 2U] = (uint8_t)(value >> 16U);
	b->bytes[offset + 3U] = (uint8_t)(value >> 24U);
}

static void riff_begin(builder_t *b)
{
	memset(b, 0, sizeof(*b));
	put_tag(b, "RIFF");
	put32(b, 0U);
	put_tag(b, "WAVE");
}

static void riff_end(builder_t *b)
{
	patch32(b, 4U, (uint32_t)(b->size - 8U));
}

static void fmt_chunk(builder_t *b, uint32_t tag, uint32_t channels, uint32_t rate, uint32_t bits)
{
	put_tag(b, "fmt ");
	put32(b, 16U);
	put16(b, tag);
	put16(b, channels);
	put32(b, rate);
	put32(b, rate * channels * (bits / 8U));
	put16(b, channels * (bits / 8U));
	put16(b, bits);
}

static void fmt_extensible(builder_t *b, uint32_t subtag, uint32_t channels, uint32_t rate, uint32_t bits, uint32_t valid, uint32_t mask)
{
	static const uint8_t tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

	put_tag(b, "fmt ");
	put32(b, 40U);
	put16(b, 0xFFFEU);
	put16(b, channels);
	put32(b, rate);
	put32(b, rate * channels * (bits / 8U));
	put16(b, channels * (bits / 8U));
	put16(b, bits);
	put16(b, 22U);
	put16(b, valid);
	put32(b, mask);
	put16(b, subtag);
	put(b, tail, sizeof(tail));
}

static void data_chunk(builder_t *b, const uint8_t *samples, size_t size)
{
	put_tag(b, "data");
	put32(b, (uint32_t)size);
	put(b, samples, size);

	if ((size & 1U) != 0U) {
		uint8_t pad = 0U;

		put(b, &pad, 1U);
	}
}

static void list_chunk(builder_t *b, size_t size)
{
	put_tag(b, "LIST");
	put32(b, (uint32_t)size);

	for (size_t index = 0U; index < size; index++) {
		uint8_t byte = (uint8_t)(0x40U + index % 26U);

		put(b, &byte, 1U);
	}

	if ((size & 1U) != 0U) {
		uint8_t pad = 0U;

		put(b, &pad, 1U);
	}
}

/* A copy in an allocation of exactly `size` bytes, for ASan. */
static uint8_t *exact(const uint8_t *bytes, size_t size)
{
	uint8_t *copy = malloc(size == 0U ? 1U : size);

	if (size != 0U) memcpy(copy, bytes, size);
	return copy;
}

static wav_status_t parse_exact(const uint8_t *bytes, size_t size, wav_info_t *info)
{
	uint8_t *copy = exact(bytes, size);
	wav_status_t status = wav_parse(copy, size, info);

	/* Every byte the parser hands back must lie inside the copy. */
	if (status == WAV_OK && info->data_bytes != 0U) {
		CHECK(info->data >= copy && info->data + info->data_bytes <= copy + size);
	}

	free(copy);
	return status;
}

/* ---- accepted files ---------------------------------------------------- */

static void test_formats(void)
{
	static const struct {
		uint32_t tag;
		uint32_t channels;
		uint32_t bits;
	} cases[] = {
		{ 1U, 1U, 8U }, { 1U, 2U, 8U }, { 1U, 1U, 16U }, { 1U, 2U, 16U }, { 1U, 2U, 24U }, { 1U, 1U, 24U },
		{ 1U, 2U, 32U }, { 1U, 6U, 16U }, { 3U, 1U, 32U }, { 3U, 2U, 32U }, { 3U, 2U, 64U }, { 1U, 8U, 16U }
	};
	uint8_t samples[512];

	for (size_t index = 0U; index < sizeof(samples); index++) samples[index] = (uint8_t)(index * 7U);

	for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
		builder_t b;
		wav_info_t info;
		uint32_t block = cases[index].channels * (cases[index].bits / 8U);

		riff_begin(&b);
		fmt_chunk(&b, cases[index].tag, cases[index].channels, 44100U, cases[index].bits);
		data_chunk(&b, samples, 6U * block);
		riff_end(&b);

		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK);
		CHECK(info.channels == cases[index].channels);
		CHECK(info.bits_per_sample == cases[index].bits);
		CHECK(info.sample_rate == 44100U);
		CHECK(info.block_align == block);
		CHECK(info.frames == 6U);
		CHECK(info.data_bytes == 6U * block);
		CHECK(!info.data_truncated);
		CHECK(info.encoding == (cases[index].tag == 3U ? WAV_ENCODING_FLOAT : WAV_ENCODING_PCM));
		CHECK(!info.extensible);
		free(b.bytes);
	}
}

static void test_extensible(void)
{
	uint8_t samples[32] = { 1 };
	builder_t b;
	wav_info_t info;

	riff_begin(&b);
	fmt_extensible(&b, 1U, 2U, 48000U, 24U, 20U, 3U);
	data_chunk(&b, samples, 24U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK);
	CHECK(info.extensible && info.encoding == WAV_ENCODING_PCM && info.bits_per_sample == 24U && info.valid_bits == 20U);
	CHECK(info.channel_mask == 3U && info.sample_rate == 48000U && info.frames == 4U);
	free(b.bytes);

	riff_begin(&b);
	fmt_extensible(&b, 3U, 2U, 44100U, 32U, 32U, 3U);
	data_chunk(&b, samples, 16U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.encoding == WAV_ENCODING_FLOAT && info.frames == 2U);
	free(b.bytes);

	/* valid bits wider than the container, or zero */
	riff_begin(&b);
	fmt_extensible(&b, 1U, 2U, 48000U, 16U, 24U, 3U);
	data_chunk(&b, samples, 8U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
	free(b.bytes);

	riff_begin(&b);
	fmt_extensible(&b, 1U, 2U, 48000U, 16U, 0U, 3U);
	data_chunk(&b, samples, 8U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
	free(b.bytes);

	/* a sub-format that is not PCM or float (ADPCM is 2) */
	riff_begin(&b);
	fmt_extensible(&b, 2U, 2U, 48000U, 16U, 16U, 3U);
	data_chunk(&b, samples, 8U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_UNSUPPORTED);
	free(b.bytes);

	/* a foreign GUID */
	riff_begin(&b);
	fmt_extensible(&b, 1U, 2U, 48000U, 16U, 16U, 3U);
	b.bytes[b.size - 1U] ^= 0xFFU;
	data_chunk(&b, samples, 8U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_UNSUPPORTED);
	free(b.bytes);

	/* a fmt chunk that says extensible but is too short for it */
	riff_begin(&b);
	fmt_chunk(&b, 0xFFFEU, 2U, 44100U, 16U);
	data_chunk(&b, samples, 8U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
	free(b.bytes);
}

static void test_chunk_walking(void)
{
	uint8_t samples[16];
	builder_t b;
	wav_info_t info;

	for (size_t index = 0U; index < sizeof(samples); index++) samples[index] = (uint8_t)(0x10U + index);

	/* LIST before fmt, odd-sized (needs its pad byte), another chunk between fmt and data, and one after. */
	riff_begin(&b);
	list_chunk(&b, 13U);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	put_tag(&b, "fact");
	put32(&b, 4U);
	put32(&b, 4U);
	list_chunk(&b, 5U);
	data_chunk(&b, samples, 16U);
	list_chunk(&b, 7U);
	riff_end(&b);
	CHECK(wav_parse(b.bytes, b.size, &info) == WAV_OK);
	CHECK(info.frames == 4U && info.data[0] == 0x10U && info.data[15] == 0x1FU);
	free(b.bytes);

	/* an odd-sized data chunk: the pad byte is not a sample, the fragment of a frame is dropped */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	data_chunk(&b, samples, 7U);
	list_chunk(&b, 3U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK);
	CHECK(info.frames == 1U && info.data_bytes == 4U);
	free(b.bytes);

	/* data before fmt */
	riff_begin(&b);
	data_chunk(&b, samples, 16U);
	fmt_chunk(&b, 1U, 1U, 8000U, 16U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.frames == 8U && info.sample_rate == 8000U);
	free(b.bytes);

	/* two fmt and two data chunks: the first of each counts */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	fmt_chunk(&b, 1U, 1U, 8000U, 8U);
	data_chunk(&b, samples, 8U);
	data_chunk(&b, samples, 16U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.sample_rate == 44100U && info.frames == 2U);
	free(b.bytes);

	/* a fmt chunk longer than 16 bytes (a cbSize of zero) */
	riff_begin(&b);
	put_tag(&b, "fmt ");
	put32(&b, 18U);
	put16(&b, 1U);
	put16(&b, 2U);
	put32(&b, 44100U);
	put32(&b, 176400U);
	put16(&b, 4U);
	put16(&b, 16U);
	put16(&b, 0U);
	data_chunk(&b, samples, 16U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.frames == 4U);
	free(b.bytes);

	/* an empty data chunk is a file with no frames, not an error */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	data_chunk(&b, samples, 0U);
	riff_end(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.frames == 0U && info.data_bytes == 0U);
	free(b.bytes);
}

static void test_sizes_that_lie(void)
{
	uint8_t samples[16] = { 1 };
	builder_t b;
	wav_info_t info;

	/* A RIFF size of 0, of 0xFFFFFFFF, or too small: not consulted. */
	static const uint32_t riff_sizes[] = { 0U, 0xFFFFFFFFU, 4U, 0x7FFFFFFFU };

	for (size_t index = 0U; index < sizeof(riff_sizes) / sizeof(riff_sizes[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, 1U, 2U, 44100U, 16U);
		data_chunk(&b, samples, 16U);
		patch32(&b, 4U, riff_sizes[index]);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.frames == 4U && !info.data_truncated);
		free(b.bytes);
	}

	/* The data chunk claims more than the file holds (a recording cut short): what is there is used. */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	data_chunk(&b, samples, 16U);
	patch32(&b, b.size - 16U - 4U, 1000U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.data_truncated && info.frames == 4U);
	free(b.bytes);

	/* ... or 4 GiB - 1, the value a streaming writer leaves there. */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	data_chunk(&b, samples, 16U);
	patch32(&b, b.size - 16U - 4U, 0xFFFFFFFFU);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_OK && info.data_truncated && info.frames == 4U);
	free(b.bytes);

	/* An unknown chunk before the data claims 4 GiB - 1: the walk ends there and there is no data. */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	put_tag(&b, "junk");
	put32(&b, 0xFFFFFFFFU);
	data_chunk(&b, samples, 16U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NO_DATA);
	free(b.bytes);

	/* ... and one whose padded size overflows 32 bits. */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	put_tag(&b, "junk");
	put32(&b, 0xFFFFFFFFU);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NO_DATA);
	free(b.bytes);

	/* A fmt chunk whose size runs off the end. */
	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	patch32(&b, 16U, 0x10000U);
	data_chunk(&b, samples, 16U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
	free(b.bytes);
}

/* ---- refused files ----------------------------------------------------- */

static void test_refusals(void)
{
	uint8_t samples[16] = { 1 };
	builder_t b;
	wav_info_t info;

	CHECK(wav_parse(0, 0U, &info) == WAV_ERR_ARGUMENT);
	CHECK(wav_parse(samples, 16U, 0) == WAV_ERR_ARGUMENT);
	CHECK(parse_exact(samples, 0U, &info) == WAV_ERR_TOO_SHORT);
	CHECK(parse_exact(samples, 11U, &info) == WAV_ERR_TOO_SHORT);

	uint8_t junk[64];

	memset(junk, 'x', sizeof(junk));
	CHECK(parse_exact(junk, sizeof(junk), &info) == WAV_ERR_NOT_RIFF);

	/* an MP3, as ID3v2 */
	memcpy(junk, "ID3\x03\x00\x00\x00\x00\x00\x00", 10U);
	CHECK(parse_exact(junk, sizeof(junk), &info) == WAV_ERR_NOT_RIFF);

	/* RIFF but AVI */
	riff_begin(&b);
	memcpy(b.bytes + 8U, "AVI ", 4U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NOT_WAVE);
	free(b.bytes);

	/* a header and nothing else */
	riff_begin(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NO_FORMAT);
	free(b.bytes);

	riff_begin(&b);
	fmt_chunk(&b, 1U, 2U, 44100U, 16U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NO_DATA);
	free(b.bytes);

	riff_begin(&b);
	data_chunk(&b, samples, 16U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_NO_FORMAT);
	free(b.bytes);

	/* a fmt chunk shorter than 16 bytes */
	riff_begin(&b);
	put_tag(&b, "fmt ");
	put32(&b, 14U);
	put(&b, samples, 14U);
	data_chunk(&b, samples, 16U);
	CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
	free(b.bytes);

	/* formats other than PCM and float: ADPCM (2), A-law (6), mu-law (7), MP3 (0x55) */
	static const uint32_t foreign[] = { 2U, 6U, 7U, 0x55U, 0U, 0xFFFFU };

	for (size_t index = 0U; index < sizeof(foreign) / sizeof(foreign[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, foreign[index], 2U, 44100U, 16U);
		data_chunk(&b, samples, 16U);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_UNSUPPORTED);
		free(b.bytes);
	}

	/* widths PCM and float do not have */
	static const struct {
		uint32_t tag;
		uint32_t bits;
	} widths[] = { { 1U, 4U }, { 1U, 12U }, { 1U, 20U }, { 1U, 64U }, { 3U, 16U }, { 3U, 24U }, { 3U, 8U } };

	for (size_t index = 0U; index < sizeof(widths) / sizeof(widths[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, widths[index].tag, 2U, 44100U, widths[index].bits);
		data_chunk(&b, samples, 16U);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_UNSUPPORTED || parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
		free(b.bytes);
	}

	/* channels: none, too many */
	static const uint32_t bad_channels[] = { 0U, 9U, 255U, 65535U };

	for (size_t index = 0U; index < sizeof(bad_channels) / sizeof(bad_channels[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, 1U, bad_channels[index], 44100U, 16U);
		data_chunk(&b, samples, 16U);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
		free(b.bytes);
	}

	/* rates: none, absurd */
	static const uint32_t bad_rates[] = { 0U, WAV_SAMPLE_RATE_MAX + 1U, 0xFFFFFFFFU };

	for (size_t index = 0U; index < sizeof(bad_rates) / sizeof(bad_rates[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, 1U, 2U, bad_rates[index], 16U);
		data_chunk(&b, samples, 16U);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
		free(b.bytes);
	}

	/* a block alignment that disagrees with channels and width, in either direction, and zero */
	static const uint32_t aligns[] = { 0U, 2U, 3U, 8U, 65535U };

	for (size_t index = 0U; index < sizeof(aligns) / sizeof(aligns[0]); index++) {
		riff_begin(&b);
		fmt_chunk(&b, 1U, 2U, 44100U, 16U);
		b.bytes[12U + 8U + 12U] = (uint8_t)aligns[index];
		b.bytes[12U + 8U + 13U] = (uint8_t)(aligns[index] >> 8U);
		data_chunk(&b, samples, 16U);
		CHECK(parse_exact(b.bytes, b.size, &info) == WAV_ERR_BAD_FORMAT);
		free(b.bytes);
	}

	/* on failure the result is zeroed */
	riff_begin(&b);
	CHECK(parse_exact(b.bytes, b.size, &info) != WAV_OK && info.frames == 0U && info.data == 0);
	free(b.bytes);

	CHECK(strcmp(wav_status_name(WAV_OK), "ok") == 0);
	CHECK(strcmp(wav_status_name((wav_status_t)99), "?") == 0);
	for (int status = WAV_OK; status <= WAV_ERR_UNSUPPORTED; status++) CHECK(strcmp(wav_status_name((wav_status_t)status), "?") != 0);
	CHECK(strcmp(wav_encoding_name(WAV_ENCODING_PCM), "PCM") == 0);
	CHECK(strcmp(wav_encoding_name(WAV_ENCODING_FLOAT), "float") == 0);
}

/* ---- conversion -------------------------------------------------------- */

static wav_info_t make_info(uint32_t tag, uint32_t channels, uint32_t bits, const uint8_t *samples, size_t size, builder_t *b)
{
	wav_info_t info;

	riff_begin(b);
	fmt_chunk(b, tag, channels, 44100U, bits);
	data_chunk(b, samples, size);
	riff_end(b);
	CHECK(wav_parse(b->bytes, b->size, &info) == WAV_OK);
	return info;
}

static void float_bits(uint8_t *out, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	out[0] = (uint8_t)bits;
	out[1] = (uint8_t)(bits >> 8U);
	out[2] = (uint8_t)(bits >> 16U);
	out[3] = (uint8_t)(bits >> 24U);
}

static void double_bits(uint8_t *out, double value)
{
	uint64_t bits;

	memcpy(&bits, &value, sizeof(bits));
	for (int index = 0; index < 8; index++) out[index] = (uint8_t)(bits >> (8 * index));
}

static void test_conversion(void)
{
	builder_t b;
	int16_t out[64];

	/* 8-bit unsigned, mono: 128 is zero */
	static const uint8_t u8[] = { 128U, 255U, 0U, 129U };
	wav_info_t info = make_info(1U, 1U, 8U, u8, sizeof(u8), &b);

	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 4U);
	CHECK(out[0] == 0 && out[1] == 0 && out[2] == 32512 && out[3] == 32512 && out[4] == -32768 && out[5] == -32768 && out[6] == 256 && out[7] == 256);
	free(b.bytes);

	/* 16-bit stereo is copied as it is */
	static const uint8_t s16[] = { 0x01, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0x34, 0x12 };

	info = make_info(1U, 2U, 16U, s16, sizeof(s16), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 2U);
	CHECK(out[0] == -32767 && out[1] == 32767 && out[2] == 0 && out[3] == 0x1234);
	free(b.bytes);

	/* 16-bit mono goes to both channels */
	info = make_info(1U, 1U, 16U, s16, sizeof(s16), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 4U);
	CHECK(out[0] == -32767 && out[1] == -32767 && out[2] == 32767 && out[3] == 32767);
	free(b.bytes);

	/* 24-bit: the top 16 bits */
	static const uint8_t s24[] = { 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x12, 0x34, 0x56, 0xFF, 0xFF, 0xFF };

	info = make_info(1U, 2U, 24U, s24, sizeof(s24), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 2U);
	CHECK(out[0] == -32768 && out[1] == 32767 && out[2] == 0x5634 && out[3] == -1);
	free(b.bytes);

	/* 32-bit */
	static const uint8_t s32[] = { 0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x7F };

	info = make_info(1U, 2U, 32U, s32, sizeof(s32), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 1U);
	CHECK(out[0] == -32768 && out[1] == 32767);
	free(b.bytes);

	/* float32: exact values, the rounding of a half, clamping, tiny values, NaN, infinity */
	static const float floats[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -3.5f, 1e-9f, -1e-9f, 0.99999f, 1.0f / 32767.0f, 0.25f };
	uint8_t raw[sizeof(floats) + 32U];

	for (size_t index = 0U; index < sizeof(floats) / sizeof(floats[0]); index++) float_bits(raw + 4U * index, floats[index]);

	static const uint32_t special[] = { 0x7FC00000U, 0xFF800000U, 0x7F800000U, 0x00000001U, 0x80000000U };

	for (size_t index = 0U; index < sizeof(special) / sizeof(special[0]); index++) {
		size_t at = sizeof(floats) + 4U * index;

		raw[at] = (uint8_t)special[index];
		raw[at + 1U] = (uint8_t)(special[index] >> 8U);
		raw[at + 2U] = (uint8_t)(special[index] >> 16U);
		raw[at + 3U] = (uint8_t)(special[index] >> 24U);
	}

	size_t float_count = sizeof(floats) / sizeof(floats[0]) + sizeof(special) / sizeof(special[0]);

	info = make_info(3U, 1U, 32U, raw, float_count * 4U, &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == float_count);

	static const int expected[] = { 0, 32767, -32768, 16384, -16384, 32767, -32768, 0, 0, 32767, 1, 8192, 0, -32768, 32767, 0, 0 };

	for (size_t index = 0U; index < float_count; index++) {
		if (out[2U * index] != expected[index]) printf("float %zu: got %d, expected %d\n", index, out[2U * index], expected[index]);
		CHECK(out[2U * index] == expected[index]);
	}

	free(b.bytes);

	/* float64 */
	static const double doubles[] = { 0.0, 1.0, -1.0, 0.5, -0.25, 7.0, 1e-300, 0.999999 };
	uint8_t raw64[sizeof(doubles)];

	for (size_t index = 0U; index < sizeof(doubles) / sizeof(doubles[0]); index++) double_bits(raw64 + 8U * index, doubles[index]);

	info = make_info(3U, 2U, 64U, raw64, sizeof(raw64), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 4U);
	CHECK(out[0] == 0 && out[1] == 32767 && out[2] == -32768 && out[3] == 16384 && out[4] == -8192 && out[5] == 32767 && out[6] == 0 && out[7] == 32767);
	free(b.bytes);

	/* More than two channels keep the first two; six channels of 16 bits */
	uint8_t six[24];

	for (size_t index = 0U; index < sizeof(six); index++) six[index] = (uint8_t)index;

	info = make_info(1U, 6U, 16U, six, sizeof(six), &b);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 64U) == 2U);
	CHECK(out[0] == 0x0100 && out[1] == 0x0302 && out[2] == 0x0D0C && out[3] == 0x0F0E);
	free(b.bytes);

	/* Starting part way, limiting the count, and running off the end */
	info = make_info(1U, 2U, 16U, s16, sizeof(s16), &b);
	CHECK(wav_convert_s16_stereo(&info, 1U, out, 64U) == 1U && out[0] == 0 && out[1] == 0x1234);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 1U) == 1U);
	CHECK(wav_convert_s16_stereo(&info, 0U, out, 0U) == 0U);
	CHECK(wav_convert_s16_stereo(&info, 2U, out, 64U) == 0U);
	CHECK(wav_convert_s16_stereo(&info, UINT64_MAX, out, 64U) == 0U);
	CHECK(wav_convert_s16_stereo(0, 0U, out, 64U) == 0U);
	CHECK(wav_convert_s16_stereo(&info, 0U, 0, 64U) == 0U);
	free(b.bytes);

	wav_info_t empty = { 0 };

	CHECK(wav_convert_s16_stereo(&empty, 0U, out, 64U) == 0U);
}

/* ---- the sweeps --------------------------------------------------------- */

static uint32_t g_seed = 12345U;

static uint32_t random32(void)
{
	g_seed = g_seed * 1664525U + 1013904223U;
	return g_seed >> 8U;
}

/* Parse; if it is accepted, convert the whole stream in small steps. Never crash, never leave the buffer. */
static void exercise(const uint8_t *bytes, size_t size)
{
	uint8_t *copy = exact(bytes, size);
	wav_info_t info;
	wav_status_t status = wav_parse(copy, size, &info);

	if (status == WAV_OK) {
		int16_t out[16];
		uint64_t frame = 0U;
		size_t got;

		CHECK(info.frames * info.block_align == info.data_bytes);
		CHECK(info.data_bytes == 0U || (info.data >= copy && info.data + info.data_bytes <= copy + size));

		while ((got = wav_convert_s16_stereo(&info, frame, out, 8U)) != 0U) frame += got;

		CHECK(frame == info.frames);
	} else {
		CHECK(info.frames == 0U && info.data == 0);
	}

	free(copy);
}

static void test_every_prefix_and_mutation(void)
{
	static const uint8_t rich[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
	builder_t files[4];

	riff_begin(&files[0]);
	list_chunk(&files[0], 9U);
	fmt_chunk(&files[0], 1U, 2U, 44100U, 16U);
	data_chunk(&files[0], rich, sizeof(rich));
	list_chunk(&files[0], 5U);
	riff_end(&files[0]);

	riff_begin(&files[1]);
	fmt_extensible(&files[1], 3U, 2U, 48000U, 32U, 32U, 3U);
	data_chunk(&files[1], rich, sizeof(rich));
	riff_end(&files[1]);

	riff_begin(&files[2]);
	fmt_chunk(&files[2], 1U, 1U, 22050U, 24U);
	data_chunk(&files[2], rich, sizeof(rich));
	riff_end(&files[2]);

	riff_begin(&files[3]);
	data_chunk(&files[3], rich, sizeof(rich));
	fmt_chunk(&files[3], 3U, 2U, 44100U, 64U);
	riff_end(&files[3]);

	/* Every prefix of every file, including the empty one. */
	for (int file = 0; file < 4; file++) {
		for (size_t length = 0U; length <= files[file].size; length++) exercise(files[file].bytes, length);
	}

	/* Every single byte set to each of a few interesting values, then random flips of several bytes. */
	static const uint8_t values[] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };

	for (int file = 0; file < 4; file++) {
		uint8_t *work = exact(files[file].bytes, files[file].size);

		for (size_t at = 0U; at < files[file].size; at++) {
			uint8_t saved = work[at];

			for (size_t value = 0U; value < sizeof(values); value++) {
				work[at] = values[value];
				exercise(work, files[file].size);
			}

			work[at] = saved;
		}

		for (unsigned int round = 0U; round < 60000U; round++) {
			memcpy(work, files[file].bytes, files[file].size);

			unsigned int flips = 1U + random32() % 6U;

			for (unsigned int flip = 0U; flip < flips; flip++) work[random32() % files[file].size] = (uint8_t)random32();

			size_t length = random32() % 8U == 0U ? random32() % (files[file].size + 1U) : files[file].size;

			exercise(work, length);
		}

		free(work);
	}

	/* Pure noise, and noise behind a valid header. */
	uint8_t noise[128];

	for (unsigned int round = 0U; round < 20000U; round++) {
		for (size_t index = 0U; index < sizeof(noise); index++) noise[index] = (uint8_t)random32();

		if (round % 2U == 0U) memcpy(noise, files[0].bytes, 12U);

		exercise(noise, random32() % (sizeof(noise) + 1U));
	}

	for (int file = 0; file < 4; file++) free(files[file].bytes);
}

int main(void)
{
	test_formats();
	test_extensible();
	test_chunk_walking();
	test_sizes_that_lie();
	test_refusals();
	test_conversion();
	test_every_prefix_and_mutation();

	printf("wav: %u check(s), %u failure(s)\n", g_checks, g_failures);
	return g_failures == 0U ? 0 : 1;
}
