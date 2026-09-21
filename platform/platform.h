#ifndef NXU_PLATFORM_H
#define NXU_PLATFORM_H

#include <platform/dtb.h>

#include <stdbool.h>
#include <stdint.h>

#define PLATFORM_MAX_MEMORY_REGIONS 8U
#define PLATFORM_MAX_VIRTIO_MMIO_DEVICES 32U

/* The most CPUs platform discovery will record; the kernel's own limit (NXU_MAX_CPUS) may be lower. */
#define PLATFORM_MAX_CPUS 16U

/* How firmware wants PSCI calls made: which instruction traps to it. */
typedef enum {
	PLATFORM_PSCI_NONE,
	PLATFORM_PSCI_HVC,
	PLATFORM_PSCI_SMC
} platform_psci_method_t;

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

	/*
	 * CPUs the Device Tree lists, in its order (the first is the boot CPU
	 * on the platforms this kernel runs on). The value is the CPU's MPIDR
	 * affinity fields (Aff3:Aff2:Aff1:Aff0), which is what PSCI CPU_ON and
	 * the GIC identify a CPU by; it is not a logical CPU number.
	 */
	uint64_t cpu_mpidr[PLATFORM_MAX_CPUS];
	uint32_t cpu_count;
	platform_psci_method_t psci_method;

	platform_region_t pcie_ecam;
	uint32_t pcie_bus_start;
	uint32_t pcie_bus_end;

	platform_region_t
		virtio_mmio[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];

	uint32_t virtio_mmio_intid[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_irq_flags[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_count;

	/*
	 * x86 legacy platform (platform/i386). Zero on arm64. On x86 the
	 * uart and rtc regions above are I/O-port ranges (base = first port,
	 * size = port count) rather than MMIO frames, and fw_cfg, the GIC
	 * regions and pcie_ecam stay empty: PCI is enumerated through
	 * configuration mechanism 1 (see platform/i386/pci.h) and the counts
	 * below only summarize what that enumeration found.
	 */
	uint32_t pci_bus_count;
	uint32_t pci_device_count;
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
