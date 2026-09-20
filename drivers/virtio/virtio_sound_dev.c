#include <drivers/virtio/virtio_sound.h>

#include <drivers/virtio/virtio_sound_internal.h>
#include <kern/audio/audio_defs.h>
#include <kern/process/proc.h>
#include <vfs/devfs.h>
#include <vfs/file.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * /dev/audio0: the character device in front of the playback stream. It maps
 * what a program says (the ABI in kern/audio/audio_defs.h) onto the driver's
 * playback interface and the driver's errors onto file system statuses; the
 * stream itself, its ring and its blocking are all in virtio_sound_pcm.c.
 */

static vfs_status_t virtio_snd_dev_status(virtio_snd_error_t error)
{
	switch (error) {
	case VIRTIO_SND_E_NONE: return VFS_STATUS_OK;
	case VIRTIO_SND_E_BUSY: return VFS_STATUS_BUSY;
	case VIRTIO_SND_E_AGAIN: return VFS_STATUS_WOULD_BLOCK;
	case VIRTIO_SND_E_INTERRUPTED: return VFS_STATUS_INTERRUPTED;
	case VIRTIO_SND_E_INVALID:
	case VIRTIO_SND_E_ILLEGAL: return VFS_STATUS_INVALID;
	case VIRTIO_SND_E_NO_MEMORY: return VFS_STATUS_NO_MEMORY;
	default: return VFS_STATUS_IO_ERROR;
	}
}

/* The sample formats of the ABI and the codes the device uses for them. */
static const struct {
	uint32_t abi;
	uint32_t device;
} g_virtio_snd_formats[] = {
	{ NXU_AUDIO_FORMAT_U8, VIRTIO_SND_PCM_FMT_U8 },
	{ NXU_AUDIO_FORMAT_S16, VIRTIO_SND_PCM_FMT_S16 },
	{ NXU_AUDIO_FORMAT_S32, VIRTIO_SND_PCM_FMT_S32 },
	{ NXU_AUDIO_FORMAT_F32, VIRTIO_SND_PCM_FMT_FLOAT }
};

#define VIRTIO_SND_FORMAT_COUNT (sizeof(g_virtio_snd_formats) / sizeof(g_virtio_snd_formats[0]))

static bool virtio_snd_dev_format_to_device(uint32_t abi, uint32_t *device)
{
	for (uint32_t index = 0U; index < VIRTIO_SND_FORMAT_COUNT; index++) {
		if (g_virtio_snd_formats[index].abi != abi) continue;

		*device = g_virtio_snd_formats[index].device;
		return true;
	}

	return false;
}

/* A format the ABI has no name for (the device fell back to something exotic) is reported as 0. */
static uint32_t virtio_snd_dev_format_to_abi(uint32_t device)
{
	for (uint32_t index = 0U; index < VIRTIO_SND_FORMAT_COUNT; index++) {
		if (g_virtio_snd_formats[index].device == device) return g_virtio_snd_formats[index].abi;
	}

	return 0U;
}

static void virtio_snd_dev_params_to_abi(const virtio_snd_params_t *params, nxu_audio_params_t *abi)
{
	abi->rate = params->rate_hz;
	abi->channels = params->channels;
	abi->format = virtio_snd_dev_format_to_abi(params->format);
	abi->period_bytes = params->period_bytes;
	abi->buffer_bytes = params->buffer_bytes;
}

static vfs_status_t virtio_snd_dev_open(void *context, uint32_t flags)
{
	virtio_snd_device_t *device = context;

	/* The kernel itself (the boot chime, the tests) is always allowed; anyone else needs the capability. */
	proc_t caller = current_proc();

	if (caller != proc_kernel() && !proc_has_caps(caller, NXU_CAP_AUDIO)) {
		VIRTIO_SND_LOG("refused: the caller does not hold NXU_CAP_AUDIO\n");
		return VFS_STATUS_DENIED;
	}

	/* A playback device: it has to be opened for writing. */
	if ((flags & VFS_OPEN_WRITE) == 0U) return VFS_STATUS_INVALID;

	return virtio_snd_dev_status(virtio_snd_playback_open(device, (flags & VFS_OPEN_NONBLOCK) != 0U));
}

static void virtio_snd_dev_close(void *context)
{
	virtio_snd_playback_close(context);
}

