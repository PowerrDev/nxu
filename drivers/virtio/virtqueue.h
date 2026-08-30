#ifndef NXU_DRIVERS_VIRTIO_VIRTQUEUE_H
#define NXU_DRIVERS_VIRTIO_VIRTQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTQUEUE_MAX_SIZE 128U

#define VIRTQ_DESC_F_NEXT 1U
#define VIRTQ_DESC_F_WRITE 2U
#define VIRTQ_DESC_F_INDIRECT 4U

typedef struct {
	uint64_t address;
	uint32_t length;
	uint16_t flags;
	uint16_t next;
} virtq_desc_t;

typedef struct {
	uint32_t id;
	uint32_t length;
} virtq_used_elem_t;

typedef struct {
	uint16_t size;
	uint16_t last_used_index;

	uint64_t storage_physical;
	void *storage_virtual;

	virtq_desc_t *descriptors;
	volatile uint16_t *available_flags;
	volatile uint16_t *available_index;
	volatile uint16_t *available_ring;
	volatile uint16_t *used_flags;
	volatile uint16_t *used_index;
	volatile virtq_used_elem_t *used_ring;

	uint16_t free_head;
	uint16_t free_count;
	uint16_t free_next[VIRTQUEUE_MAX_SIZE];

	bool initialized;
} virtqueue_t;

/*
 * virtqueue_init:
 *
 * Allocate and initialize one modern split virtqueue.
 *
 * The queue owns one physically contiguous page containing the descriptor
 * table, available ring, and used ring. The CPU accesses that storage through
 * the permanent higher-half direct map; the device receives physical
 * addresses through the transport.
 *
 * The queue object owns ring memory and descriptor allocation state. Request
 * buffers and descriptor contents remain the responsibility of the device
 * driver using the queue.
 *
 * Returns true when the queue is ready for transport publication.
 */
bool virtqueue_init(virtqueue_t *queue, uint16_t size);

/*
 * virtqueue_destroy:
 *
 * Release queue storage after the queue is no longer visible to a device.
 *
 * Returns true when the backing page was released.
 */
bool virtqueue_destroy(virtqueue_t *queue);

/*
 * virtqueue_alloc_descriptor:
 *
 * Reserve one descriptor from the queue-local free list.
 *
 * Returns true and writes the descriptor index when space is available.
 */
bool virtqueue_alloc_descriptor(virtqueue_t *queue, uint16_t *index);

/*
 * virtqueue_free_descriptor:
 *
 * Return one descriptor to the queue-local free list. The caller must ensure
 * the device no longer owns or references the descriptor.
 */
bool virtqueue_free_descriptor(virtqueue_t *queue, uint16_t index);

/*
 * virtqueue_submit:
 *
 * Publish one descriptor head to the available ring. Ring stores are ordered
 * before the available index update with an AArch64 DMA barrier.
 *
 * The caller notifies the transport separately.
 */
bool virtqueue_submit(virtqueue_t *queue, uint16_t head);

/*
 * virtqueue_pop_used:
 *
 * Consume one device-completed used-ring entry in queue order.
 *
 * Returns true when a completion was available.
 */
bool virtqueue_pop_used(
	virtqueue_t *queue,
	uint32_t *descriptor_id,
	uint32_t *length
);

uint64_t virtqueue_desc_physical(const virtqueue_t *queue);
uint64_t virtqueue_available_physical(const virtqueue_t *queue);
uint64_t virtqueue_used_physical(const virtqueue_t *queue);

#endif
