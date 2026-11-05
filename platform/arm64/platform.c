#include <kern/console/console.h>
#include <platform/platform.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PLATFORM_MAX_DTB_DEPTH 64U

static platform_t g_platform;
static bool g_platform_ready;

/*
 * Backing storage for platform_t::device_nodes. Held out of line (rather than
 * inline in platform_t) so the shared struct stays small enough for the x86
 * boot stack; see platform/platform.h. Discovery runs once, into g_platform,
 * so a single static array suffices and platform_discover() points the
 * pointer here after it zero-initializes the struct.
 */
static platform_device_node_t g_device_nodes[PLATFORM_MAX_DEVICE_NODES];

typedef struct {
	const char *name;
	uint32_t depth;
	const void *compatible;
	uint32_t compatible_length;
	const char *status;
	uint32_t device_node_index;

	uint32_t register_address_cells;
	uint32_t register_size_cells;

	/* Keep the adjacent 32-bit child-cell defaults 8-byte aligned as a pair. */
	uint32_t cell_alignment_pad;
	uint32_t child_address_cells;
	uint32_t child_size_cells;

	const void *reg;
	uint32_t reg_length;

	const void *bus_range;
	uint32_t bus_range_length;

	const void *interrupts;
	uint32_t interrupts_length;
	const void *ranges;
	uint32_t ranges_length;
	uint32_t phandle;
	uint32_t interrupt_parent_phandle;
	uint32_t interrupt_cells;
	bool ranges_present;
	bool dma_coherent;

	bool is_memory;
	bool is_uart;
	bool is_fw_cfg;
	bool is_gic;
	bool is_rtc;
	bool is_pcie;
	bool is_virtio_mmio;

	bool is_psci;
	bool psci_method_hvc;
	bool psci_method_smc;
	bool cpu_disabled;
} platform_node_t;

typedef struct {
	platform_t *platform;

	platform_node_t nodes[PLATFORM_MAX_DTB_DEPTH];

	bool failed;
} platform_discovery_context_t;

static bool
platform_string_equals(const char *left, const char *right)
{
	if (left == 0 || right == 0) {
		return false;
	}

	while (*left != '\0' && *right != '\0') {
		if (*left != *right) {
			return false;
		}

		left++;
		right++;
	}

	return *left == '\0' && *right == '\0';
}

static bool
platform_string_starts_with(const char *string, const char *prefix)
{
	if (string == 0 || prefix == 0) {
		return false;
	}

	while (*prefix != '\0') {
		if (*string != *prefix) {
			return false;
		}

		string++;
		prefix++;
	}

	return true;
}

static uint32_t
platform_read_be32(const void *address)
{
	const uint8_t *bytes = address;

	return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) | ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static bool
platform_read_cells(const uint8_t *value, uint32_t cell_count, uint64_t *result)
{
	if (value == 0 || result == 0 || cell_count > 2U) {
		return false;
	}

	uint64_t number = 0;

	for (uint32_t index = 0; index < cell_count; index++) {
		number <<= 32U;
		number |= platform_read_be32(value + index * 4U);
	}

	*result = number;

	return true;
}

static bool
platform_value_string_equals(const void *value, uint32_t length, const char *expected)
{
	if (value == 0 || expected == 0 || length == 0U) {
		return false;
	}

	const uint8_t *bytes = value;
	uint32_t index = 0;

	while (expected[index] != '\0') {
		if (index >= length || bytes[index] != (uint8_t)expected[index]) {
			return false;
		}

		index++;
	}

	return index < length && bytes[index] == '\0';
}

static bool
platform_value_has_nul(const void *value, uint32_t length)
{
	if (value == 0) return false;
	const uint8_t *bytes = value;
	for (uint32_t index = 0U; index < length; index++) {
		if (bytes[index] == '\0') return true;
	}
	return false;
}

