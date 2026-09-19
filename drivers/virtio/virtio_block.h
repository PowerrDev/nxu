#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_BLOCK_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_BLOCK_H

#include <drivers/block/block_device.h>
#include <drivers/virtio/virtio_transport.h>
#include <drivers/virtio/virtqueue.h>

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_BLOCK_MAX_DEVICES 4U
#define VIRTIO_BLOCK_QUEUE_SIZE 8U
#define VIRTIO_BLOCK_MAX_SECTORS_PER_REQUEST 8U

typedef struct {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
} virtio_block_request_header_t;

/*
 * struct virtio_block_device
 *
 * Driver state for one VirtIO block device.
 *
 * The first implementation serializes requests through one request queue and
 * one 4 KiB DMA bounce page. Queue interrupts are suppressed; synchronous
 * callers poll the used ring until device ownership returns. This keeps the
 * upper block-device contract stable while asynchronous I/O is deferred to a
 * later scheduler/wait-queue lesson.
 */
typedef struct {
	virtio_device_t transport;

	virtqueue_t requestq;

	block_device_t block;
	struct block_device block_storage;

	uint64_t control_physical;
	virtio_block_request_header_t *header;
	volatile uint8_t *status;

	uint64_t bounce_physical;
	uint8_t *bounce;

	volatile uint32_t lock;

	bool read_only;
	bool flush_supported;
	bool attached;

	uint64_t request_count;
	uint64_t read_request_count;
	uint64_t write_request_count;
	uint64_t flush_request_count;
} virtio_block_device_t;

/*
 * virtio_block_attach:
 *
 * Bind one VirtIO block device (any transport), publish requestq, discover
 * medium geometry, and register the resulting block device.
 */
bool virtio_block_attach(
	const virtio_device_t *transport
);

uint32_t virtio_block_device_count(void);

const virtio_block_device_t *virtio_block_device(
	uint32_t index
);

void virtio_block_dump(void);

#endif
