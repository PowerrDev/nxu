#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_INTERNAL_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_INTERNAL_H

#include <drivers/virtio/virtio_snd.h>
#include <drivers/virtio/virtio_sound.h>
#include <drivers/virtio/virtio_sound_core.h>
#include <drivers/virtio/virtio_transport.h>
#include <drivers/virtio/virtqueue.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * What the files of the VirtIO Sound driver share: virtio_sound.c attaches
 * the device and runs the control queue, virtio_sound_pcm.c streams audio
 * to it. Nothing outside the driver includes this.
 */

/* Every line the driver logs starts with the name of the function that prints it. */
#define VIRTIO_SND_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

/* Streams the driver keeps information about; QEMU offers at most 10. */
#define VIRTIO_SND_MAX_STREAMS 16U

/*
 * Ring sizes. The control queue carries one request at a time (two
 * descriptors), the event queue holds the buffers the device reports through,
 * the tx queue carries two descriptors per period.
 */
#define VIRTIO_SND_CONTROL_QUEUE_SIZE 8U
#define VIRTIO_SND_EVENT_QUEUE_SIZE 16U
#define VIRTIO_SND_TX_QUEUE_SIZE 64U

/*
 * The control page: the request at the start, the response from
 * VIRTIO_SND_CONTROL_RESPONSE_OFFSET on. Item information for up to
 * VIRTIO_SND_MAX_STREAMS streams (32 bytes each) fits with room to spare.
 */
#define VIRTIO_SND_CONTROL_RESPONSE_OFFSET 512U
#define VIRTIO_SND_CONTROL_RESPONSE_MAX 3072U

/* A control request the device has not answered within this long is given up on, and so is the device. */
#define VIRTIO_SND_CONTROL_TIMEOUT_US 2000000ULL

/* No playback stream was chosen. */
#define VIRTIO_SND_NO_STREAM 0xFFFFFFFFU

/* One page of DMA memory and where the CPU sees it. */
typedef struct {
	uint64_t physical;
	uint8_t *virtual_address;
} virtio_snd_page_t;

typedef struct virtio_snd_device {
	virtio_device_t transport;
	ioreg_id_t ioreg_family;
	ioreg_id_t ioreg_node;

	/* The device configuration space (section 5.14.4). */
	uint32_t jacks;
	uint32_t streams;
	uint32_t chmaps;

	virtqueue_t controlq;
	virtqueue_t eventq;
	virtqueue_t txq;

	virtio_snd_page_t control_page;
	bool control_dead;
	uint64_t control_requests;
	uint64_t control_failures;

	/* What PCM_INFO said, for the streams the driver keeps (the first VIRTIO_SND_MAX_STREAMS). */
	virtio_snd_pcm_info_t pcm_info[VIRTIO_SND_MAX_STREAMS];
	uint32_t pcm_info_count;

	/* The output stream /dev/audio0 plays on, or VIRTIO_SND_NO_STREAM. */
	uint32_t playback_stream;

	bool attached;
} virtio_snd_device_t;

/*
 * virtio_snd_control:
 *
 * Send the request the caller built at the start of the control page and
 * wait for the device's answer, polling the used ring (control interrupts are
 * suppressed) for at most VIRTIO_SND_CONTROL_TIMEOUT_US. The response is
 * left at VIRTIO_SND_CONTROL_RESPONSE_OFFSET; *response_bytes is how much of
 * it the device wrote (the header included). A request that times out
 * poisons the device: it may still write into the page, so nothing else is
 * ever sent (VIRTIO_SND_E_DEAD from then on).
 */
virtio_snd_error_t virtio_snd_control(virtio_snd_device_t *device, uint32_t request_bytes, uint32_t response_capacity, uint32_t *response_bytes);

/*
 * virtio_snd_pcm_request:
 *
 * A stream request that is just the header and a stream ID (PREPARE, RELEASE,
 * START, STOP), and the wait for its status.
 */
virtio_snd_error_t virtio_snd_pcm_request(virtio_snd_device_t *device, uint32_t request, uint32_t stream_id);

#endif
