#ifndef NXU_VM_DMA_H
#define NXU_VM_DMA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	DMA_TO_DEVICE,
	DMA_FROM_DEVICE,
	DMA_BIDIRECTIONAL
} dma_direction_t;

typedef struct {
	/* Map physical storage into a device-visible IOVA range. */
	bool (*map)(void *context, uint64_t physical_address, uint64_t size, dma_direction_t direction,
				uint64_t address_mask, uint64_t *device_address);
	void (*unmap)(void *context, uint64_t device_address, uint64_t size, dma_direction_t direction);
} dma_iommu_ops_t;

typedef struct {
	/* True when CPU and device observe coherent cache contents. */
	bool coherent;
	/* Highest bus address this device can reach, inclusive. */
	uint64_t address_mask;
	/* Optional platform IOMMU domain for this device. */
	const dma_iommu_ops_t *iommu_ops;
	void *iommu_context;
} dma_device_t;

typedef struct {
	uint64_t physical_address;
	uint64_t device_address;
	void *virtual_address;
	uint64_t size;
	uint64_t page_count;
} dma_buffer_t;

typedef struct {
	void *original_virtual_address;
	uint64_t original_physical_address;
	void *dma_virtual_address;
	uint64_t device_address;
	uint64_t size;
	dma_direction_t direction;
	dma_buffer_t bounce_buffer;
	bool active;
	bool iommu_mapped;
} dma_mapping_t;

/* Allocate a physically contiguous buffer reachable within the device mask. */
bool dma_buffer_alloc(const dma_device_t *device, uint64_t size, dma_buffer_t *buffer);
bool dma_buffer_free(dma_buffer_t *buffer);
bool dma_page_alloc(const dma_device_t *device, uint64_t *physical_address, void **virtual_address);

/* Map a pinned direct-map range, using a reachable bounce buffer if needed. */
bool dma_map_buffer(const dma_device_t *device, void *virtual_address, uint64_t size, dma_direction_t direction,
				dma_mapping_t *mapping);
bool dma_sync_mapping_for_device(const dma_device_t *device, dma_mapping_t *mapping);
bool dma_sync_mapping_for_cpu(const dma_device_t *device, dma_mapping_t *mapping);
bool dma_unmap_buffer(const dma_device_t *device, dma_mapping_t *mapping);

/* Map a pinned direct-map range; the current backend uses identity DMA addresses. */
bool dma_map_single(const dma_device_t *device, const void *virtual_address, uint64_t size,
					uint64_t *device_address);

/*
 * Sync transfers ownership of the rounded cache-line range. For noncoherent
 * devices, callers must not concurrently access other data in those lines
 * until ownership is returned with dma_sync_for_cpu().
 */
bool dma_sync_for_device(const dma_device_t *device, const void *address, uint64_t size, dma_direction_t direction);
bool dma_sync_for_cpu(const dma_device_t *device, const void *address, uint64_t size, dma_direction_t direction);
uint32_t dma_cache_line_size(void);

#endif
