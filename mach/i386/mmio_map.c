/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/mmio_map.c
 *
 * Glue between the devices area and the VM area. PCI code that needs a
 * device's memory BAR once paging is on (virtio-input's modern transport)
 * calls i386_mmio_map(); the mapping itself is pmap_map_mmio(), which places
 * it uncached in the MMIO window, even above the direct map.
 */

#include <mach/i386/pmap.h>

#include <stdbool.h>
#include <stdint.h>

void *i386_mmio_map(uint64_t physical, uint64_t size)
{
	void *virtual_address = 0;

	if (!pmap_map_mmio(physical, size, &virtual_address)) return 0;

	return virtual_address;
}
