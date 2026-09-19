#ifndef NXU_PLATFORM_H
#define NXU_PLATFORM_H

#include <platform/dtb.h>

#include <stdbool.h>
#include <stdint.h>

#define PLATFORM_MAX_MEMORY_REGIONS 8U
#define PLATFORM_MAX_VIRTIO_MMIO_DEVICES 32U

typedef struct {
	uint64_t base;
	uint64_t size;
} platform_region_t;

typedef struct {
	platform_region_t memory_regions[PLATFORM_MAX_MEMORY_REGIONS];
	uint32_t memory_region_count;

	platform_region_t uart;
	platform_region_t fw_cfg;

	platform_region_t gic_distributor;
	platform_region_t gic_redistributor;

	platform_region_t rtc;

	platform_region_t pcie_ecam;
	uint32_t pcie_bus_start;
	uint32_t pcie_bus_end;

	platform_region_t
		virtio_mmio[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];

	uint32_t virtio_mmio_intid[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_irq_flags[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_count;
} platform_t;

bool platform_discover(
	const dtb_t *dtb,
	platform_t *platform
);

/**
 * platform_bootstrap - Discover and retain the boot platform.
 * @dtb: Initialized boot Device Tree.
 *
 * Return: true on success, otherwise false.
 */
bool platform_bootstrap(const dtb_t *dtb);

/**
 * platform_get - Return the permanent boot platform.
 *
 * Return: Permanent platform state after successful initialization,
 * otherwise null.
 */
const platform_t *platform_get(void);

void platform_dump(const platform_t *platform);

#endif
