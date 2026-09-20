/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        libk/mp3.h
 *
 * Recognising MP3 files, not decoding them: the kernel has no decoder, but the
 * boot chime ships as an MP3 with a pre-decoded WAV beside it and the boot log
 * says what the MP3 is. mp3_probe() skips an ID3v2 tag if there is one, finds
 * the first MPEG audio frame header and reads the stream parameters from it.
 * Freestanding like libk/wav.h (only <stdint.h> and <stddef.h>).
 */

#ifndef NXU_LIBK_MP3_H
#define NXU_LIBK_MP3_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	int id3v2; /* an ID3v2 tag was skipped */
	uint32_t id3_major; /* its version: 2, 3 or 4 */
	uint32_t id3_bytes; /* its size, header included */
	uint32_t mpeg; /* 1, 2 or 25 (MPEG 2.5) */
	uint32_t layer; /* 1, 2 or 3 */
	uint32_t bitrate_kbps;
	uint32_t sample_rate;
	uint32_t channels;
	uint64_t frame_offset; /* where the first frame header is */
} mp3_info_t;

/*
 * Returns 1 and fills *info when bytes[0..size) start with (an ID3v2 tag and
 * then) a valid MPEG audio frame header, else 0 and *info zeroed. Looks for
 * the header at the end of the tag, and within the next 4 KiB, which is where
 * encoders that pad the tag put it.
 */
int mp3_probe(const uint8_t *bytes, size_t size, mp3_info_t *info);

#endif
