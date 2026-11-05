#include <drivers/virtio/virtqueue.h>

#include <kern/machine/barrier.h>
#include <vm/pmm.h>
#include <kern/lock.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static nxu_stats_virtio_t g_virtqueue_stats;
static nxu_spinlock_t g_virtqueue_stats_lock = NXU_SPINLOCK_INIT;

static void
virtqueue_count(uint64_t *counter, uint64_t amount)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_virtqueue_stats_lock);
	*counter += amount;
	nxu_spin_unlock_irqrestore(&g_virtqueue_stats_lock, irq);
}

void
virtqueue_statistics(nxu_stats_virtio_t *out)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_virtqueue_stats_lock);
	*out = g_virtqueue_stats;
	nxu_spin_unlock_irqrestore(&g_virtqueue_stats_lock, irq);
	out->struct_size = sizeof(*out);
	out->version = NXU_STATS_VERSION;
}

#define VIRTQUEUE_STORAGE_SIZE PMM_PAGE_SIZE

/*
 * virtqueue_align_up:
 *
 * Round a queue metadata offset up to the required split-ring alignment.
 */
static uint64_t
virtqueue_align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static inline void
virtqueue_publish_barrier(void)
{
	ml_dma_wmb();
}

static inline void
virtqueue_consume_barrier(void)
{
	ml_dma_rmb();
}

static bool
virtqueue_unmap_chain(virtqueue_t *queue, uint16_t head)
{
	bool ok = true;
	for (uint16_t traversed = 0U; traversed < queue->size; traversed++) {
		if (head >= queue->size || head >= VIRTQUEUE_MAX_SIZE) return false;
		virtq_desc_t *descriptor = &queue->descriptors[head];
		uint16_t next = descriptor->next;
		uint16_t flags = descriptor->flags;
		dma_mapping_t *mapping = &queue->payload_mappings[head];
		if (mapping->active) {
			uint64_t original_physical_address = mapping->original_physical_address;
			if (!dma_unmap_buffer(&queue->dma, mapping)) ok = false;
			descriptor->address = original_physical_address;
		}
		if ((flags & VIRTQ_DESC_F_NEXT) == 0U) return ok;
		head = next;
	}
	return false;
}

static bool
virtqueue_map_chain(virtqueue_t *queue, uint16_t head)
{
	uint16_t mapped[VIRTQUEUE_MAX_SIZE];
	uint16_t mapped_count = 0U;
	for (uint16_t traversed = 0U; traversed < queue->size; traversed++) {
		if (head >= queue->size || head >= VIRTQUEUE_MAX_SIZE) goto fail;
		virtq_desc_t *descriptor = &queue->descriptors[head];
		uint16_t descriptor_index = head;
		if ((descriptor->flags & VIRTQ_DESC_F_INDIRECT) != 0U) goto fail;
		if (descriptor->length != 0U) {
			dma_mapping_t *mapping = &queue->payload_mappings[head];
			if (mapping->active) goto fail;
			uint64_t virtual_address;
			if (!vmm_physical_to_higher_half(descriptor->address, &virtual_address) ||
				!dma_map_buffer(&queue->dma, (void *)(uintptr_t)virtual_address, descriptor->length,
								(descriptor->flags & VIRTQ_DESC_F_WRITE) != 0U ? DMA_FROM_DEVICE : DMA_TO_DEVICE,
								mapping)) goto fail;
			descriptor->address = mapping->device_address;
			mapped[mapped_count++] = descriptor_index;
		}
		if ((descriptor->flags & VIRTQ_DESC_F_NEXT) == 0U) return true;
		head = descriptor->next;
	}
	fail:
	for (uint16_t index = 0U; index < mapped_count; index++) {
		uint16_t descriptor_index = mapped[index];
		virtq_desc_t *descriptor = &queue->descriptors[descriptor_index];
		dma_mapping_t *mapping = &queue->payload_mappings[descriptor_index];
		uint64_t original_physical_address = mapping->original_physical_address;
		(void)dma_unmap_buffer(&queue->dma, mapping);
		descriptor->address = original_physical_address;
	}
	return false;
}

/*
 * virtqueue_init:
 *
 * Allocate one physically contiguous split-ring page and initialize
 * descriptor/free-list state.
 */
