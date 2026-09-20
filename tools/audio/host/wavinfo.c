/*
 * wavinfo <file.wav>: what the kernel's own WAV reader (libk/wav.c) makes of a
 * file, and whether the samples are alive. Used by tools/audio/make_boot_audio.sh
 * to check the generated boot chime; exits 1 when the file is not one the boot
 * chime can play (not 16-bit stereo at 44.1 kHz) or holds only silence.
 */

#include <libk/wav.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: wavinfo <file.wav>\n");
		return 2;
	}

	FILE *file = fopen(argv[1], "rb");

	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}

	fseek(file, 0, SEEK_END);

	long length = ftell(file);

	fseek(file, 0, SEEK_SET);

	uint8_t *bytes = malloc(length > 0 ? (size_t)length : 1U);

	if (bytes == NULL || fread(bytes, 1U, (size_t)length, file) != (size_t)length) {
		fprintf(stderr, "wavinfo: cannot read %s\n", argv[1]);
		return 2;
	}

	fclose(file);

	wav_info_t info;
	wav_status_t status = wav_parse(bytes, (size_t)length, &info);

	if (status != WAV_OK) {
		printf("wavinfo: %s: %s\n", argv[1], wav_status_name(status));
		return 1;
	}

	printf("wavinfo: %s\n", argv[1]);
	printf("wavinfo: %u-bit %s, %u channel(s), %u Hz\n", info.bits_per_sample, wav_encoding_name(info.encoding), info.channels, info.sample_rate);
	printf("wavinfo: %llu frame(s), %llu.%03llu s\n", (unsigned long long)info.frames, (unsigned long long)(info.frames / info.sample_rate), (unsigned long long)(info.frames % info.sample_rate * 1000ULL / info.sample_rate));

	int16_t block[2 * 4096];
	uint64_t frame = 0U;
	size_t got;
	int peak = 0;
	double energy = 0.0;
	uint64_t first_loud = UINT64_MAX;

	while ((got = wav_convert_s16_stereo(&info, frame, block, 4096U)) != 0U) {
		for (size_t index = 0U; index < 2U * got; index++) {
			int value = block[index] < 0 ? -block[index] : block[index];

			if (value > peak) peak = value;
			if (value > 500 && first_loud == UINT64_MAX) first_loud = frame + index / 2U;

			energy += (double)block[index] * block[index];
		}

		frame += got;
	}

	printf("wavinfo: peak %d of 32768\n", peak);
	printf("wavinfo: rms %.0f\n", info.frames != 0U ? __builtin_sqrt(energy / (2.0 * (double)info.frames)) : 0.0);

	if (first_loud != UINT64_MAX) printf("wavinfo: first sample above 500 at frame %llu (%.0f ms)\n", (unsigned long long)first_loud, 1000.0 * (double)first_loud / info.sample_rate);

	if (info.bits_per_sample != 16U || info.encoding != WAV_ENCODING_PCM || info.channels != 2U || info.sample_rate != 44100U) {
		printf("wavinfo: not 16-bit stereo PCM at 44100 Hz\n");
		return 1;
	}

	if (peak < 100) {
		printf("wavinfo: the file is silent\n");
		return 1;
	}

	printf("wavinfo: ok\n");
	return 0;
}
