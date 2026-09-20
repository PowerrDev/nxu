/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/sound_test.c
 *
 * See kern/tests/sound_test.h.
 */

#include <kern/tests/sound_test.h>

#include <kern/audio/audio_defs.h>
#include <kern/boot/boot_chime.h>
#include <kern/console/console.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SOUND_TEST_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

#define SOUND_TEST_CHUNK_FRAMES 1024U

void sound_test_fill_tone(int16_t *frames, uint32_t first_frame, uint32_t count)
{
	for (uint32_t index = 0U; index < count; index++) {
		uint32_t phase = (first_frame + index) % SOUND_TEST_TONE_PERIOD_FRAMES;
		int32_t step = 2 * SOUND_TEST_TONE_AMPLITUDE / (int32_t)(SOUND_TEST_TONE_PERIOD_FRAMES / 2U);
		int32_t left = phase < SOUND_TEST_TONE_PERIOD_FRAMES / 2U
			? -SOUND_TEST_TONE_AMPLITUDE + (int32_t)phase * step
			: SOUND_TEST_TONE_AMPLITUDE - (int32_t)(phase - SOUND_TEST_TONE_PERIOD_FRAMES / 2U) * step;

		frames[2U * index] = (int16_t)left;
		frames[2U * index + 1U] = (int16_t)-left;
	}
}

static volatile bool g_sound_test_done;
static volatile bool g_sound_test_ok;

/* Play the tone and the silence after it through /dev/audio0; true when nothing went wrong. */
static bool sound_test_play_tone(void)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	vfs_status_t status = vfs_open(filedesc, NXU_AUDIO_DEVICE_PATH, VFS_OPEN_WRITE, &descriptor);

	if (status != VFS_STATUS_OK) {
		SOUND_TEST_LOG("FAILED: %s could not be opened: %s\n", NXU_AUDIO_DEVICE_PATH, vfs_status_name(status));
		return false;
	}

	/* A second open of an exclusive device must be refused while the first is held. */
	uint32_t second;

	if (vfs_open(filedesc, NXU_AUDIO_DEVICE_PATH, VFS_OPEN_WRITE, &second) != VFS_STATUS_BUSY) {
		SOUND_TEST_LOG("FAILED: a second open of %s was not refused as busy\n", NXU_AUDIO_DEVICE_PATH);
		(void)vfs_close(filedesc, descriptor);
		return false;
	}

	SOUND_TEST_LOG("a second open of %s is refused as busy\n", NXU_AUDIO_DEVICE_PATH);

	nxu_audio_params_t params = { .rate = 44100U, .channels = 2U, .format = NXU_AUDIO_FORMAT_S16 };

	status = vfs_ioctl(filedesc, descriptor, NXU_AUDIO_SET_PARAMS, &params);

	if (status != VFS_STATUS_OK || params.rate != 44100U || params.channels != 2U || params.format != NXU_AUDIO_FORMAT_S16) {
		SOUND_TEST_LOG("FAILED: 16-bit stereo at 44100 Hz was not accepted (%s)\n", vfs_status_name(status));
		(void)vfs_close(filedesc, descriptor);
		return false;
	}

	static int16_t chunk[2U * SOUND_TEST_CHUNK_FRAMES];
	uint32_t total = SOUND_TEST_TONE_FRAMES + SOUND_TEST_TAIL_FRAMES;
	uint32_t frame = 0U;
	uint64_t started = timer_get_microseconds();
	bool ok = true;

	while (frame < total && ok) {
		uint32_t count = total - frame < SOUND_TEST_CHUNK_FRAMES ? total - frame : SOUND_TEST_CHUNK_FRAMES;

		memset(chunk, 0, sizeof(chunk));

		if (frame < SOUND_TEST_TONE_FRAMES) {
			uint32_t tone = SOUND_TEST_TONE_FRAMES - frame < count ? SOUND_TEST_TONE_FRAMES - frame : count;

			sound_test_fill_tone(chunk, frame, tone);
		}

		uint64_t written;

		status = vfs_write(filedesc, descriptor, chunk, (uint64_t)count * 4U, &written);

		if (status != VFS_STATUS_OK || written != (uint64_t)count * 4U) {
			SOUND_TEST_LOG("FAILED: write at frame %u: %s\n", frame, vfs_status_name(status));
			ok = false;
		}

		frame += count;
	}

	if (ok && (status = vfs_ioctl(filedesc, descriptor, NXU_AUDIO_DRAIN, 0)) != VFS_STATUS_OK) {
		SOUND_TEST_LOG("FAILED: drain: %s\n", vfs_status_name(status));
		ok = false;
	}

	uint64_t elapsed_ms = (timer_get_microseconds() - started) / 1000ULL;
	nxu_audio_info_t info;

	if (vfs_ioctl(filedesc, descriptor, NXU_AUDIO_GET_INFO, &info) != VFS_STATUS_OK) {
		SOUND_TEST_LOG("FAILED: the device information could not be read\n");
		memset(&info, 0, sizeof(info));
		ok = false;
	}

	(void)vfs_close(filedesc, descriptor);

	uint64_t expected_ms = (uint64_t)total * 1000ULL / 44100ULL;

	SOUND_TEST_LOG("wrote %llu byte(s), %llu period(s) played, %llu underrun(s), %llu I/O error(s)\n", (unsigned long long)info.bytes_written, (unsigned long long)info.periods_played, (unsigned long long)info.xruns, (unsigned long long)info.io_errors);
	SOUND_TEST_LOG("took %llu ms for %llu ms of audio\n", (unsigned long long)elapsed_ms, (unsigned long long)expected_ms);

	if (info.xruns != 0ULL || info.io_errors != 0ULL) ok = false;
	if (info.bytes_written != (uint64_t)total * 4ULL) ok = false;

	/* The device plays in real time: much less than the length of the audio means it was not really played. */
	if (elapsed_ms + expected_ms / 10ULL < expected_ms) {
		SOUND_TEST_LOG("FAILED: played faster than real time\n");
		ok = false;
	}

	return ok;
}

static void sound_test_thread(void *parameter)
{
	(void)parameter;

	bool ok = sound_test_play_tone();

	SOUND_TEST_LOG("the tone %s\n", ok ? "played" : "did not play");

	/* Then the chime, through the code the boot uses (its WAV is read from the system volume). */
	if (ok) {
		bool chime = boot_chime_play();

		SOUND_TEST_LOG("the boot chime %s\n", chime ? "played" : "did not play");
		ok = chime;
	}

	g_sound_test_ok = ok;
	g_sound_test_done = true;
}

bool sound_test_run(void)
{
	g_sound_test_done = false;
	g_sound_test_ok = false;

	/* The driver's completions arrive by interrupt; nothing has enabled them yet at this point of a boot. */
	ml_irq_enable();

	thread_t thread;

	if (!kernel_thread_create(proc_task(proc_kernel()), sound_test_thread, 0, &thread) || !sched_thread_start(thread)) {
		SOUND_TEST_LOG("FAILED: the test thread could not be started\n");
		return false;
	}

	while (!g_sound_test_done) {
		if (!sched_yield()) {
			SOUND_TEST_LOG("FAILED: the scheduler stopped\n");
			return false;
		}
	}

	return g_sound_test_ok;
}