bool
virtqueue_init(virtqueue_t *queue, const dma_device_t *dma, uint16_t size)
{
	if (queue == 0 || dma == 0 || size == 0U || size > VIRTQUEUE_MAX_SIZE) return false;

	if ((size & (uint16_t)(size - 1U)) != 0U) return false;
	if (!vmm_higher_half_direct_map_enabled()) return false;

	memset(queue, 0, sizeof(*queue));

	uint64_t descriptors_offset = 0ULL;
	uint64_t descriptors_size = (uint64_t)size * sizeof(virtq_desc_t);
	uint64_t available_offset = descriptors_size;
	uint64_t available_size = 4ULL + (uint64_t)size * sizeof(uint16_t);
	uint64_t alignment = dma_cache_line_size();
	if (alignment < 4ULL) alignment = 4ULL;
	uint64_t used_offset = virtqueue_align_up(available_offset + available_size, alignment);
	uint64_t used_size = 4ULL + (uint64_t)size * sizeof(virtq_used_elem_t);

	if (used_offset + used_size > VIRTQUEUE_STORAGE_SIZE) return false;

	dma_buffer_t storage;
	if (!dma_buffer_alloc(dma, PMM_PAGE_SIZE, &storage)) return false;
	if (!dma_map_buffer(dma, storage.virtual_address, storage.size, DMA_BIDIRECTIONAL, &queue->storage_mapping) ||
		queue->storage_mapping.dma_virtual_address != storage.virtual_address) {
		if (queue->storage_mapping.active) (void)dma_unmap_buffer(dma, &queue->storage_mapping);
		(void)dma_buffer_free(&storage);
		return false;
	}

	uint8_t *base = storage.virtual_address;
	queue->size = size;
	queue->storage_physical = storage.physical_address;
	queue->storage_virtual = storage.virtual_address;
	queue->storage_pages = (uint32_t)storage.page_count;
	queue->storage_size = storage.size;
	queue->descriptors_size = descriptors_size;
	queue->available_offset = available_offset;
	queue->available_size = available_size;
	queue->used_offset = used_offset;
	queue->used_size = used_size;
	queue->dma = *dma;
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
		queue->free_next[index] = index + 1U < size ? (uint16_t)(index + 1U) : UINT16_MAX;
	}

	queue->initialized = true;
	virtqueue_count(&g_virtqueue_stats.live_queues, 1ULL);
	return true;
}

/*
 * virtqueue_init_legacy:
 *
 * Legacy VirtIO-PCI layout: the device derives the used ring address from the
 * queue PFN and the fixed 4096-byte QUEUE_ALIGN, so it is not free to sit right
 * behind the available ring as in the modern layout above.
 */
bool
virtqueue_init_legacy(virtqueue_t *queue, const dma_device_t *dma, uint16_t size)
{
	if (queue == 0 || dma == 0 || size == 0U || size > VIRTQUEUE_LEGACY_MAX_SIZE) return false;

	if ((size & (uint16_t)(size - 1U)) != 0U) return false;
	if (!vmm_higher_half_direct_map_enabled()) return false;

	uint64_t descriptors_size = (uint64_t)size * sizeof(virtq_desc_t);
	uint64_t available_offset = descriptors_size;
	uint64_t available_size = 6ULL + (uint64_t)size * sizeof(uint16_t);
	uint64_t used_offset = virtqueue_align_up(available_offset + available_size, PMM_PAGE_SIZE);
	uint64_t used_size = 6ULL + (uint64_t)size * sizeof(virtq_used_elem_t);
	uint64_t total = virtqueue_align_up(used_offset + used_size, PMM_PAGE_SIZE);
	memset(queue, 0, sizeof(*queue));

	dma_buffer_t storage;
	if (!dma_buffer_alloc(dma, total, &storage)) return false;
	if (!dma_map_buffer(dma, storage.virtual_address, storage.size, DMA_BIDIRECTIONAL, &queue->storage_mapping) ||
		queue->storage_mapping.dma_virtual_address != storage.virtual_address) {
		if (queue->storage_mapping.active) (void)dma_unmap_buffer(dma, &queue->storage_mapping);
		(void)dma_buffer_free(&storage);
		return false;
	}

	uint8_t *base = storage.virtual_address;
	queue->size = size;
	queue->storage_physical = storage.physical_address;
	queue->storage_virtual = storage.virtual_address;
	queue->storage_pages = (uint32_t)storage.page_count;
	queue->storage_size = storage.size;
	queue->descriptors_size = descriptors_size;
	queue->available_offset = available_offset;
	queue->available_size = available_size;
	queue->used_offset = used_offset;
	queue->used_size = used_size;
	queue->dma = *dma;
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
		queue->free_next[index] = index + 1U < usable ? (uint16_t)(index + 1U) : UINT16_MAX;
	}

	queue->initialized = true;
	virtqueue_count(&g_virtqueue_stats.live_queues, 1ULL);
	return true;
}

bool
virtqueue_prepare_device(virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized || !queue->storage_mapping.active) return false;
	const uint8_t *base = queue->storage_virtual;
	return dma_sync_for_device(&queue->dma, base, queue->descriptors_size, DMA_TO_DEVICE) &&
		   dma_sync_for_device(&queue->dma, base + queue->available_offset, queue->available_size, DMA_TO_DEVICE) &&
		   dma_sync_for_device(&queue->dma, base + queue->used_offset, queue->used_size, DMA_FROM_DEVICE);
}

/*
 * virtqueue_destroy:
 *
 * Release queue metadata after device ownership has ended.
 */
