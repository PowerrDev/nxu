#include <drivers/virtio/virtqueue.h>

#include <mach/machine/barrier.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTQUEUE_STORAGE_SIZE PMM_PAGE_SIZE

/*
 * virtqueue_align_up:
 *
 * Round a queue metadata offset up to the required split-ring alignment.
 */
static uint64_t virtqueue_align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static inline void virtqueue_publish_barrier(void)
{
	ml_dma_wmb();
}

static inline void virtqueue_consume_barrier(void)
{
	ml_dma_rmb();
}

/*
 * virtqueue_init:
 *
 * Allocate one physically contiguous split-ring page and initialize
 * descriptor/free-list state.
 */
bool virtqueue_init(virtqueue_t *queue, uint16_t size)
{
	if (queue == 0 || size == 0U || size > VIRTQUEUE_MAX_SIZE) return false;

	if ((size & (uint16_t)(size - 1U)) != 0U) return false;
	if (!vmm_higher_half_direct_map_enabled()) return false;

	memset(queue, 0, sizeof(*queue));

	uint64_t descriptors_offset = 0ULL;
	uint64_t descriptors_size = (uint64_t)size * sizeof(virtq_desc_t);
	uint64_t available_offset = descriptors_size;
	uint64_t available_size = 4ULL + (uint64_t)size * sizeof(uint16_t);
	uint64_t used_offset = virtqueue_align_up(
		available_offset + available_size,
		4ULL
	);
	uint64_t used_size = 4ULL + (uint64_t)size * sizeof(virtq_used_elem_t);

	if (used_offset + used_size > VIRTQUEUE_STORAGE_SIZE) return false;

	uint64_t physical;
	if (!pmm_allocate_page(&physical)) return false;

	uint64_t virtual_address;
	if (!vmm_physical_to_higher_half(physical, &virtual_address)) {
		(void)pmm_free_page(physical);
		return false;
	}

	memset((void *)virtual_address, 0, PMM_PAGE_SIZE);

	uint8_t *base = (uint8_t *)virtual_address;
	queue->size = size;
	queue->storage_physical = physical;
	queue->storage_virtual = (void *)virtual_address;
	queue->storage_pages = 1U;
	queue->descriptors = (virtq_desc_t *)(base + descriptors_offset);
	queue->available_flags = (volatile uint16_t *)(base + available_offset);
	queue->available_index = (volatile uint16_t *)(base + available_offset + 2ULL);
	queue->available_ring = (volatile uint16_t *)(base + available_offset + 4ULL);
	queue->used_flags = (volatile uint16_t *)(base + used_offset);
	queue->used_index = (volatile uint16_t *)(base + used_offset + 2ULL);
	queue->used_ring = (volatile virtq_used_elem_t *)(base + used_offset + 4ULL);
	queue->free_head = 0U;
	queue->free_count = size;
	queue->last_used_index = 0U;

	for (uint16_t index = 0U; index < size; index++) {
		queue->free_next[index] = index + 1U < size
			? (uint16_t)(index + 1U)
			: UINT16_MAX;
	}

	queue->initialized = true;
	return true;
}

/*
 * virtqueue_init_legacy:
 *
 * Legacy VirtIO-PCI layout: the device derives the used ring address from the
 * queue PFN and the fixed 4096-byte QUEUE_ALIGN, so it is not free to sit right
 * behind the available ring as in the modern layout above.
 */
bool virtqueue_init_legacy(virtqueue_t *queue, uint16_t size)
{
	if (queue == 0 || size == 0U || size > VIRTQUEUE_LEGACY_MAX_SIZE) return false;

	if ((size & (uint16_t)(size - 1U)) != 0U) return false;
	if (!vmm_higher_half_direct_map_enabled()) return false;

	uint64_t descriptors_size = (uint64_t)size * sizeof(virtq_desc_t);
	uint64_t available_offset = descriptors_size;
	uint64_t available_size = 6ULL + (uint64_t)size * sizeof(uint16_t);
	uint64_t used_offset = virtqueue_align_up(
		available_offset + available_size,
		PMM_PAGE_SIZE
	);
	uint64_t used_size = 6ULL + (uint64_t)size * sizeof(virtq_used_elem_t);
	uint64_t total = virtqueue_align_up(used_offset + used_size, PMM_PAGE_SIZE);
	uint32_t pages = (uint32_t)(total / PMM_PAGE_SIZE);

	memset(queue, 0, sizeof(*queue));

	uint64_t physical;
	if (!pmm_allocate_contiguous_pages(pages, &physical)) return false;

	uint64_t virtual_address;
	if (!vmm_physical_to_higher_half(physical, &virtual_address)) {
		(void)pmm_free_contiguous_pages(physical, pages);
		return false;
	}

	memset((void *)virtual_address, 0, (size_t)total);

	uint8_t *base = (uint8_t *)virtual_address;
	queue->size = size;
	queue->storage_physical = physical;
	queue->storage_virtual = (void *)virtual_address;
	queue->storage_pages = pages;
	queue->descriptors = (virtq_desc_t *)base;
	queue->available_flags = (volatile uint16_t *)(base + available_offset);
	queue->available_index = (volatile uint16_t *)(base + available_offset + 2ULL);
	queue->available_ring = (volatile uint16_t *)(base + available_offset + 4ULL);
	queue->used_flags = (volatile uint16_t *)(base + used_offset);
	queue->used_index = (volatile uint16_t *)(base + used_offset + 2ULL);
	queue->used_ring = (volatile virtq_used_elem_t *)(base + used_offset + 4ULL);
	queue->free_head = 0U;
	queue->last_used_index = 0U;

	uint16_t usable = size > VIRTQUEUE_MAX_SIZE ? (uint16_t)VIRTQUEUE_MAX_SIZE : size;

	queue->free_count = usable;

	for (uint16_t index = 0U; index < usable; index++) {
		queue->free_next[index] = index + 1U < usable
			? (uint16_t)(index + 1U)
			: UINT16_MAX;
	}

	queue->initialized = true;
	return true;
}

