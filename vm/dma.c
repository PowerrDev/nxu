#include <vm/dma.h>

#include <kern/machine/barrier.h>
#include <vm/pmm.h>
#include <vm/vm_kmap.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool
dma_address_range_valid(uint64_t address, uint64_t size, uint64_t mask)
{
	if (size == 0ULL || address > mask || size - 1ULL > mask - address) return false;
	return true;
}

uint32_t
dma_cache_line_size(void)
{
	return ml_dma_cache_line_size();
}

bool
dma_buffer_alloc(const dma_device_t *device, uint64_t size, dma_buffer_t *buffer)
{
	if (device == 0 || buffer == 0 || size == 0ULL || !vm_kmap_direct_map_enabled()) return false;
	if (size > UINT64_MAX - (PMM_PAGE_SIZE - 1ULL)) return false;
	uint64_t pages = (size + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
	if (pages > UINT32_MAX || pages > UINT64_MAX / PMM_PAGE_SIZE) return false;
	uint64_t allocation_size = pages * PMM_PAGE_SIZE;
	if (sizeof(size_t) < sizeof(allocation_size) && allocation_size > (uint64_t)((size_t)-1)) return false;
	uint64_t physical_address;
	bool iommu_available = device->iommu_ops != 0 && device->iommu_ops->map != 0 && device->iommu_ops->unmap != 0;
	if (iommu_available) {
		if (!pmm_allocate_contiguous_pages(pages, &physical_address)) return false;
	} else if (!pmm_allocate_contiguous_pages_below(pages, device->address_mask, &physical_address)) {
		return false;
	}
	uint64_t virtual_address;
	if (!vm_kmap_phys_to_direct(physical_address, &virtual_address)) {
		(void)pmm_free_contiguous_pages(physical_address, pages);
		return false;
	}
	*buffer = (dma_buffer_t){.physical_address = physical_address,
							 .device_address = physical_address,
							 .virtual_address = (void *)(uintptr_t)virtual_address,
							 .size = allocation_size,
							 .page_count = pages};
	memset(buffer->virtual_address, 0, (size_t)allocation_size);
	return true;
}

bool
dma_buffer_free(dma_buffer_t *buffer)
{
	if (buffer == 0 || buffer->page_count == 0ULL || buffer->physical_address == 0ULL) return false;
	uint64_t physical_address = buffer->physical_address;
	uint64_t page_count = buffer->page_count;
	*buffer = (dma_buffer_t){0};
	return pmm_free_contiguous_pages(physical_address, page_count);
}

bool
dma_page_alloc(const dma_device_t *device, uint64_t *physical_address, void **virtual_address)
{
	if (device == 0 || physical_address == 0 || virtual_address == 0) return false;
	dma_buffer_t buffer;
	if (!dma_buffer_alloc(device, PMM_PAGE_SIZE, &buffer)) return false;
	*physical_address = buffer.physical_address;
	*virtual_address = buffer.virtual_address;
	return true;
}

bool
dma_map_single(const dma_device_t *device, const void *virtual_address, uint64_t size, uint64_t *device_address)
{
	if (device == 0 || virtual_address == 0 || device_address == 0 || size == 0ULL) return false;
	uint64_t physical_address;
	if (!vm_kmap_direct_to_phys((uint64_t)(uintptr_t)virtual_address, &physical_address) ||
		!dma_address_range_valid(physical_address, size, device->address_mask)) return false;
	*device_address = physical_address;
	return true;
}

bool
dma_map_buffer(const dma_device_t *device, void *virtual_address, uint64_t size, dma_direction_t direction,
			   dma_mapping_t *mapping)
{
	if (device == 0 || virtual_address == 0 || mapping == 0 || size == 0ULL || direction > DMA_BIDIRECTIONAL ||
		size > (uint64_t)((size_t)-1)) return false;
	*mapping = (dma_mapping_t){0};

	uint64_t physical_address;
	if (!vm_kmap_direct_to_phys((uint64_t)(uintptr_t)virtual_address, &physical_address) ||
		size - 1ULL > UINT64_MAX - physical_address) return false;
	void *original_virtual_address = virtual_address;
	uint64_t original_physical_address = physical_address;

	uint64_t device_address = 0ULL;
	bool iommu_mapped = false;
	if (device->iommu_ops != 0 && device->iommu_ops->map != 0 && device->iommu_ops->unmap != 0) {
		iommu_mapped = device->iommu_ops->map(device->iommu_context, physical_address, size, direction,
											 device->address_mask, &device_address);
		if (iommu_mapped && !dma_address_range_valid(device_address, size, device->address_mask)) {
			if (device->iommu_ops->unmap != 0)
				device->iommu_ops->unmap(device->iommu_context, device_address, size, direction);
			iommu_mapped = false;
		}
	}
	if (!iommu_mapped && !dma_address_range_valid(physical_address, size, device->address_mask)) {
		dma_buffer_t bounce;
		if (!dma_buffer_alloc(device, size, &bounce)) return false;
		if (direction != DMA_FROM_DEVICE) memcpy(bounce.virtual_address, virtual_address, (size_t)size);
		physical_address = bounce.physical_address;
		virtual_address = bounce.virtual_address;
		if (device->iommu_ops != 0 && device->iommu_ops->map != 0 && device->iommu_ops->unmap != 0) {
			iommu_mapped = device->iommu_ops->map(device->iommu_context, physical_address, size, direction,
												 device->address_mask, &device_address);
			if (iommu_mapped && !dma_address_range_valid(device_address, size, device->address_mask)) {
				if (device->iommu_ops->unmap != 0)
					device->iommu_ops->unmap(device->iommu_context, device_address, size, direction);
				iommu_mapped = false;
			}
		}
		if (!iommu_mapped) {
			if (!dma_address_range_valid(physical_address, size, device->address_mask)) {
				(void)dma_buffer_free(&bounce);
				return false;
			}
			device_address = physical_address;
		}
		*mapping = (dma_mapping_t){.original_virtual_address = original_virtual_address,
									 .original_physical_address = original_physical_address,
									 .dma_virtual_address = virtual_address,
									 .device_address = device_address,
									 .size = size,
									 .direction = direction,
									 .bounce_buffer = bounce,
									 .active = true,
									 .iommu_mapped = iommu_mapped};
	} else {
		if (!iommu_mapped) device_address = physical_address;
		*mapping = (dma_mapping_t){.original_virtual_address = original_virtual_address,
									 .original_physical_address = original_physical_address,
									 .dma_virtual_address = virtual_address,
									 .device_address = device_address,
									 .size = size,
									 .direction = direction,
									 .active = true,
									 .iommu_mapped = iommu_mapped};
	}
	if (dma_sync_mapping_for_device(device, mapping)) return true;
	if (mapping->iommu_mapped && device->iommu_ops != 0 && device->iommu_ops->unmap != 0)
		device->iommu_ops->unmap(device->iommu_context, mapping->device_address, mapping->size, mapping->direction);
	if (mapping->bounce_buffer.page_count != 0ULL) (void)dma_buffer_free(&mapping->bounce_buffer);
	*mapping = (dma_mapping_t){0};
	return false;
}

bool
dma_sync_mapping_for_device(const dma_device_t *device, dma_mapping_t *mapping)
{
	if (device == 0 || mapping == 0 || !mapping->active) return false;
	if (mapping->bounce_buffer.page_count != 0ULL && mapping->direction != DMA_FROM_DEVICE)
		memcpy(mapping->dma_virtual_address, mapping->original_virtual_address, (size_t)mapping->size);
	return dma_sync_for_device(device, mapping->dma_virtual_address, mapping->size, mapping->direction);
}

bool
dma_sync_mapping_for_cpu(const dma_device_t *device, dma_mapping_t *mapping)
{
	if (device == 0 || mapping == 0 || !mapping->active) return false;
	bool ok = dma_sync_for_cpu(device, mapping->dma_virtual_address, mapping->size, mapping->direction);
	if (ok && mapping->bounce_buffer.page_count != 0ULL && mapping->direction != DMA_TO_DEVICE)
		memcpy(mapping->original_virtual_address, mapping->dma_virtual_address, (size_t)mapping->size);
	return ok;
}

bool
dma_unmap_buffer(const dma_device_t *device, dma_mapping_t *mapping)
{
	if (device == 0 || mapping == 0 || !mapping->active) return false;
	bool ok = dma_sync_mapping_for_cpu(device, mapping);
	if (mapping->iommu_mapped && device->iommu_ops != 0 && device->iommu_ops->unmap != 0)
		device->iommu_ops->unmap(device->iommu_context, mapping->device_address, mapping->size, mapping->direction);
	if (mapping->bounce_buffer.page_count != 0ULL && !dma_buffer_free(&mapping->bounce_buffer)) ok = false;
	*mapping = (dma_mapping_t){0};
	return ok;
}

bool
dma_sync_for_device(const dma_device_t *device, const void *address, uint64_t size, dma_direction_t direction)
{
	uint64_t physical_address;
	if (device == 0 || address == 0 || size == 0ULL || direction > DMA_BIDIRECTIONAL ||
		!vm_kmap_direct_to_phys((uint64_t)(uintptr_t)address, &physical_address) ||
		size - 1ULL > UINT64_MAX - physical_address) return false;
	if (device->coherent) {
		ml_dma_wmb();
		return true;
	}
	uint64_t start = (uint64_t)(uintptr_t)address;
	if (direction == DMA_TO_DEVICE) ml_dma_cache_clean(start, size);
	else ml_dma_cache_clean_invalidate(start, size);
	ml_dma_wmb();
	return true;
}

bool
dma_sync_for_cpu(const dma_device_t *device, const void *address, uint64_t size, dma_direction_t direction)
{
	uint64_t physical_address;
	if (device == 0 || address == 0 || size == 0ULL || direction > DMA_BIDIRECTIONAL ||
		!vm_kmap_direct_to_phys((uint64_t)(uintptr_t)address, &physical_address) ||
		size - 1ULL > UINT64_MAX - physical_address) return false;
	if (device->coherent) {
		ml_dma_rmb();
		return true;
	}
	if (direction != DMA_TO_DEVICE) ml_dma_cache_invalidate((uint64_t)(uintptr_t)address, size);
	ml_dma_rmb();
	return true;
}