bool
virtqueue_destroy(virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return false;

	dma_buffer_t storage = {.physical_address = queue->storage_physical,
							 .device_address = queue->storage_physical,
							 .virtual_address = queue->storage_virtual,
							 .size = queue->storage_size,
							 .page_count = queue->storage_pages};
	for (uint16_t descriptor = 0U; descriptor < VIRTQUEUE_MAX_SIZE; descriptor++) {
		if (queue->payload_mappings[descriptor].active) (void)dma_unmap_buffer(&queue->dma, &queue->payload_mappings[descriptor]);
	}
	if (queue->storage_mapping.active) (void)dma_unmap_buffer(&queue->dma, &queue->storage_mapping);
	memset(queue, 0, sizeof(*queue));
	/* Removed from service even if returning its backing pages fails. */
	virtqueue_count(&g_virtqueue_stats.live_queues, UINT64_MAX);

	return dma_buffer_free(&storage);
}

/*
 * virtqueue_alloc_descriptor:
 *
 * Remove one descriptor from the queue-local free list.
 */
bool
virtqueue_alloc_descriptor(virtqueue_t *queue, uint16_t *index)
{
	if (queue == 0 || index == 0 || !queue->initialized) return false;

	if (queue->free_count == 0U || queue->free_head == UINT16_MAX) {
		virtqueue_count(&g_virtqueue_stats.descriptor_exhaustions, 1ULL);
		return false;
	}

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
bool
virtqueue_free_descriptor(virtqueue_t *queue, uint16_t index)
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
bool
virtqueue_submit(virtqueue_t *queue, uint16_t head)
{
	if (queue == 0 || !queue->initialized || head >= queue->size || head >= VIRTQUEUE_MAX_SIZE) {
		virtqueue_count(&g_virtqueue_stats.rejected_submissions, 1ULL);
		return false;
	}
	if (!virtqueue_map_chain(queue, head)) return false;

	uint16_t available = *queue->available_index;
	uint16_t ring_slot = (uint16_t)(available % queue->size);
	queue->available_ring[ring_slot] = head;
	const uint8_t *base = queue->storage_virtual;
	if (!dma_sync_for_device(&queue->dma, base, queue->descriptors_size, DMA_TO_DEVICE) ||
		!dma_sync_for_device(&queue->dma, (const void *)&queue->available_ring[ring_slot], sizeof(uint16_t),
						 DMA_TO_DEVICE)) {
		(void)virtqueue_unmap_chain(queue, head);
		return false;
	}
	virtqueue_publish_barrier();
	*queue->available_index = (uint16_t)(available + 1U);
	if (!dma_sync_for_device(&queue->dma, (const void *)queue->available_index, sizeof(uint16_t), DMA_TO_DEVICE)) {
		return false;
	}
	virtqueue_publish_barrier();
	virtqueue_count(&g_virtqueue_stats.submissions, 1ULL);
	return true;
}

/*
 * virtqueue_pop_used:
 *
 * Consume the next used-ring completion after ordering device writes
 * before CPU reads.
 */
bool
virtqueue_pop_used(virtqueue_t *queue, uint32_t *id, uint32_t *length)
{
	if (queue == 0 || id == 0 || length == 0 || !queue->initialized) {
		return false;
	}

	const uint8_t *base = queue->storage_virtual;
	if (!dma_sync_for_cpu(&queue->dma, base + queue->used_offset, queue->used_size, DMA_FROM_DEVICE)) return false;
	uint16_t device_index = *queue->used_index;
	virtqueue_consume_barrier();

	if (queue->last_used_index == device_index) return false;

	volatile virtq_used_elem_t *element = &queue->used_ring[queue->last_used_index % queue->size];

	uint32_t used_id = element->id;
	uint32_t used_length = element->length;
	virtqueue_consume_barrier();
	bool valid = used_id < queue->size && used_id < VIRTQUEUE_MAX_SIZE &&
			 virtqueue_unmap_chain(queue, (uint16_t)used_id);

	uint64_t irq = nxu_spin_lock_irqsave(&g_virtqueue_stats_lock);
	g_virtqueue_stats.completions++;
	g_virtqueue_stats.used_bytes += used_length;
	if (!valid) g_virtqueue_stats.invalid_completions++;
	nxu_spin_unlock_irqrestore(&g_virtqueue_stats_lock, irq);
	queue->last_used_index++;
	*id = valid ? used_id : UINT32_MAX;
	*length = valid ? used_length : 0U;
	return true;
}

uint64_t
virtqueue_desc_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;
	return queue->storage_mapping.device_address;
}

uint64_t
virtqueue_available_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;

	return queue->storage_mapping.device_address + queue->available_offset;
}

uint64_t
virtqueue_used_physical(const virtqueue_t *queue)
{
	if (queue == 0 || !queue->initialized) return 0ULL;

	return queue->storage_mapping.device_address + queue->used_offset;
}
