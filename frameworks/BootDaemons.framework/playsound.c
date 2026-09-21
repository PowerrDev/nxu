#include <kern/audio/audio_defs.h>
#include <libk/wav.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdint.h>

/*
 * playsound [file.wav]
 *
 * Reads a WAV file, checks it with the same reader the kernel uses (libk/wav.c),
 * opens /dev/audio0, negotiates the format, converts the samples to what the
 * device plays (16-bit stereo) as it goes and writes them, then waits for the
 * device to finish and says what happened: the file, its format and length, what
 * the device took, how many bytes went, how many periods played, underruns.
 *
 * With no file it plays the boot chime, Boot_Audio.wav in the system volume:
 * the kernel can start a program but not hand it arguments, and that is how
 * its tests reach this one.
 *
 * It needs NXU_CAP_AUDIO (whoever starts it grants it: nxu_spawn_caps, or the
 * kernel for the tests). Exit status: 0 played, 1 usage, 2 the file could not
 * be read, 3 it is not a WAV this reader accepts, 4 the audio device refused
 * it (10: the capability is missing), 5 the format was not accepted, 6 a write
 * failed.
 */

#define PLAYSOUND_CHUNK_FRAMES 1024U
#define PLAYSOUND_DEFAULT_PATH "/disk/System/Library/Resources/Audio/Boot_Audio.wav"

static void
say(const char *text)
{
	(void)nxu_write(1ULL, text, nxu_strlen(text));
}

static void
say_u64(uint64_t value)
{
	char digits[24];
	uint32_t count = 0U;

	do {
		digits[count++] = (char)('0' + value % 10ULL);
		value /= 10ULL;
	} while (value != 0ULL);

	char text[24];
	uint32_t length = 0U;

	while (count != 0U) text[length++] = digits[--count];
	text[length] = '\0';
	say(text);
}

/* One line of the report: a label, a number, an optional unit, a newline. */
static void
report(const char *label, uint64_t value, const char *unit)
{
	say("playsound: ");
	say(label);
	say_u64(value);
	if (unit != 0) say(unit);
	say("\n");
}

static void
report_text(const char *label, const char *text)
{
	say("playsound: ");
	say(label);
	say(text);
	say("\n");
}

static int
fail(int status, const char *message, const char *detail)
{
	say("playsound: ");
	say(message);
	if (detail != 0) say(detail);
	say("\n");
	return status;
}

static const char *
format_name(uint32_t format)
{
	switch (format) {
	case NXU_AUDIO_FORMAT_U8: return "8-bit unsigned";
	case NXU_AUDIO_FORMAT_S16: return "16-bit signed";
	case NXU_AUDIO_FORMAT_S32: return "32-bit signed";
	case NXU_AUDIO_FORMAT_F32: return "32-bit float";
	default: return "unknown";
	}
}

