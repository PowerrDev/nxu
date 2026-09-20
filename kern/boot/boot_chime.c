#include <kern/boot/boot_chime.h>

#include <drivers/virtio/virtio_sound.h>
#include <kern/audio/audio_defs.h>
#include <kern/boot/boot_args.h>
#include <kern/console/console.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <libk/mp3.h>
#include <libk/wav.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOOT_CHIME_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

/* How long the thread waits for the sound device and the file before it says why it gave up. */
#define BOOT_CHIME_WAIT_US 10000000ULL

/* Frames converted and written at a time. */
#define BOOT_CHIME_CHUNK_FRAMES 2048U

static bool g_boot_chime_armed;
static bool g_boot_chime_started;
static uint64_t g_boot_chime_display_us;

static uint64_t boot_chime_ms(uint64_t microseconds)
{
	return microseconds / 1000ULL;
}

void boot_chime_arm(void)
{
	if (g_boot_chime_armed) return;

	if (boot_arg_present("-no-chime")) {
		BOOT_CHIME_LOG("boot chime disabled by -no-chime\n");
		return;
	}

	g_boot_chime_armed = true;
	g_boot_chime_display_us = timer_get_microseconds();

	BOOT_CHIME_LOG("boot chime armed: display output has started\n");
	BOOT_CHIME_LOG("display output started at %llu ms\n", (unsigned long long)boot_chime_ms(g_boot_chime_display_us));
	BOOT_CHIME_LOG("playback waits for the sound device and %s\n", BOOT_CHIME_WAV_PATH);
}

/* Whether path names something in the VFS. */
static bool boot_chime_exists(const char *path)
{
	vnode_t vnode;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	vnode_rele(vnode);
	return true;
}

/*
 * boot_chime_read:
 *
 * The whole of a file into a fresh allocation the caller frees; 0 when it is
 * missing, empty, does not fit or cannot be read.
 */
static uint8_t *boot_chime_read(const char *path, size_t *size)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	vnode_t vnode;

	*size = 0U;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return 0;

	uint64_t length = vnode->v_size;

	vnode_rele(vnode);

	if (length == 0ULL || length > 16ULL * 1024ULL * 1024ULL) return 0;

	uint8_t *bytes = kmalloc((size_t)length);

	if (bytes == 0) {
		BOOT_CHIME_LOG("no memory for %s (%llu bytes)\n", path, (unsigned long long)length);
		return 0;
	}

	uint32_t descriptor;

	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) {
		(void)kfree(bytes);
		return 0;
	}

	uint64_t done = 0ULL;

	while (done < length) {
		uint64_t got = 0ULL;
		uint64_t want = length - done < 8192ULL ? length - done : 8192ULL;

		if (vfs_read(filedesc, descriptor, bytes + done, want, &got) != VFS_STATUS_OK || got == 0ULL) break;

		done += got;
	}

	(void)vfs_close(filedesc, descriptor);

	if (done != length) {
		BOOT_CHIME_LOG("%s: read %llu of %llu bytes\n", path, (unsigned long long)done, (unsigned long long)length);
		(void)kfree(bytes);
		return 0;
	}

	*size = (size_t)length;
	return bytes;
}

/*
 * boot_chime_wait_ready:
 *
 * Wait until the sound device has a playback stream and the file can be
 * found, for at most BOOT_CHIME_WAIT_US. Both are normally there already
 * (the drivers attach before the volume mounts); the wait is for a boot where
 * one of them shows up late, and for the log to say which was missing when
 * it gives up.
 */
static bool boot_chime_wait_ready(void)
{
	uint64_t deadline = timer_get_microseconds() + BOOT_CHIME_WAIT_US;

	for (;;) {
		bool device = virtio_snd_device_at(0U) != 0;
		bool file = boot_chime_exists(BOOT_CHIME_WAV_PATH);

		if (device && file) return true;

		/* The bus scan is over by now: with no sound device attached, waiting will not make one appear. */
		if (!device && virtio_snd_device_count() == 0U) {
			BOOT_CHIME_LOG("no VirtIO Sound device: the boot chime is skipped\n");
			return false;
		}

		if (timer_get_microseconds() > deadline) {
			BOOT_CHIME_LOG("gave up after %llu s: %s\n", (unsigned long long)(BOOT_CHIME_WAIT_US / 1000000ULL), !device ? "the sound device has no playback stream" : "Boot_Audio.wav is not in the system volume");
			return false;
		}

		if (!sched_yield()) return false;
	}
}