static bool
platform_compatible_contains(const void *value, uint32_t length, const char *compatible)
{
	if (value == 0 || compatible == 0) {
		return false;
	}

	const uint8_t *bytes = value;
	uint32_t offset = 0;

	while (offset < length) {
		uint32_t item_length = 0;

		while (offset + item_length < length && bytes[offset + item_length] != '\0') {
			item_length++;
		}

		if (offset + item_length >= length) {
			return false;
		}

		uint32_t compatible_length = 0;

		while (compatible[compatible_length] != '\0') {
			compatible_length++;
		}

		if (item_length == compatible_length) {
			bool matches = true;

			for (uint32_t index = 0; index < item_length; index++) {
				if (bytes[offset + index] != (uint8_t)compatible[index]) {
					matches = false;
					break;
				}
			}

			if (matches) {
				return true;
			}
		}

		offset += item_length + 1U;
	}

	return false;
}

static bool
platform_read_address(const uint8_t *value, uint32_t cell_count, uint32_t *space, uint64_t *address)
{
	if (value == 0 || space == 0 || address == 0 || cell_count == 0U || cell_count > 3U) return false;
	*space = 0U;
	if (cell_count == 3U) {
		/* PCI's first address cell carries space and BDF flags; the next two are the address. */
		*space = platform_read_be32(value) & 0x03000000U;
		return platform_read_cells(value + 4U, 2U, address);
	}
	return platform_read_cells(value, cell_count, address);
}

static bool
platform_translate_region(uint32_t child_address_cells, uint64_t *address, uint32_t *space, uint64_t size,
						  const void *ranges, uint32_t ranges_length, bool ranges_present,
						  uint32_t parent_address_cells, uint32_t size_cells)
{
	if (!ranges_present) return true;
	if (child_address_cells == 0U || child_address_cells > 3U || parent_address_cells == 0U ||
		parent_address_cells > 3U || size_cells > 2U) return false;
	if (ranges_length == 0U) return true;
	if (ranges == 0) return false;

	uint32_t entry_cells = child_address_cells + parent_address_cells + size_cells;
	if (entry_cells == 0U || entry_cells > UINT32_MAX / 4U) return false;
	uint32_t entry_size = entry_cells * 4U;
	if (ranges_length % entry_size != 0U) return false;

	const uint8_t *bytes = ranges;
	for (uint32_t offset = 0U; offset < ranges_length; offset += entry_size) {
		uint32_t child_space;
		uint64_t child_base;
		uint32_t parent_space;
		uint64_t parent_base;
		uint64_t window_size;
		const uint8_t *entry = bytes + offset;
		if (!platform_read_address(entry, child_address_cells, &child_space, &child_base) ||
			!platform_read_address(entry + child_address_cells * 4U, parent_address_cells, &parent_space, &parent_base) ||
			!platform_read_cells(entry + (child_address_cells + parent_address_cells) * 4U, size_cells,
							 &window_size)) return false;

		if (child_space != *space || *address < child_base) continue;
		uint64_t displacement = *address - child_base;
		if (displacement > window_size || size > window_size - displacement || parent_base > UINT64_MAX - displacement)
			continue;

		*address = parent_base + displacement;
		*space = parent_space;
		return true;
	}
	return false;
}

static bool
platform_translate_internal(const platform_discovery_context_t *discovery, const platform_node_t *node,
							uint64_t *address, uint32_t *space, uint64_t size)
{
	if (discovery == 0 || node == 0 || address == 0 || space == 0) return false;
	uint32_t bus_depth = node->depth;
	while (bus_depth != 0U) {
		uint32_t bus_index = --bus_depth;
		if (bus_index == 0U) break;
		const platform_node_t *bus = &discovery->nodes[bus_index];
		if (!platform_translate_region(bus->child_address_cells, address, space, size, bus->ranges,
									   bus->ranges_length, bus->ranges_present, bus->register_address_cells,
									   bus->child_size_cells)) return false;
	}
	return true;
}

