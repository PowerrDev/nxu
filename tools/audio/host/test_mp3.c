/*
 * Host test for the MP3 recogniser (libk/mp3.c): the real boot chime, and
 * hand-made headers, tags and rubbish. Run with the path of Boot_Audio.mp3.
 * Built with ASan and UBSan by tools/audio/test_host.sh.
 */

#include <libk/mp3.h>

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

static uint8_t *exact(const uint8_t *bytes, size_t size)
{
	uint8_t *copy = malloc(size == 0U ? 1U : size);

	if (size != 0U) memcpy(copy, bytes, size);
	return copy;
}

static int probe(const uint8_t *bytes, size_t size, mp3_info_t *info)
{
	uint8_t *copy = exact(bytes, size);
	int result = mp3_probe(copy, size, info);

	free(copy);
	return result;
}

static void test_chime(const char *path)
{
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		printf("FAIL  cannot open %s\n", path);
		g_failures++;
		return;
	}

	fseek(file, 0, SEEK_END);

	long length = ftell(file);

	fseek(file, 0, SEEK_SET);

	uint8_t *bytes = malloc((size_t)length);

	if (fread(bytes, 1U, (size_t)length, file) != (size_t)length) g_failures++;
	fclose(file);

	mp3_info_t info;

	CHECK(probe(bytes, (size_t)length, &info));
	CHECK(info.id3v2 && info.id3_major == 3U);
	CHECK(info.mpeg == 1U && info.layer == 3U);
	CHECK(info.bitrate_kbps == 128U && info.sample_rate == 44100U && info.channels == 2U);
	CHECK(info.frame_offset >= info.id3_bytes);
	printf("mp3: %s: ID3v2.%u tag of %u bytes, MPEG-%u layer %u, %u kbps, %u Hz, %u channel(s), first frame at %llu\n", path, info.id3_major, info.id3_bytes, info.mpeg, info.layer, info.bitrate_kbps, info.sample_rate, info.channels, (unsigned long long)info.frame_offset);

	/* Every prefix: never past the end, and it stops recognising it once the frame header is cut. */
	for (size_t prefix = 0U; prefix <= 6000U && prefix <= (size_t)length; prefix++) {
		mp3_info_t part;
		int found = probe(bytes, prefix, &part);

		if (prefix < info.frame_offset + 4U) CHECK(!found);
		else CHECK(found && part.bitrate_kbps == 128U);
	}

	free(bytes);
}

