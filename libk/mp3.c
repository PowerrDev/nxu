#include <libk/mp3.h>

#include <stddef.h>
#include <stdint.h>

/* Bitrates in kbit/s by bitrate index, for MPEG-1 layers I, II, III and MPEG-2/2.5 layers I, II+III; 0 = free format, 15 = invalid. */
static const uint16_t g_mp3_bitrates[5][16] = {
	{ 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
	{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
	{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
	{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
	{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }
};

/* Sample rates in Hz by sampling index for MPEG-1, MPEG-2 and MPEG-2.5. */
static const uint16_t g_mp3_rates[3][3] = {
	{ 44100, 48000, 32000 },
	{ 22050, 24000, 16000 },
	{ 11025, 12000, 8000 }
};

/* Read the frame header at bytes[0..4); 1 when it is a valid one. */
static int mp3_read_header(const uint8_t *bytes, mp3_info_t *info)
{
	uint32_t header = ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];

	if ((header >> 21U) != 0x7FFU) return 0;

	uint32_t version = (header >> 19U) & 3U; /* 0: 2.5, 1: reserved, 2: MPEG-2, 3: MPEG-1 */
	uint32_t layer_bits = (header >> 17U) & 3U; /* 1: III, 2: II, 3: I, 0: reserved */
	uint32_t bitrate_index = (header >> 12U) & 15U;
	uint32_t rate_index = (header >> 10U) & 3U;

	if (version == 1U || layer_bits == 0U || bitrate_index == 0U || bitrate_index == 15U || rate_index == 3U) return 0;

	uint32_t layer = 4U - layer_bits;
	uint32_t row;

	if (version == 3U) row = layer - 1U;
	else row = layer == 1U ? 3U : 4U;

	info->mpeg = version == 3U ? 1U : version == 2U ? 2U : 25U;
	info->layer = layer;
	info->bitrate_kbps = g_mp3_bitrates[row][bitrate_index];
	info->sample_rate = g_mp3_rates[version == 3U ? 0U : version == 2U ? 1U : 2U][rate_index];
	info->channels = ((header >> 6U) & 3U) == 3U ? 1U : 2U;
	return 1;
}

int mp3_probe(const uint8_t *bytes, size_t size, mp3_info_t *info)
{
	if (info == 0) return 0;

	*info = (mp3_info_t) { 0 };

	if (bytes == 0 || size < 4U) return 0;

	uint64_t offset = 0U;
	mp3_info_t found = { 0 };

	/* "ID3", major, revision, flags, then the tag size as four 7-bit bytes ("synchsafe"); the footer flag adds 10 more. */
	if (size >= 10U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3' && bytes[3] < 0xFFU && bytes[4] < 0xFFU && bytes[6] < 0x80U && bytes[7] < 0x80U && bytes[8] < 0x80U && bytes[9] < 0x80U) {
		uint64_t tag = ((uint64_t)bytes[6] << 21U) | ((uint64_t)bytes[7] << 14U) | ((uint64_t)bytes[8] << 7U) | (uint64_t)bytes[9];

		tag += 10U + ((bytes[5] & 0x10U) != 0U ? 10U : 0U);
		found.id3v2 = 1;
		found.id3_major = bytes[3];
		found.id3_bytes = tag > 0xFFFFFFFFU ? 0xFFFFFFFFU : (uint32_t)tag;
		offset = tag;
	}

	for (uint64_t scan = 0U; scan <= 4096U; scan++) {
		uint64_t at = offset + scan;

		if (at + 4U > size) break;

		if (mp3_read_header(bytes + at, &found)) {
			found.frame_offset = at;
			*info = found;
			return 1;
		}
	}

	return 0;
}
