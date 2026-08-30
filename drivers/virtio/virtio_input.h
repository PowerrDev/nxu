#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_INPUT_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_INPUT_H

#include <drivers/input/input.h>
#include <drivers/virtio/virtio_mmio.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_INPUT_MAX_DEVICES 8U
#define VIRTIO_INPUT_EVENT_QUEUE_SIZE 64U
#define VIRTIO_INPUT_STATUS_QUEUE_SIZE 8U
#define VIRTIO_INPUT_NAME_MAX 128U

typedef struct {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} virtio_input_event_t;

typedef struct {
	virtio_mmio_device_t transport;
	virtqueue_t eventq;
	virtqueue_t statusq;

	uint64_t event_buffer_physical;
	volatile virtio_input_event_t *event_buffers;
	uint16_t descriptor_to_buffer[VIRTIO_INPUT_EVENT_QUEUE_SIZE];

	uint32_t input_device_id;
	input_device_class_t device_class;
	char name[VIRTIO_INPUT_NAME_MAX + 1U];
	uint64_t event_count;
	uint64_t irq_count;
	bool attached;
} virtio_input_device_t;

/*
 * virtio_input_attach:
 *
 * Attach the VirtIO Input driver to one negotiated MMIO transport.
 *
 * Queue 0 is maintained as a permanently populated device-writable event
 * queue. Each completed event descriptor is consumed, normalized, delivered
 * to the input subsystem, and returned to the available ring without an IRQ-
 * time allocation. Queue 1 is initialized as the status queue for future
 * host-directed output events.
 *
 * The transport remains responsible for MMIO protocol and interrupt status.
 * This driver owns VirtIO Input configuration, queue semantics, event
 * normalization, and device classification.
 *
 * Returns true after the device is classified, queued, and interrupt-enabled.
 */
bool virtio_input_attach(const virtio_mmio_device_t *transport);

/*
 * virtio_input_service:
 *
 * Poll attached VirtIO Input used rings and route any completed events.
 * This is safe to call from a non-IRQ host loop and provides a fallback when
 * platform interrupt delivery is not yet reliable. Local IRQs are masked while
 * the rings are drained so the IRQ handler cannot race the poller.
 */
void virtio_input_service(void);

uint32_t virtio_input_device_count(void);
const virtio_input_device_t *virtio_input_device(uint32_t index);
void virtio_input_dump(void);

#endif