static bool
platform_read_reg(const platform_discovery_context_t *discovery, const platform_node_t *node, uint32_t entry_index,
			  platform_region_t *region)
{
	if (discovery == 0 || node == 0 || region == 0 || node->reg == 0 || node->register_address_cells == 0U ||
		node->register_address_cells > 3U || node->register_size_cells > 2U) return false;

	uint32_t entry_cells = node->register_address_cells + node->register_size_cells;

	if (entry_cells == 0U) {
		return false;
	}

	uint32_t entry_size = entry_cells * 4U;

	if (entry_index > 0xFFFFFFFFU / entry_size) {
		return false;
	}

	uint32_t offset = entry_index * entry_size;

	if (offset > node->reg_length || entry_size > node->reg_length - offset) {
		return false;
	}

	const uint8_t *entry = (const uint8_t *)node->reg + offset;

	uint32_t space;
	uint64_t base;
	if (!platform_read_address(entry, node->register_address_cells, &space, &base)) return false;

	uint64_t size;

	if (!platform_read_cells(entry + node->register_address_cells * 4U, node->register_size_cells, &size)) {
		return false;
	}

	if (!platform_translate_internal(discovery, node, &base, &space, size)) return false;
	region->base = base;
	region->size = size;

	return true;
}

static void
platform_begin_node(const char *name, uint32_t depth, void *context)
{
	platform_discovery_context_t *discovery = context;

	if (discovery == 0 || depth >= PLATFORM_MAX_DTB_DEPTH) {
		if (discovery != 0) {
			discovery->failed = true;
		}

		return;
	}

	platform_node_t *node = &discovery->nodes[depth];
	platform_t *platform = discovery->platform;

	if (platform->device_node_count >= PLATFORM_MAX_DEVICE_NODES) {
		discovery->failed = true;
		return;
	}

	uint32_t node_index = platform->device_node_count++;
	platform_device_node_t *device_node = &platform->device_nodes[node_index];
	memset(device_node, 0, sizeof(*device_node));
	device_node->name = name;
	device_node->parent_index = depth == 0U ? UINT32_MAX : discovery->nodes[depth - 1U].device_node_index;
	device_node->depth = depth;

	memset(node, 0, sizeof(*node));
	node->name = name;
	node->depth = depth;
	node->device_node_index = node_index;
	/* Device Tree defaults when the parent omits these properties. */
	node->child_address_cells = 2U;
	node->child_size_cells = 1U;

	if (depth != 0U) {
		const platform_node_t *parent = &discovery->nodes[depth - 1U];

		node->register_address_cells = parent->child_address_cells;

		node->register_size_cells = parent->child_size_cells;
		node->interrupt_parent_phandle = parent->interrupt_parent_phandle;
		node->dma_coherent = parent->dma_coherent;
	}
	device_node->reg_address_cells = node->register_address_cells;
	device_node->reg_size_cells = node->register_size_cells;
}

