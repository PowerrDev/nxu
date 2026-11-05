#ifndef NXU_PLATFORM_H
#define NXU_PLATFORM_H

#include <platform/dtb.h>

#include <stdbool.h>
#include <stdint.h>

#define PLATFORM_MAX_MEMORY_REGIONS 8U
#define PLATFORM_MAX_VIRTIO_MMIO_DEVICES 32U

/* The most CPUs platform discovery will record; the kernel's own limit (NXU_MAX_CPUS) may be lower. */
#define PLATFORM_MAX_CPUS 16U

/* Maximum flattened nodes retained for the boot-time hardware tree view. */
#define PLATFORM_MAX_DEVICE_NODES 512U

/* How firmware wants PSCI calls made: which instruction traps to it. */
typedef enum { PLATFORM_PSCI_NONE, PLATFORM_PSCI_HVC, PLATFORM_PSCI_SMC } platform_psci_method_t;

typedef struct {
	uint64_t base;
	uint64_t size;
} platform_region_t;

/* A compact view of one FDT node. String data points into the retained DTB. */
typedef struct {
	const char *name;
	const char *compatible;
	const char *status;
	uint32_t parent_index;
	uint32_t depth;
	uint32_t compatible_length;
	platform_region_t reg;
	const void *reg_data;
	uint32_t reg_length;
	uint32_t reg_address_cells;
	uint32_t reg_size_cells;
	const void *interrupts;
	uint32_t interrupts_length;
	const void *ranges;
	uint32_t ranges_length;
	uint32_t ranges_child_address_cells;
	uint32_t ranges_parent_address_cells;
	uint32_t ranges_size_cells;
	uint32_t phandle;
	uint32_t interrupt_parent_phandle;
	uint32_t interrupt_parent_index;
	uint32_t interrupt_cells;
	uint32_t interrupt_parent_cells;
	bool ranges_present;
	bool interrupt_parent_resolved;
	bool has_reg;
	bool dma_coherent;
} platform_device_node_t;

typedef struct {
	/* Source blob retained for on-demand access to all node properties. */
	const dtb_t *source_dtb;

	/*
	 * Out-of-line storage for the flattened device tree. Kept as a pointer,
	 * not an inline array, so platform_t stays small enough to live on the
	 * kernel boot stack: at PLATFORM_MAX_DEVICE_NODES the inline array was
	 * ~57 KiB, which overflowed the 16 KiB boot stack the moment the x86 VM
	 * phase zeroed its stack-local platform_t. Only the device-tree port
	 * (arm64) populates this; it points the pointer at backing storage in
	 * platform_discover(). On ports without a device tree it stays null and
	 * device_node_count stays zero, so nothing dereferences it.
	 */
	platform_device_node_t *device_nodes;
	uint32_t device_node_count;

	platform_region_t memory_regions[PLATFORM_MAX_MEMORY_REGIONS];
	uint32_t memory_region_count;

	platform_region_t uart;

	/*
	 * A second "arm,pl011" in the Device Tree (QEMU virt with two -serial
	 * options): the serial link to the Trusted Enclave Processor, owned by
	 * drivers/tep/tep_mailbox.c. Empty when the machine has only the console.
	 */
	platform_region_t mailbox_uart;

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

	platform_region_t virtio_mmio[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];

	uint32_t virtio_mmio_intid[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	uint32_t virtio_mmio_irq_flags[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
	bool virtio_mmio_dma_coherent[PLATFORM_MAX_VIRTIO_MMIO_DEVICES];
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

bool platform_discover(const dtb_t *dtb, platform_t *platform);

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

/* Decode any reg entry retained for a discovered node, applying bus ranges. */
bool platform_device_reg_get(const platform_t *platform, uint32_t node_index, uint32_t entry_index,
							 platform_region_t *region);

/* Return a raw property value for one retained node; its bytes remain in the DTB. */
bool platform_device_property_get(const platform_t *platform, uint32_t node_index, const char *property_name,
								  const void **value, uint32_t *length);

#endif