/* Say what the MP3 is, from its own headers. */
static void boot_chime_describe_mp3(void)
{
	size_t size;
	uint8_t *bytes = boot_chime_read(BOOT_CHIME_MP3_PATH, &size);

	if (bytes == 0) {
		BOOT_CHIME_LOG("%s is not in the system volume\n", BOOT_CHIME_MP3_PATH);
		return;
	}

	mp3_info_t info;

	BOOT_CHIME_LOG("%s: %llu bytes\n", BOOT_CHIME_MP3_PATH, (unsigned long long)size);

	if (!mp3_probe(bytes, size, &info)) {
		BOOT_CHIME_LOG("Boot_Audio.mp3 has no ID3v2 tag or MPEG audio frame header: not an MP3\n");
		(void)kfree(bytes);
		return;
	}

	if (info.id3v2) BOOT_CHIME_LOG("Boot_Audio.mp3: ID3v2.%u tag, %u bytes\n", info.id3_major, info.id3_bytes);

	BOOT_CHIME_LOG("Boot_Audio.mp3: MPEG audio frame header at byte %llu\n", (unsigned long long)info.frame_offset);
	BOOT_CHIME_LOG("Boot_Audio.mp3: MPEG-%s Layer %s\n", info.mpeg == 1U ? "1" : info.mpeg == 2U ? "2" : "2.5", info.layer == 3U ? "III" : info.layer == 2U ? "II" : "I");
	BOOT_CHIME_LOG("Boot_Audio.mp3: %u kbps, %u Hz, %s\n", info.bitrate_kbps, info.sample_rate, info.channels == 1U ? "mono" : "stereo");
	(void)kfree(bytes);
}