static void
platform_property(const char *name, const void *value, uint32_t length, uint32_t depth, void *context)
{
	platform_discovery_context_t *discovery = context;

	if (discovery == 0 || discovery->failed || depth >= PLATFORM_MAX_DTB_DEPTH) {
		return;
	}

	platform_node_t *node = &discovery->nodes[depth];

	if (platform_string_equals(name, "#address-cells") && length == 4U) {
		node->child_address_cells = platform_read_be32(value);

		return;
	}

	if (platform_string_equals(name, "#size-cells") && length == 4U) {
		node->child_size_cells = platform_read_be32(value);

		return;
	}

	if (platform_string_equals(name, "reg")) {
		node->reg = value;
		node->reg_length = length;
		return;
	}

	if (platform_string_equals(name, "ranges")) {
		node->ranges = value;
		node->ranges_length = length;
		node->ranges_present = true;
		return;
	}

	if (platform_string_equals(name, "dma-coherent")) {
		node->dma_coherent = true;
		return;
	}

	if (platform_string_equals(name, "dma-noncoherent")) {
		node->dma_coherent = false;
		return;
	}

	if ((platform_string_equals(name, "phandle") || platform_string_equals(name, "linux,phandle")) && length == 4U) {
		node->phandle = platform_read_be32(value);
		return;
	}

	if (platform_string_equals(name, "interrupt-parent") && length == 4U) {
		node->interrupt_parent_phandle = platform_read_be32(value);
		return;
	}

	if (platform_string_equals(name, "#interrupt-cells") && length == 4U) {
		node->interrupt_cells = platform_read_be32(value);
		return;
	}

	if (platform_string_equals(name, "compatible")) {
		node->compatible = value;
		node->compatible_length = length;
	} else if (platform_string_equals(name, "status")) {
		if (length != 0U && platform_value_has_nul(value, length)) {
			node->status = value;
		}
		if (platform_value_string_equals(value, length, "disabled") ||
			platform_value_string_equals(value, length, "fail")) {
			node->cpu_disabled = true;
		}
		return;
	} else if (platform_string_equals(name, "bus-range")) {
		node->bus_range = value;
		node->bus_range_length = length;
		return;
	} else if (platform_string_equals(name, "interrupts")) {
		node->interrupts = value;
		node->interrupts_length = length;
		return;
	} else if (platform_string_equals(name, "device_type")) {
		if (platform_value_string_equals(value, length, "memory")) {
			node->is_memory = true;
		}
		return;
	} else if (platform_string_equals(name, "method")) {
		if (platform_value_string_equals(value, length, "hvc")) {
			node->psci_method_hvc = true;
		} else if (platform_value_string_equals(value, length, "smc")) {
			node->psci_method_smc = true;
		}
		return;
	}

	/* Compatible strings classify devices and also remain visible in the tree. */
	if (!platform_string_equals(name, "compatible")) {
		return;
	}

	if (platform_compatible_contains(value, length, "arm,psci") ||
		platform_compatible_contains(value, length, "arm,psci-0.2") ||
		platform_compatible_contains(value, length, "arm,psci-1.0")) {
		node->is_psci = true;
	}

	if (platform_compatible_contains(value, length, "arm,pl011")) {
		node->is_uart = true;
	}

	if (platform_compatible_contains(value, length, "qemu,fw-cfg-mmio")) {
		node->is_fw_cfg = true;
	}

	if (platform_compatible_contains(value, length, "arm,gic-v3")) {
		node->is_gic = true;
	}

	if (platform_compatible_contains(value, length, "arm,pl031")) {
		node->is_rtc = true;
	}

	if (platform_compatible_contains(value, length, "pci-host-ecam-generic")) {
		node->is_pcie = true;
	}

	if (platform_compatible_contains(value, length, "virtio,mmio")) {
		node->is_virtio_mmio = true;
	}
}

/*
 * platform_read_gic_interrupt
 *
 * Decode the three-cell GIC interrupt specifier used by QEMU's AArch64
 * `virt` Device Tree. Cell zero is the interrupt type, cell one is the
 * type-relative number and cell two contains trigger flags.
 *
 * This is intentionally platform discovery code rather than a VirtIO
 * assumption: drivers receive a normal architectural INTID and never need
 * to know how Device Tree numbers GIC SPIs and PPIs.
 */
static bool
platform_resolve_interrupt_parents(platform_t *platform)
{
	if (platform == 0) return false;
	for (uint32_t index = 0U; index < platform->device_node_count; index++) {
		platform_device_node_t *node = &platform->device_nodes[index];
		if (node->interrupt_parent_phandle == 0U) continue;
		uint32_t match = UINT32_MAX;
		for (uint32_t candidate = 0U; candidate < platform->device_node_count; candidate++) {
			if (platform->device_nodes[candidate].phandle != node->interrupt_parent_phandle) continue;
			if (match != UINT32_MAX) {
				match = UINT32_MAX;
				break;
			}
			match = candidate;
		}
		if (match != UINT32_MAX) {
			node->interrupt_parent_index = match;
			node->interrupt_parent_resolved = true;
			node->interrupt_parent_cells = platform->device_nodes[match].interrupt_cells;
		}
	}
	return true;
}

