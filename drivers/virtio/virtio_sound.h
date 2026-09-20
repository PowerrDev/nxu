#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_H

#include <drivers/virtio/virtio_sound_core.h>
#include <drivers/virtio/virtio_transport.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_SND_MAX_DEVICES 1U

struct virtio_snd_device;
typedef struct virtio_snd_device virtio_snd_device_t;

/*
 * virtio_snd_attach:
 *
 * Attach the VirtIO Sound driver to one VirtIO device on any transport
 * (VirtIO-MMIO on arm64, VirtIO-PCI on x86).
 *
 * Returns true when the device is bound and registered. A device the driver
 * cannot use (no VERSION_1, a queue that will not start) is reported and
 * skipped: sound is never a reason to stop the bus scan or the boot.
 */
bool virtio_snd_attach(const virtio_device_t *transport);

/*
 * virtio_snd_probe:
 *
 * Report the outcome of the bus scan: how many sound devices were bound, or
 * plainly that there is none. Called once the scan is over.
 */
void virtio_snd_probe(void);

uint32_t virtio_snd_device_count(void);
void virtio_snd_dump(void);

/*
 * Playback.
 *
 * What one open of the audio device does with the output stream, one
 * exclusive user at a time:
 *
 *   open       claim the stream (VIRTIO_SND_E_BUSY when it is claimed)
 *   set_params negotiate the format with what the stream reports; the format
 *              really used is handed back, it may differ from the request
 *   write      copy samples into the ring of periods and hand full periods to
 *              the device; the first write configures and prepares the
 *              stream, and it is started once enough is queued. It waits for
 *              room (asleep on a wait queue when interrupts are delivered,
 *              polling otherwise) unless the stream is non-blocking. A short
 *              count is a success; AGAIN or INTERRUPTED only when nothing
 *              was taken
 *   drain      hand over the partial period, start if need be, wait until
 *              the device has consumed everything, let the host play out its
 *              buffer, stop
 *   stop       abandon what is queued (stop and release the stream)
 *   close      stop, free the ring, log the totals
 */
typedef struct {
	uint32_t rate_hz;
	uint32_t channels;
	uint32_t format; /* VIRTIO_SND_PCM_FMT_* */
	uint32_t period_bytes; /* set by the driver */
	uint32_t buffer_bytes; /* set by the driver */
} virtio_snd_params_t;

typedef struct {
	virtio_snd_pcm_state_t state;
	virtio_snd_params_t params;

	/* What the stream can do. */
	uint32_t channels_min;
	uint32_t channels_max;
	uint64_t formats; /* bit map of VIRTIO_SND_PCM_FMT_* */
	uint64_t rates; /* bit map of VIRTIO_SND_PCM_RATE_* */

	uint32_t latency_bytes;
	uint32_t queued_periods;
	uint64_t bytes_written;
	uint64_t periods_played;
	uint64_t xruns;
	uint64_t io_errors;
	uint64_t irq_count;
	uint64_t start_us;
	uint64_t end_us;
} virtio_snd_playback_info_t;

/* The sound device that has a playback stream, or 0. */
virtio_snd_device_t *virtio_snd_device_at(uint32_t index);

virtio_snd_error_t virtio_snd_playback_open(virtio_snd_device_t *device, bool nonblock);
void virtio_snd_playback_close(virtio_snd_device_t *device);
void virtio_snd_playback_set_nonblock(virtio_snd_device_t *device, bool nonblock);
virtio_snd_error_t virtio_snd_playback_set_params(virtio_snd_device_t *device, const virtio_snd_params_t *wanted, virtio_snd_params_t *actual);
virtio_snd_error_t virtio_snd_playback_write(virtio_snd_device_t *device, const void *data, uint64_t size, uint64_t *written);
virtio_snd_error_t virtio_snd_playback_drain(virtio_snd_device_t *device);
virtio_snd_error_t virtio_snd_playback_stop(virtio_snd_device_t *device);
void virtio_snd_playback_info(virtio_snd_device_t *device, virtio_snd_playback_info_t *info);

#endif