bool boot_chime_play(void)
{
	if (!boot_chime_wait_ready()) return false;

	boot_chime_describe_mp3();
	BOOT_CHIME_LOG("the kernel has no MP3 decoder: playing the pre-decoded Boot_Audio.wav\n");

	size_t size;
	uint8_t *bytes = boot_chime_read(BOOT_CHIME_WAV_PATH, &size);

	if (bytes == 0) {
		BOOT_CHIME_LOG("%s could not be read\n", BOOT_CHIME_WAV_PATH);
		return false;
	}

	wav_info_t wav;
	wav_status_t parsed = wav_parse(bytes, size, &wav);

	if (parsed != WAV_OK) {
		BOOT_CHIME_LOG("%s is not playable: %s\n", BOOT_CHIME_WAV_PATH, wav_status_name(parsed));
		(void)kfree(bytes);
		return false;
	}

	uint64_t duration_ms = wav.frames * 1000ULL / wav.sample_rate;

	BOOT_CHIME_LOG("Boot_Audio.wav: %u-bit %s, %u channel(s), %u Hz\n", wav.bits_per_sample, wav_encoding_name(wav.encoding), wav.channels, wav.sample_rate);
	BOOT_CHIME_LOG("Boot_Audio.wav: %llu frames, %llu ms\n", (unsigned long long)wav.frames, (unsigned long long)duration_ms);

	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	vfs_status_t status = vfs_open(filedesc, NXU_AUDIO_DEVICE_PATH, VFS_OPEN_WRITE, &descriptor);

	if (status != VFS_STATUS_OK) {
		BOOT_CHIME_LOG("%s could not be opened: %s\n", NXU_AUDIO_DEVICE_PATH, vfs_status_name(status));
		(void)kfree(bytes);
		return false;
	}

	nxu_audio_params_t params = { .rate = wav.sample_rate, .channels = 2U, .format = NXU_AUDIO_FORMAT_S16 };

	status = vfs_ioctl(filedesc, descriptor, NXU_AUDIO_SET_PARAMS, &params);

	if (status != VFS_STATUS_OK || params.format != NXU_AUDIO_FORMAT_S16 || params.channels != 2U) {
		BOOT_CHIME_LOG("the device does not take 16-bit stereo: %s\n", vfs_status_name(status));
		(void)vfs_close(filedesc, descriptor);
		(void)kfree(bytes);
		return false;
	}

	if (params.rate != wav.sample_rate) BOOT_CHIME_LOG("the device runs at %u Hz, the chime is %u Hz: it is not resampled\n", params.rate, wav.sample_rate);

	static int16_t chunk[2U * BOOT_CHIME_CHUNK_FRAMES];
	uint64_t frame = 0ULL;
	uint64_t started_us = 0ULL;
	bool ok = true;
	size_t got;

	while ((got = wav_convert_s16_stereo(&wav, frame, chunk, BOOT_CHIME_CHUNK_FRAMES)) != 0U) {
		uint64_t written = 0ULL;

		if (started_us == 0ULL) {
			started_us = timer_get_microseconds();
			BOOT_CHIME_LOG("playback started at %llu ms\n", (unsigned long long)boot_chime_ms(started_us));

			if (g_boot_chime_armed && started_us >= g_boot_chime_display_us) {
				BOOT_CHIME_LOG("%llu ms after display output started\n", (unsigned long long)boot_chime_ms(started_us - g_boot_chime_display_us));
			}
		}

		status = vfs_write(filedesc, descriptor, chunk, (uint64_t)got * 4ULL, &written);

		if (status != VFS_STATUS_OK || written != (uint64_t)got * 4ULL) {
			BOOT_CHIME_LOG("write at frame %llu failed: %s\n", (unsigned long long)frame, vfs_status_name(status));
			ok = false;
			break;
		}

		frame += got;
	}

	if (ok && (status = vfs_ioctl(filedesc, descriptor, NXU_AUDIO_DRAIN, 0)) != VFS_STATUS_OK) {
		BOOT_CHIME_LOG("drain failed: %s\n", vfs_status_name(status));
		ok = false;
	}

	uint64_t ended_us = timer_get_microseconds();
	nxu_audio_info_t info;

	if (vfs_ioctl(filedesc, descriptor, NXU_AUDIO_GET_INFO, &info) != VFS_STATUS_OK) memset(&info, 0, sizeof(info));

	(void)vfs_close(filedesc, descriptor);
	(void)kfree(bytes);

	BOOT_CHIME_LOG("playback ended at %llu ms\n", (unsigned long long)boot_chime_ms(ended_us));
	BOOT_CHIME_LOG("playback lasted %llu ms for %llu ms of audio\n", (unsigned long long)boot_chime_ms(ended_us - started_us), (unsigned long long)duration_ms);
	BOOT_CHIME_LOG("%llu frame(s) written, %llu underrun(s), %llu I/O error(s)\n", (unsigned long long)frame, (unsigned long long)info.xruns, (unsigned long long)info.io_errors);

	return ok && frame == wav.frames && info.xruns == 0ULL && info.io_errors == 0ULL;
}

static void boot_chime_thread(void *parameter)
{
	(void)parameter;

	bool ok = boot_chime_play();

	BOOT_CHIME_LOG("boot chime %s\n", ok ? "finished" : "did not play");
}

void boot_chime_start(void)
{
	if (g_boot_chime_started) return;

	if (!g_boot_chime_armed) {
		BOOT_CHIME_LOG("boot chime not started: it was not armed (no display output, or -no-chime)\n");
		return;
	}

	g_boot_chime_started = true;

	thread_t thread;

	if (!kernel_thread_create(proc_task(proc_kernel()), boot_chime_thread, 0, &thread) || !sched_thread_start(thread)) {
		BOOT_CHIME_LOG("the boot chime thread could not be started\n");
		return;
	}

	BOOT_CHIME_LOG("system volume mounted: boot chime thread started, %llu ms after display output started\n", (unsigned long long)boot_chime_ms(timer_get_microseconds() - g_boot_chime_display_us));
}