static bool
platform_read_gic_interrupt(const platform_t *platform, uint32_t node_index, uint32_t *intid, uint32_t *flags)
{
	if (platform == 0 || node_index >= platform->device_node_count || intid == 0 || flags == 0) return false;
	const platform_device_node_t *node = &platform->device_nodes[node_index];
	if (node->interrupts == 0 || node->interrupts_length < 12U || node->interrupt_parent_cells != 3U ||
		!node->interrupt_parent_resolved || node->interrupt_parent_index >= platform->device_node_count) return false;
	const platform_device_node_t *controller = &platform->device_nodes[node->interrupt_parent_index];
	if (!platform_compatible_contains(controller->compatible, controller->compatible_length, "arm,gic-v3")) return false;

	const uint8_t *cells = node->interrupts;
	uint32_t type = platform_read_be32(cells);
	uint32_t number = platform_read_be32(cells + 4U);

	if (type == 0U) {
		if (number > 987U) return false;
		*intid = 32U + number;
	} else if (type == 1U) {
		if (number > 15U) return false;
		*intid = 16U + number;
	} else {
		return false;
	}

	*flags = platform_read_be32(cells + 8U);
	return true;
}

static void
platform_collect_virtio(const platform_t *platform, platform_t *mutable_platform)
{
	for (uint32_t index = 0U; index < platform->device_node_count; index++) {
		const platform_device_node_t *node = &platform->device_nodes[index];
		if (!platform_compatible_contains(node->compatible, node->compatible_length, "virtio,mmio") ||
			platform_value_string_equals(node->status, node->status != 0 ? (uint32_t)strlen(node->status) + 1U : 0U,
										 "disabled") ||
			platform_value_string_equals(node->status, node->status != 0 ? (uint32_t)strlen(node->status) + 1U : 0U,
										 "fail")) continue;
		if (mutable_platform->virtio_mmio_count >= PLATFORM_MAX_VIRTIO_MMIO_DEVICES) break;
		platform_region_t region;
		uint32_t intid;
		uint32_t flags;
		if (!platform_device_reg_get(platform, index, 0U, &region) ||
			!platform_read_gic_interrupt(platform, index, &intid, &flags)) continue;
		uint32_t slot = mutable_platform->virtio_mmio_count++;
		mutable_platform->virtio_mmio[slot] = region;
		mutable_platform->virtio_mmio_intid[slot] = intid;
		mutable_platform->virtio_mmio_irq_flags[slot] = flags;
		mutable_platform->virtio_mmio_dma_coherent[slot] = node->dma_coherent;
	}
}