static void test_headers(void)
{
	mp3_info_t info;

	/* MPEG-1 layer III, 128 kbps, 44.1 kHz, joint stereo: FF FB 90 64 */
	static const uint8_t plain[] = { 0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0 };

	CHECK(probe(plain, sizeof(plain), &info) && !info.id3v2 && info.mpeg == 1U && info.layer == 3U && info.bitrate_kbps == 128U && info.sample_rate == 44100U && info.channels == 2U && info.frame_offset == 0U);

	/* mono (channel mode 3), 48 kHz, 320 kbps */
	static const uint8_t mono[] = { 0xFF, 0xFB, 0xE4, 0xC0 };

	CHECK(probe(mono, sizeof(mono), &info) && info.channels == 1U && info.sample_rate == 48000U && info.bitrate_kbps == 320U);

	/* MPEG-2 layer III, 24 kHz, 64 kbps; MPEG-2.5, 8 kHz, 16 kbps; MPEG-1 layer II 192 kbps 32 kHz; layer I */
	static const uint8_t mpeg2[] = { 0xFF, 0xF3, 0x84, 0x00 };
	static const uint8_t mpeg25[] = { 0xFF, 0xE3, 0x28, 0x00 };
	static const uint8_t layer2[] = { 0xFF, 0xFD, 0xA8, 0x00 };
	static const uint8_t layer1[] = { 0xFF, 0xFF, 0xA0, 0x00 };

	CHECK(probe(mpeg2, sizeof(mpeg2), &info) && info.mpeg == 2U && info.layer == 3U && info.sample_rate == 24000U && info.bitrate_kbps == 64U);
	CHECK(probe(mpeg25, sizeof(mpeg25), &info) && info.mpeg == 25U && info.sample_rate == 8000U);
	CHECK(probe(layer2, sizeof(layer2), &info) && info.layer == 2U);
	CHECK(probe(layer1, sizeof(layer1), &info) && info.layer == 1U);

	/* reserved version, reserved layer, free-format and invalid bitrate, reserved sample rate */
	static const uint8_t bad[][4] = { { 0xFF, 0xEB, 0x90, 0x64 }, { 0xFF, 0xF9, 0x90, 0x64 }, { 0xFF, 0xFB, 0x00, 0x64 }, { 0xFF, 0xFB, 0xF0, 0x64 }, { 0xFF, 0xFB, 0x9C, 0x64 }, { 0xFE, 0xFB, 0x90, 0x64 } };

	for (size_t index = 0U; index < sizeof(bad) / sizeof(bad[0]); index++) {
		CHECK(!probe(bad[index], 4U, &info) && info.mpeg == 0U);
	}

	/* an ID3v2.4 tag of 10 bytes of body plus a footer, then the frame */
	uint8_t tagged[64];

	memset(tagged, 0, sizeof(tagged));
	memcpy(tagged, "ID3\x04\x00\x10\x00\x00\x00\x0A", 10U);
	memcpy(tagged + 30U, plain, 4U);
	CHECK(probe(tagged, 34U, &info) && info.id3v2 && info.id3_major == 4U && info.id3_bytes == 30U && info.frame_offset == 30U);

	/* a tag whose size runs past the end, a bad synchsafe size, a lone "ID3" */
	memset(tagged, 0, sizeof(tagged));
	memcpy(tagged, "ID3\x03\x00\x00\x7F\x7F\x7F\x7F", 10U);
	CHECK(!probe(tagged, sizeof(tagged), &info));
	memcpy(tagged, "ID3\x03\x00\x00\x80\x00\x00\x00", 10U);
	CHECK(!probe(tagged, sizeof(tagged), &info));
	CHECK(!probe((const uint8_t *)"ID3", 3U, &info));

	/* a frame header a few bytes after the tag (padding) is found, one 5 KiB later is not */
	uint8_t *padded = calloc(1U, 6000U);

	memcpy(padded, "ID3\x03\x00\x00\x00\x00\x00\x00", 10U);
	memcpy(padded + 10U + 100U, plain, 4U);
	CHECK(mp3_probe(padded, 6000U, &info) && info.frame_offset == 110U);
	memset(padded, 0, 6000U);
	memcpy(padded, "ID3\x03\x00\x00\x00\x00\x00\x00", 10U);
	memcpy(padded + 10U + 5000U, plain, 4U);
	CHECK(!mp3_probe(padded, 6000U, &info));
	free(padded);

	/* NULL and tiny inputs */
	CHECK(!mp3_probe(0, 0U, &info));
	CHECK(!mp3_probe(plain, 3U, &info));
	CHECK(!mp3_probe(plain, sizeof(plain), 0));

	/* noise never crashes */
	uint32_t seed = 7U;

	for (unsigned int round = 0U; round < 20000U; round++) {
		uint8_t noise[300];

		for (size_t index = 0U; index < sizeof(noise); index++) {
			seed = seed * 1664525U + 1013904223U;
			noise[index] = (uint8_t)(seed >> 16U);
		}

		if (round % 3U == 0U) memcpy(noise, "ID3\x03\x00\x00\x00\x00\x00\x40", 10U);

		(void)probe(noise, (size_t)(seed >> 8U) % (sizeof(noise) + 1U), &info);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: test_mp3 <Boot_Audio.mp3>\n");
		return 2;
	}

	test_chime(argv[1]);
	test_headers();

	printf("mp3: %u check(s), %u failure(s)\n", g_checks, g_failures);
	return g_failures == 0U ? 0 : 1;
}