/*
 * virtqueue_destroy:
 *
 * Release queue metadata after device ownership has ended.
 */
bool virtqueue_destroy(virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return false;

	uint64_t physical = queue->storage_physical;
	uint32_t pages = queue->storage_pages;
	memset(queue, 0, sizeof(*queue));

	if (pages > 1U) return pmm_free_contiguous_pages(physical, pages);
	return pmm_free_page(physical);
}

/*
 * virtqueue_alloc_descriptor:
 *
 * Remove one descriptor from the queue-local free list.
 */
bool virtqueue_alloc_descriptor(virtqueue_t *queue, uint16_t *index)
{
	if (queue == 0 || index == 0 || !queue->initialized) return false;

	if (queue->free_count == 0U || queue->free_head == UINT16_MAX) return false;

	uint16_t descriptor = queue->free_head;
	queue->free_head = queue->free_next[descriptor];
	queue->free_next[descriptor] = UINT16_MAX;
	queue->free_count--;
	*index = descriptor;
	return true;
}

/*
 * virtqueue_free_descriptor:
 *
 * Return one descriptor to the queue-local free list after device
 * ownership has ended.
 */
bool virtqueue_free_descriptor(virtqueue_t *queue, uint16_t index)
{
	if (queue == 0 || !queue->initialized || index >= queue->size || index >= VIRTQUEUE_MAX_SIZE) return false;

	if (queue->free_count >= queue->size) return false;

	memset(&queue->descriptors[index], 0, sizeof(queue->descriptors[index]));
	queue->free_next[index] = queue->free_head;
	queue->free_head = index;
	queue->free_count++;
	return true;
}

/*
 * virtqueue_submit:
 *
 * Publish one descriptor head to the available ring with the required DMA
 * store ordering.
 */
bool virtqueue_submit(virtqueue_t *queue, uint16_t head)
{
	if (queue == 0 || !queue->initialized || head >= queue->size) return false;

	uint16_t available = *queue->available_index;
	queue->available_ring[available % queue->size] = head;
	virtqueue_publish_barrier();
	*queue->available_index = (uint16_t)(available + 1U);
	virtqueue_publish_barrier();
	return true;
}

/*
 * virtqueue_pop_used:
 *
 * Consume the next used-ring completion after ordering device writes
 * before CPU reads.
 */
bool virtqueue_pop_used(
	virtqueue_t *queue,
	uint32_t *id,
	uint32_t *length
)
{
	if (queue == 0 || id == 0 || length == 0 || !queue->initialized) {
		return false;
	}

	uint16_t device_index = *queue->used_index;
	virtqueue_consume_barrier();

	if (queue->last_used_index == device_index) return false;

	volatile virtq_used_elem_t *element = &queue->used_ring[queue->last_used_index % queue->size];

	uint32_t used_id = element->id;
	uint32_t used_length = element->length;
	virtqueue_consume_barrier();

	queue->last_used_index++;
	*id = used_id;
	*length = used_length;
	return true;
}

uint64_t virtqueue_desc_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;
	return queue->storage_physical;
}

uint64_t virtqueue_available_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;

	return queue->storage_physical +
		(uint64_t)((const uint8_t *)queue->available_flags -
		(const uint8_t *)queue->storage_virtual);
}

uint64_t virtqueue_used_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;

	return queue->storage_physical +
		(uint64_t)((const uint8_t *)queue->used_flags -
		(const uint8_t *)queue->storage_virtual);
}