static vfs_status_t virtio_snd_dev_write(void *context, const void *buffer, uint64_t size, uint64_t *written_size)
{
	return virtio_snd_dev_status(virtio_snd_playback_write(context, buffer, size, written_size));
}

static void virtio_snd_dev_info(virtio_snd_device_t *device, nxu_audio_info_t *abi)
{
	virtio_snd_playback_info_t info;

	virtio_snd_playback_info(device, &info);
	memset(abi, 0, sizeof(*abi));

	abi->bytes_written = info.bytes_written;
	abi->periods_played = info.periods_played;
	abi->xruns = info.xruns;
	abi->io_errors = info.io_errors;
	abi->interrupts = info.irq_count;
	virtio_snd_dev_params_to_abi(&info.params, &abi->params);

	switch (info.state) {
	case VIRTIO_SND_PCM_STATE_PREPARED: abi->state = NXU_AUDIO_STATE_PREPARED; break;
	case VIRTIO_SND_PCM_STATE_STARTED: abi->state = NXU_AUDIO_STATE_RUNNING; break;
	case VIRTIO_SND_PCM_STATE_STOPPED: abi->state = NXU_AUDIO_STATE_STOPPED; break;
	default: abi->state = NXU_AUDIO_STATE_IDLE; break;
	}

	abi->channels_min = info.channels_min;
	abi->channels_max = info.channels_max;

	for (uint32_t rate = 0U; rate < VIRTIO_SND_PCM_RATE_COUNT; rate++) {
		if ((info.rates & (1ULL << rate)) == 0ULL) continue;

		uint32_t hz = virtio_snd_rate_hz(rate);

		if (abi->rate_min == 0U || hz < abi->rate_min) abi->rate_min = hz;
		if (hz > abi->rate_max) abi->rate_max = hz;
	}

	for (uint32_t index = 0U; index < VIRTIO_SND_FORMAT_COUNT; index++) {
		if ((info.formats & (1ULL << g_virtio_snd_formats[index].device)) != 0ULL) abi->formats |= 1U << g_virtio_snd_formats[index].abi;
	}

	abi->latency_bytes = info.latency_bytes;
	abi->queued_periods = info.queued_periods;
}

static vfs_status_t virtio_snd_dev_ioctl(void *context, uint32_t command, void *argument)
{
	virtio_snd_device_t *device = context;

	switch (command) {
	case NXU_AUDIO_GET_INFO:
		virtio_snd_dev_info(device, argument);
		return VFS_STATUS_OK;

	case NXU_AUDIO_SET_PARAMS: {
		nxu_audio_params_t *abi = argument;
		virtio_snd_params_t wanted;
		virtio_snd_params_t actual;

		wanted.rate_hz = abi->rate;
		wanted.channels = abi->channels;
		wanted.period_bytes = 0U;
		wanted.buffer_bytes = 0U;

		if (!virtio_snd_dev_format_to_device(abi->format, &wanted.format)) return VFS_STATUS_INVALID;

		vfs_status_t status = virtio_snd_dev_status(virtio_snd_playback_set_params(device, &wanted, &actual));

		if (status == VFS_STATUS_OK) virtio_snd_dev_params_to_abi(&actual, abi);
		return status;
	}

	case NXU_AUDIO_DRAIN:
		return virtio_snd_dev_status(virtio_snd_playback_drain(device));

	case NXU_AUDIO_STOP:
		return virtio_snd_dev_status(virtio_snd_playback_stop(device));

	case NXU_AUDIO_SET_NONBLOCK:
		virtio_snd_playback_set_nonblock(device, *(const uint32_t *)argument != 0U);
		return VFS_STATUS_OK;

	default:
		return VFS_STATUS_NOT_SUPPORTED;
	}
}

static const devfs_operations_t g_virtio_snd_dev_operations = {
	.open = virtio_snd_dev_open,
	.close = virtio_snd_dev_close,
	.write = virtio_snd_dev_write,
	.ioctl = virtio_snd_dev_ioctl
};

bool virtio_snd_dev_register(virtio_snd_device_t *device)
{
	if (!devfs_register_device("audio0", &g_virtio_snd_dev_operations, device)) {
		VIRTIO_SND_LOG("/dev/audio0 could not be registered\n");
		return false;
	}

	VIRTIO_SND_LOG("registered /dev/audio0 (playback stream %u)\n", device->playback_stream);
	return true;
}
