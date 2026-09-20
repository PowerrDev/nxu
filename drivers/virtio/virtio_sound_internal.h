#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_INTERNAL_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_INTERNAL_H

#include <drivers/virtio/virtio_snd.h>
#include <drivers/virtio/virtio_sound.h>
#include <drivers/virtio/virtio_sound_core.h>
#include <drivers/virtio/virtio_transport.h>
#include <drivers/virtio/virtqueue.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <kern/sched_prism/waitq.h>

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

/*
 * The playback ring: VIRTIO_SND_TX_SLOTS periods, one DMA page each. A
 * period's page holds the message the device reads in one descriptor (the
 * 4-byte header and up to VIRTIO_SND_PERIOD_MAX_BYTES of samples) and, in a
 * second descriptor at the end of the page, the status it writes back.
 * 4080 bytes is a whole number of frames for every layout the driver plays
 * (mono or stereo; 8, 16 or 32 bits, or float), so a period never ends in
 * the middle of a frame.
 */
#define VIRTIO_SND_TX_SLOTS 16U
#define VIRTIO_SND_SLOT_DATA_OFFSET 4U
#define VIRTIO_SND_SLOT_STATUS_OFFSET 4088U
#define VIRTIO_SND_PERIOD_MAX_BYTES 4080U
#define VIRTIO_SND_NO_SLOT 0xFFFFFFFFU

/* The stream is started once this many periods are queued (or when the writer drains). */
#define VIRTIO_SND_PREBUFFER_SLOTS 8U

/* After the last period completes the host may still be playing what it has buffered; wait this long before STOP. */
#define VIRTIO_SND_DRAIN_TAIL_US 120000ULL

_Static_assert(VIRTIO_SND_TX_SLOTS * 2U <= VIRTIO_SND_TX_QUEUE_SIZE, "the tx queue must hold every slot's two descriptors");
_Static_assert(VIRTIO_SND_SLOT_DATA_OFFSET + VIRTIO_SND_PERIOD_MAX_BYTES <= VIRTIO_SND_SLOT_STATUS_OFFSET, "a period must end before its status");
_Static_assert(VIRTIO_SND_SLOT_STATUS_OFFSET + 8U <= 4096U, "the status must fit in the page");

/* One page of DMA memory and where the CPU sees it. */
typedef struct {
	uint64_t physical;
	uint8_t *virtual_address;
} virtio_snd_page_t;

typedef enum {
	VIRTIO_SND_SLOT_FREE,
	VIRTIO_SND_SLOT_FILLING,
	VIRTIO_SND_SLOT_QUEUED
} virtio_snd_slot_state_t;

typedef struct {
	virtio_snd_page_t page;
	uint16_t data_desc;
	uint16_t status_desc;
	uint8_t descriptors; /* how many of the two are held, so a half-built slot can be undone */
	virtio_snd_slot_state_t state;
	uint32_t bytes;
} virtio_snd_slot_t;

/*
 * The one output stream /dev/audio0 plays on. Touched by the writer, by the
 * interrupt handler (completions) and by the polling paths, always with
 * interrupts masked: this kernel has one CPU, SMP would want a lock.
 */
typedef struct {
	bool open;
	bool nonblock;
	bool needs_configure;
	bool draining;

	/* The stream's state as the device last confirmed it. */
	virtio_snd_pcm_state_t state;

	virtio_snd_params_t params;
	virtio_snd_pcm_format_t format;
	uint32_t frame_bytes;

	virtio_snd_slot_t slots[VIRTIO_SND_TX_SLOTS];
	bool slots_allocated;
	uint32_t period_count;
	uint32_t fill_slot;
	uint32_t queued;

	waitq_t waitq;

	uint32_t latency_bytes;
	uint64_t bytes_written;
	uint64_t periods_submitted;
	uint64_t periods_completed;
	uint64_t xruns;
	uint64_t io_errors;
	uint64_t start_us;
	uint64_t end_us;

	/* Totals over every open. */
	uint64_t opens;
	uint64_t total_periods;
	uint64_t total_xruns;
} virtio_snd_playback_t;

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

	/* Where the device reports asynchronously: the buffers posted for it, one 8-byte event each. */
	virtio_snd_page_t event_page;
	uint16_t event_desc[VIRTIO_SND_EVENT_QUEUE_SIZE];
	uint64_t irq_count;
	uint64_t event_count;
	uint64_t xrun_events;
	uint64_t jack_events;
	uint32_t last_event;
	uint32_t last_event_data;

	/* What PCM_INFO said, for the streams the driver keeps (the first VIRTIO_SND_MAX_STREAMS). */
	virtio_snd_pcm_info_t pcm_info[VIRTIO_SND_MAX_STREAMS];
	uint32_t pcm_info_count;

	/* The output stream /dev/audio0 plays on, or VIRTIO_SND_NO_STREAM. */
	uint32_t playback_stream;
	virtio_snd_playback_t playback;

	bool ready;
	bool attached;
} virtio_snd_device_t;

/* DMA pages (virtio_sound.c). */
bool virtio_snd_allocate_page(virtio_snd_page_t *page);
void virtio_snd_free_page(virtio_snd_page_t *page);

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

/*
 * virtio_snd_set_params_request:
 *
 * PCM_SET_PARAMS for a stream, with no optional stream features.
 */
virtio_snd_error_t virtio_snd_set_params_request(virtio_snd_device_t *device, uint32_t stream_id, uint32_t buffer_bytes, uint32_t period_bytes, const virtio_snd_pcm_format_t *format);

/* /dev/audio0 (virtio_sound_dev.c): publish the playback stream as a character device. */
bool virtio_snd_dev_register(virtio_snd_device_t *device);

/*
 * The event queue and the interrupt (virtio_sound_pcm.c).
 *
 * virtio_snd_post_events puts an empty buffer on the event queue for every
 * descriptor's worth of room, before the device goes live. virtio_snd_irq is
 * the handler registered with the transport: it acknowledges the device,
 * takes the events and the completed periods and wakes a writer waiting for
 * room. Nothing in it sleeps or logs.
 */
bool virtio_snd_post_events(virtio_snd_device_t *device);
void virtio_snd_irq(uint32_t intid, void *context);

#endif