static int
playsound(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && (argv == 0 || argv[1] == 0))) return fail(1, "usage: playsound [file.wav]", 0);

	const char *path = argc == 2 ? argv[1] : PLAYSOUND_DEFAULT_PATH;

	report_text("file ", path);

	nxu_stat_t stat;

	if (nxu_stat(path, &stat) != 0 || stat.type != NXU_STAT_TYPE_REGULAR) return fail(2, "cannot find ", path);
	if (stat.size == 0ULL) return fail(2, "the file is empty: ", path);

	report("file size ", stat.size, " bytes");

	int64_t file = nxu_open(path, NXU_O_READ);

	if (file < 0) return fail(2, "cannot open ", path);

	int64_t region = nxu_mmap(stat.size, NXU_MMAP_PROT_READ_WRITE);

	if (region < 0) return fail(2, "no memory for the file: ", path);

	uint8_t *bytes = (uint8_t *)(uintptr_t)region;
	uint64_t done = 0ULL;

	while (done < stat.size) {
		int64_t got = nxu_read((uint64_t)file, bytes + done, stat.size - done);

		if (got <= 0) break;

		done += (uint64_t)got;
	}

	(void)nxu_close((uint64_t)file);

	if (done != stat.size) return fail(2, "short read of ", path);

	wav_info_t wav;
	wav_status_t parsed = wav_parse(bytes, (size_t)stat.size, &wav);

	if (parsed != WAV_OK) return fail(3, "not a playable WAV file: ", wav_status_name(parsed));

	report("format ", wav.bits_per_sample, wav.encoding == WAV_ENCODING_FLOAT ? "-bit float" : "-bit PCM");
	report("channels ", wav.channels, 0);
	report("sample rate ", wav.sample_rate, " Hz");
	report("frames ", wav.frames, 0);
	report("duration ", wav.frames * 1000ULL / wav.sample_rate, " ms");

	if (wav.data_truncated) say("playsound: the data chunk is cut short: playing what is there\n");
	if (wav.frames == 0ULL) return fail(0, "nothing to play", 0);

	int64_t device = nxu_open(NXU_AUDIO_DEVICE_PATH, NXU_O_WRITE);

	if (device < 0) {
		if (device == -NXU_SYS_E_DENIED) return fail(10, "the audio device needs the audio capability: ", NXU_AUDIO_DEVICE_PATH);
		if (device == -NXU_SYS_E_BUSY) return fail(4, "the audio device is in use: ", NXU_AUDIO_DEVICE_PATH);
		return fail(4, "cannot open ", NXU_AUDIO_DEVICE_PATH);
	}

	report_text("device ", NXU_AUDIO_DEVICE_PATH);

	nxu_audio_info_t info;

	if (nxu_ioctl((uint64_t)device, NXU_AUDIO_GET_INFO, &info) == 0) {
		report("device channels from ", info.channels_min, 0);
		report("device channels up to ", info.channels_max, 0);
		report("device rates from ", info.rate_min, " Hz");
		report("device rates up to ", info.rate_max, " Hz");
	}

	nxu_audio_params_t params = { .rate = wav.sample_rate, .channels = 2U, .format = NXU_AUDIO_FORMAT_S16 };

	if (nxu_ioctl((uint64_t)device, NXU_AUDIO_SET_PARAMS, &params) != 0) return fail(5, "the device did not accept 16-bit stereo", 0);

	report("device rate ", params.rate, " Hz");
	report("device channels ", params.channels, 0);
	report_text("device format ", format_name(params.format));
	report("device period ", params.period_bytes, " bytes");
	report("device buffer ", params.buffer_bytes, " bytes");

	if (params.format != NXU_AUDIO_FORMAT_S16 || params.channels != 2U) return fail(5, "the device is not running 16-bit stereo", 0);
	if (params.rate != wav.sample_rate) say("playsound: the device rate differs from the file's: the sound is not resampled\n");

	int16_t chunk[2U * PLAYSOUND_CHUNK_FRAMES];
	uint64_t frame = 0ULL;
	uint64_t sent = 0ULL;
	uint64_t started = (uint64_t)nxu_uptime_us();
	size_t frames;

	while ((frames = wav_convert_s16_stereo(&wav, frame, chunk, PLAYSOUND_CHUNK_FRAMES)) != 0U) {
		uint64_t want = (uint64_t)frames * 4ULL;
		uint64_t offset = 0ULL;

		while (offset < want) {
			int64_t written = nxu_write((uint64_t)device, (const uint8_t *)chunk + offset, want - offset);

			if (written <= 0) {
				report("write failed after ", sent, " bytes");
				(void)nxu_close((uint64_t)device);
				return 6;
			}

			offset += (uint64_t)written;
			sent += (uint64_t)written;
		}

		frame += frames;
	}

	int64_t drained = nxu_ioctl((uint64_t)device, NXU_AUDIO_DRAIN, 0);
	uint64_t elapsed_ms = ((uint64_t)nxu_uptime_us() - started) / 1000ULL;

	if (drained != 0) say("playsound: the drain was cut short\n");

	if (nxu_ioctl((uint64_t)device, NXU_AUDIO_GET_INFO, &info) == 0) {
		report("device latency ", info.latency_bytes, " bytes");
		report("device periods played ", info.periods_played, 0);
		report("device interrupts ", info.interrupts, 0);
		report("device I/O errors ", info.io_errors, 0);
		report("underruns ", info.xruns, 0);
	}

	(void)nxu_close((uint64_t)device);

	report("bytes written ", sent, 0);
	report("played in ", elapsed_ms, " ms");
	say(sent == wav.frames * 4ULL ? "playsound: done\n" : "playsound: not everything was written\n");

	return sent == wav.frames * 4ULL && drained == 0 ? 0 : 6;
}

/*
 * The i386 loader starts a process with no arguments on its stack (see
 * frameworks/crt0_i386.S), so main() takes none there and reads nothing.
 */
#if defined(__i386__)
int
main(void)
{
	return playsound(0, 0);
}
#else
int
main(int argc, char **argv)
{
	return playsound(argc, argv);
}
#endif