static void
platform_end_node(uint32_t depth, void *context)
{
	platform_discovery_context_t *discovery = context;

	if (discovery == 0 || discovery->failed || depth >= PLATFORM_MAX_DTB_DEPTH) {
		return;
	}

	platform_node_t *node = &discovery->nodes[depth];

	platform_t *platform = discovery->platform;
	platform_device_node_t *device_node = &platform->device_nodes[node->device_node_index];
	device_node->reg_data = node->reg;
	device_node->reg_length = node->reg_length;
	device_node->interrupts = node->interrupts;
	device_node->interrupts_length = node->interrupts_length;
	device_node->ranges = node->ranges;
	device_node->ranges_length = node->ranges_length;
	device_node->ranges_present = node->ranges_present;
	device_node->ranges_child_address_cells = node->child_address_cells;
	device_node->ranges_parent_address_cells = node->register_address_cells;
	device_node->ranges_size_cells = node->child_size_cells;
	device_node->phandle = node->phandle;
	device_node->interrupt_parent_phandle = node->interrupt_parent_phandle;
	device_node->interrupt_parent_index = UINT32_MAX;
	device_node->interrupt_cells = node->interrupt_cells;
	device_node->dma_coherent = node->dma_coherent;
	if (node->compatible_length != 0U && platform_value_has_nul(node->compatible, node->compatible_length)) {
		device_node->compatible = node->compatible;
		device_node->compatible_length = node->compatible_length;
	}
	device_node->status = node->status;
	device_node->has_reg = platform_read_reg(discovery, node, 0U, &device_node->reg);

	/*
	 * /cpus/cpu@N: `reg` is the CPU's MPIDR affinity (its parent has
	 * #address-cells = 1, #size-cells = 0). Only the affinity fields are
	 * kept: the other MPIDR bits (MT, U, RES1) are not part of the identity
	 * PSCI and the GIC match on.
	 */
	if (platform_string_starts_with(node->name, "cpu@") && !node->cpu_disabled) {
		platform_region_t affinity;

		if (platform_read_reg(discovery, node, 0U, &affinity) && platform->cpu_count < PLATFORM_MAX_CPUS) {
			platform->cpu_mpidr[platform->cpu_count++] = affinity.base & 0xFF00FFFFFFULL;
		}

		return;
	}

	if (node->is_psci) {
		platform->psci_method = node->psci_method_hvc	? PLATFORM_PSCI_HVC
								: node->psci_method_smc ? PLATFORM_PSCI_SMC
														: PLATFORM_PSCI_NONE;

		return;
	}

	if (node->is_memory || platform_string_starts_with(node->name, "memory@")) {
		if (platform->memory_region_count < PLATFORM_MAX_MEMORY_REGIONS) {
			platform_region_t *region = &platform->memory_regions[platform->memory_region_count];

			if (platform_read_reg(discovery, node, 0U, region)) {
				platform->memory_region_count++;
			}
		}

		return;
	}

	if (node->is_uart) {
		/* The first PL011 is the console; a second one is the tepOS mailbox link. */
		(void)platform_read_reg(discovery, node, 0U,
							platform->uart.size == 0U ? &platform->uart : &platform->mailbox_uart);

		return;
	}

	if (node->is_fw_cfg) {
		(void)platform_read_reg(discovery, node, 0U, &platform->fw_cfg);

		return;
	}

	if (node->is_rtc) {
		(void)platform_read_reg(discovery, node, 0U, &platform->rtc);

		return;
	}

	if (node->is_gic) {
		(void)platform_read_reg(discovery, node, 0U, &platform->gic_distributor);

		(void)platform_read_reg(discovery, node, 1U, &platform->gic_redistributor);

		return;
	}

	if (node->is_pcie) {
		(void)platform_read_reg(discovery, node, 0U, &platform->pcie_ecam);

		if (node->bus_range != 0 && node->bus_range_length >= 8U) {
			const uint8_t *range = node->bus_range;

			platform->pcie_bus_start = platform_read_be32(range);

			platform->pcie_bus_end = platform_read_be32(range + 4U);
		}

		return;
	}

}

typedef struct {
	uint32_t next_node_index;
	uint32_t current_node_index;
	uint32_t target_node_index;
	const char *target_property;
	const void *value;
	uint32_t length;
	bool found;
} platform_property_query_t;

static void
platform_property_query_begin(const char *name, uint32_t depth, void *context)
{
	(void)name;
	(void)depth;
	platform_property_query_t *query = context;
	query->current_node_index = query->next_node_index++;
}

static void
platform_property_query_property(const char *name, const void *value, uint32_t length, uint32_t depth, void *context)
{
	(void)depth;
	platform_property_query_t *query = context;
	if (query->found || query->current_node_index != query->target_node_index ||
		!platform_string_equals(name, query->target_property)) return;
	query->value = value;
	query->length = length;
	query->found = true;
}

bool
platform_device_property_get(const platform_t *platform, uint32_t node_index, const char *property_name,
							const void **value, uint32_t *length)
{
	if (platform == 0 || platform->source_dtb == 0 || node_index >= platform->device_node_count ||
		property_name == 0 || value == 0 || length == 0) return false;
	platform_property_query_t query = {.target_node_index = node_index, .target_property = property_name};
	dtb_visitor_t visitor = {.begin_node = platform_property_query_begin,
							 .property = platform_property_query_property,
							 .end_node = 0};
	if (!dtb_walk(platform->source_dtb, &visitor, &query) || !query.found) return false;
	*value = query.value;
	*length = query.length;
	return true;
}

bool
platform_device_reg_get(const platform_t *platform, uint32_t node_index, uint32_t entry_index,
						platform_region_t *region)
{
	if (platform == 0 || node_index >= platform->device_node_count || region == 0) return false;
	const platform_device_node_t *node = &platform->device_nodes[node_index];
	if (node->reg_data == 0 || node->reg_address_cells == 0U || node->reg_address_cells > 3U ||
		node->reg_size_cells > 2U) return false;

	uint32_t cells = node->reg_address_cells + node->reg_size_cells;
	uint32_t entry_size = cells * 4U;
	if (entry_size == 0U || entry_index > UINT32_MAX / entry_size) return false;
	uint32_t offset = entry_index * entry_size;
	if (offset > node->reg_length || entry_size > node->reg_length - offset) return false;

	const uint8_t *entry = (const uint8_t *)node->reg_data + offset;
	uint32_t space;
	if (!platform_read_address(entry, node->reg_address_cells, &space, &region->base) ||
		!platform_read_cells(entry + node->reg_address_cells * 4U, node->reg_size_cells, &region->size)) return false;

	uint32_t parent_index = node->parent_index;
	while (parent_index != UINT32_MAX) {
		if (parent_index >= platform->device_node_count) return false;
		if (parent_index == 0U) break;
		const platform_device_node_t *bus = &platform->device_nodes[parent_index];
		if (!platform_translate_region(bus->ranges_child_address_cells, &region->base, &space, region->size,
										   bus->ranges, bus->ranges_length, bus->ranges_present,
										   bus->ranges_parent_address_cells, bus->ranges_size_cells)) return false;
		parent_index = bus->parent_index;
	}
	return true;
}

bool
platform_discover(const dtb_t *dtb, platform_t *platform)
{
	if (dtb == 0 || platform == 0) {
		return false;
	}

	*platform = (platform_t){0};
	platform->source_dtb = dtb;
	platform->device_nodes = g_device_nodes;

	platform_discovery_context_t context = {.platform = platform, .failed = false};

	/*
	 * Hardware discovery also runs before the permanent TTBR1 transition.
	 * Build callback pointers at runtime for the same reason as dtb_dump().
	 */
	dtb_visitor_t visitor;
	visitor.begin_node = platform_begin_node;
	visitor.property = platform_property;
	visitor.end_node = platform_end_node;

	if (!dtb_walk(dtb, &visitor, &context)) {
		return false;
	}

	if (context.failed) {
		return false;
	}

	if (!platform_resolve_interrupt_parents(platform)) return false;
	platform_collect_virtio(platform, platform);

	return platform->memory_region_count != 0U && platform->uart.size != 0U && platform->gic_distributor.size != 0U &&
		   platform->gic_redistributor.size != 0U;
}

bool
platform_bootstrap(const dtb_t *dtb)
{
	if (g_platform_ready || dtb == 0) {
		return false;
	}

	memset(&g_platform, 0, sizeof(g_platform));

	if (!platform_discover(dtb, &g_platform)) {
		memset(&g_platform, 0, sizeof(g_platform));
		return false;
	}

	g_platform_ready = true;
	return true;
}

const platform_t *
platform_get(void)
{
	return g_platform_ready ? &g_platform : 0;
}

static void
platform_dump_region(const char *name, const platform_region_t *region)
{
	kprintf("platform: %s base: %p\n", name, (void *)region->base);
	kverbosef("platform: %s size: %llu bytes\n", name, (unsigned long long)region->size);
}

void
platform_dump(const platform_t *platform)
{
	if (platform == 0) {
		return;
	}

	kprintf("platform: device tree nodes: %u\n", platform->device_node_count);
	for (uint32_t index = 0; index < platform->device_node_count; index++) {
		const platform_device_node_t *node = &platform->device_nodes[index];
		for (uint32_t level = 0; level < node->depth; level++) {
			kprintf("  ");
		}
		kprintf("+- %s", node->name != 0 && node->name[0] != '\0' ? node->name : "/");
		if (node->compatible != 0) {
			kprintf(" [");
			kprintf("%s", node->compatible);
			kprintf("]");
		}
		if (node->has_reg) {
			kprintf(" @ %p", (void *)node->reg.base);
			for (uint32_t entry = 1U;; entry++) {
				platform_region_t range;
				if (!platform_device_reg_get(platform, index, entry, &range)) break;
				kprintf(", %p+%llu", (void *)range.base, (unsigned long long)range.size);
			}
		}
		if (node->ranges_present) kprintf(" [ranges: %u bytes]", node->ranges_length);
		if (node->interrupt_parent_resolved && node->interrupt_parent_index < platform->device_node_count) {
			const platform_device_node_t *controller = &platform->device_nodes[node->interrupt_parent_index];
			kprintf(" [interrupt-parent: %s, %u cells]", controller->name, node->interrupt_parent_cells);
		} else if (node->interrupt_parent_phandle != 0U) {
			kprintf(" [interrupt-parent phandle 0x%x unresolved]", node->interrupt_parent_phandle);
		}
		if (node->interrupts != 0) kprintf(" [interrupt specifier: %u bytes]", node->interrupts_length);
		kprintf(" (%s)\n", node->status != 0 ? node->status : "okay");
	}

	kprintf("platform: memory regions: %u\n", platform->memory_region_count);

	for (uint32_t index = 0; index < platform->memory_region_count; index++) {
		kprintf("platform: RAM[%u] base: %p\n", index, (void *)platform->memory_regions[index].base);
		kverbosef("platform: RAM[%u] size: %llu bytes\n", index,
				  (unsigned long long)platform->memory_regions[index].size);
	}

	platform_dump_region("UART", &platform->uart);
	if (platform->mailbox_uart.size != 0ULL) platform_dump_region("TEP mailbox UART", &platform->mailbox_uart);

	if (platform->fw_cfg.size != 0ULL) {
		platform_dump_region("fw_cfg", &platform->fw_cfg);
	}

	platform_dump_region("GIC distributor", &platform->gic_distributor);

	platform_dump_region("GIC redistributor", &platform->gic_redistributor);

	if (platform->rtc.size != 0ULL) {
		platform_dump_region("RTC", &platform->rtc);
	}

	if (platform->pcie_ecam.size != 0U) {
		platform_dump_region("PCI ECAM", &platform->pcie_ecam);

		kprintf("platform: PCI bus range: %u-%u\n", (unsigned)platform->pcie_bus_start,
				(unsigned)platform->pcie_bus_end);
	}

	kprintf("platform: VirtIO MMIO transports: %u\n", platform->virtio_mmio_count);

	/* One line per transport slot: a table for `-v`, noise otherwise. */
	for (uint32_t index = 0; index < platform->virtio_mmio_count; index++) {
		kverbosef("platform: VirtIO[%u] base: %p, INTID: %u\n", index, (void *)platform->virtio_mmio[index].base,
				  (unsigned)platform->virtio_mmio_intid[index]);
	}
}
