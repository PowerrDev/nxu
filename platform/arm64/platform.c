#include <kern/console/console.h>
#include <platform/platform.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PLATFORM_MAX_DTB_DEPTH 64U

static platform_t g_platform;
static bool g_platform_ready;

typedef struct {
	const char *name;

	uint32_t register_address_cells;
	uint32_t register_size_cells;

	uint32_t child_address_cells;
	uint32_t child_size_cells;

	const void *reg;
	uint32_t reg_length;

	const void *bus_range;
	uint32_t bus_range_length;

	const void *interrupts;
	uint32_t interrupts_length;

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

static bool platform_string_equals(
	const char *left,
	const char *right
)
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

static bool platform_string_starts_with(
	const char *string,
	const char *prefix
)
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

static uint32_t platform_read_be32(const void *address)
{
	const uint8_t *bytes = address;

	return ((uint32_t)bytes[0] << 24U)
		| ((uint32_t)bytes[1] << 16U)
		| ((uint32_t)bytes[2] << 8U)
		| (uint32_t)bytes[3];
}

static bool platform_read_cells(
	const uint8_t *value,
	uint32_t cell_count,
	uint64_t *result
)
{
	if (value == 0 || result == 0 || cell_count > 2U) {
		return false;
	}

	uint64_t number = 0;

	for (uint32_t index = 0; index < cell_count; index++) {
		number <<= 32U;
		number |= platform_read_be32(
			value + index * 4U
		);
	}

	*result = number;

	return true;
}

static bool platform_value_string_equals(
	const void *value,
	uint32_t length,
	const char *expected
)
{
	if (value == 0 || expected == 0 || length == 0U) {
		return false;
	}

	const uint8_t *bytes = value;
	uint32_t index = 0;

	while (expected[index] != '\0') {
		if (
			index >= length ||
			bytes[index] != (uint8_t)expected[index]
		) {
			return false;
		}

		index++;
	}

	return index < length && bytes[index] == '\0';
}

static bool platform_compatible_contains(
	const void *value,
	uint32_t length,
	const char *compatible
)
{
	if (value == 0 || compatible == 0) {
		return false;
	}

	const uint8_t *bytes = value;
	uint32_t offset = 0;

	while (offset < length) {
		uint32_t item_length = 0;

		while (
			offset + item_length < length &&
			bytes[offset + item_length] != '\0'
		) {
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

			for (
				uint32_t index = 0;
				index < item_length;
				index++
			) {
				if (
					bytes[offset + index] !=
					(uint8_t)compatible[index]
				) {
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

static bool platform_read_reg(
	const platform_node_t *node,
	uint32_t entry_index,
	platform_region_t *region
)
{
	if (
		node == 0 ||
		region == 0 ||
		node->reg == 0 ||
		node->register_address_cells == 0U ||
		node->register_address_cells > 2U ||
		node->register_size_cells > 2U
	) {
		return false;
	}

	uint32_t entry_cells =
		node->register_address_cells +
		node->register_size_cells;

	if (entry_cells == 0U) {
		return false;
	}

	uint32_t entry_size = entry_cells * 4U;

	if (
		entry_index >
		0xFFFFFFFFU / entry_size
	) {
		return false;
	}

	uint32_t offset = entry_index * entry_size;

	if (
		offset > node->reg_length ||
		entry_size > node->reg_length - offset
	) {
		return false;
	}

	const uint8_t *entry = (const uint8_t *)node->reg + offset;

	uint64_t base;

	if (!platform_read_cells(
		entry,
		node->register_address_cells,
		&base
	)) {
		return false;
	}

	uint64_t size;

	if (!platform_read_cells(
		entry + node->register_address_cells * 4U,
		node->register_size_cells,
		&size
	)) {
		return false;
	}

	region->base = base;
	region->size = size;

	return true;
}

static void platform_begin_node(
	const char *name,
	uint32_t depth,
	void *context
)
{
	platform_discovery_context_t *discovery = context;

	if (
		discovery == 0 ||
		depth >= PLATFORM_MAX_DTB_DEPTH
	) {
		if (discovery != 0) {
			discovery->failed = true;
		}

		return;
	}

	platform_node_t *node = &discovery->nodes[depth];

	*node = (platform_node_t){
		.name = name,

		/*
		 * Device Tree defaults for a node that does not provide
		 * these properties.
		 */
		.child_address_cells = 2U,
		.child_size_cells = 1U
	};

	if (depth != 0U) {
		const platform_node_t *parent = &discovery->nodes[depth - 1U];

		node->register_address_cells = parent->child_address_cells;

		node->register_size_cells = parent->child_size_cells;
	}
}

static void platform_property(
	const char *name,
	const void *value,
	uint32_t length,
	uint32_t depth,
	void *context
)
{
	platform_discovery_context_t *discovery = context;

	if (
		discovery == 0 ||
		discovery->failed ||
		depth >= PLATFORM_MAX_DTB_DEPTH
	) {
		return;
	}

	platform_node_t *node = &discovery->nodes[depth];

	if (
		platform_string_equals(
			name,
			"#address-cells"
		) &&
		length == 4U
	) {
		node->child_address_cells = platform_read_be32(value);

		return;
	}

	if (
		platform_string_equals(
			name,
			"#size-cells"
		) &&
		length == 4U
	) {
		node->child_size_cells = platform_read_be32(value);

		return;
	}

	if (platform_string_equals(name, "reg")) {
		node->reg = value;
		node->reg_length = length;
		return;
	}

	if (platform_string_equals(name, "bus-range")) {
		node->bus_range = value;
		node->bus_range_length = length;
		return;
	}

	if (platform_string_equals(name, "interrupts")) {
		node->interrupts = value;
		node->interrupts_length = length;
		return;
	}

	if (platform_string_equals(name, "device_type")) {
		if (platform_value_string_equals(
			value,
			length,
			"memory"
		)) {
			node->is_memory = true;
		}

		return;
	}

	/* PSCI's conduit: which instruction a CPU_ON call is made with. */
	if (platform_string_equals(name, "method")) {
		if (platform_value_string_equals(value, length, "hvc")) {
			node->psci_method_hvc = true;
		} else if (platform_value_string_equals(value, length, "smc")) {
			node->psci_method_smc = true;
		}

		return;
	}

	/* A CPU the firmware lists but says must not be used. */
	if (platform_string_equals(name, "status")) {
		if (
			platform_value_string_equals(value, length, "disabled") ||
			platform_value_string_equals(value, length, "fail")
		) {
			node->cpu_disabled = true;
		}

		return;
	}

	if (!platform_string_equals(name, "compatible")) {
		return;
	}

	if (platform_compatible_contains(
		value,
		length,
		"arm,psci"
	) || platform_compatible_contains(
		value,
		length,
		"arm,psci-0.2"
	) || platform_compatible_contains(
		value,
		length,
		"arm,psci-1.0"
	)) {
		node->is_psci = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"arm,pl011"
	)) {
		node->is_uart = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"qemu,fw-cfg-mmio"
	)) {
		node->is_fw_cfg = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"arm,gic-v3"
	)) {
		node->is_gic = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"arm,pl031"
	)) {
		node->is_rtc = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"pci-host-ecam-generic"
	)) {
		node->is_pcie = true;
	}

	if (platform_compatible_contains(
		value,
		length,
		"virtio,mmio"
	)) {
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
static bool platform_read_gic_interrupt(
	const platform_node_t *node,
	uint32_t *intid,
	uint32_t *flags
)
{
	if (
		node == 0 ||
		intid == 0 ||
		flags == 0 ||
		node->interrupts == 0 ||
		node->interrupts_length < 12U
	) {
		return false;
	}

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

static void platform_end_node(
	uint32_t depth,
	void *context
)
{
	platform_discovery_context_t *discovery = context;

	if (
		discovery == 0 ||
		discovery->failed ||
		depth >= PLATFORM_MAX_DTB_DEPTH
	) {
		return;
	}

	platform_node_t *node = &discovery->nodes[depth];

	platform_t *platform = discovery->platform;

	/*
	 * /cpus/cpu@N: `reg` is the CPU's MPIDR affinity (its parent has
	 * #address-cells = 1, #size-cells = 0). Only the affinity fields are
	 * kept: the other MPIDR bits (MT, U, RES1) are not part of the identity
	 * PSCI and the GIC match on.
	 */
	if (
		platform_string_starts_with(node->name, "cpu@") &&
		!node->cpu_disabled
	) {
		platform_region_t affinity;

		if (
			platform_read_reg(node, 0U, &affinity) &&
			platform->cpu_count < PLATFORM_MAX_CPUS
		) {
			platform->cpu_mpidr[platform->cpu_count++] =
				affinity.base & 0xFF00FFFFFFULL;
		}

		return;
	}

	if (node->is_psci) {
		platform->psci_method = node->psci_method_hvc
			? PLATFORM_PSCI_HVC
			: node->psci_method_smc
				? PLATFORM_PSCI_SMC
				: PLATFORM_PSCI_NONE;

		return;
	}

	if (
		node->is_memory ||
		platform_string_starts_with(
			node->name,
			"memory@"
		)
	) {
		if (
			platform->memory_region_count <
			PLATFORM_MAX_MEMORY_REGIONS
		) {
			platform_region_t *region =
				&platform->memory_regions[
					platform->memory_region_count
				];

			if (platform_read_reg(node, 0U, region)) {
				platform->memory_region_count++;
			}
		}

		return;
	}

	if (node->is_uart) {
		(void)platform_read_reg(
			node,
			0U,
			&platform->uart
		);

		return;
	}

	if (node->is_fw_cfg) {
		(void)platform_read_reg(
			node,
			0U,
			&platform->fw_cfg
		);

		return;
	}

	if (node->is_rtc) {
		(void)platform_read_reg(
			node,
			0U,
			&platform->rtc
		);

		return;
	}

	if (node->is_gic) {
		(void)platform_read_reg(
			node,
			0U,
			&platform->gic_distributor
		);

		(void)platform_read_reg(
			node,
			1U,
			&platform->gic_redistributor
		);

		return;
	}

	if (node->is_pcie) {
		(void)platform_read_reg(
			node,
			0U,
			&platform->pcie_ecam
		);

		if (
			node->bus_range != 0 &&
			node->bus_range_length >= 8U
		) {
			const uint8_t *range = node->bus_range;

			platform->pcie_bus_start = platform_read_be32(range);

			platform->pcie_bus_end = platform_read_be32(range + 4U);
		}

		return;
	}

	if (
		node->is_virtio_mmio &&
		platform->virtio_mmio_count <
		PLATFORM_MAX_VIRTIO_MMIO_DEVICES
	) {
		uint32_t index = platform->virtio_mmio_count;
		platform_region_t *region = &platform->virtio_mmio[index];
		uint32_t intid;
		uint32_t flags;

		if (
			platform_read_reg(node, 0U, region) &&
			platform_read_gic_interrupt(node, &intid, &flags)
		) {
			platform->virtio_mmio_intid[index] = intid;
			platform->virtio_mmio_irq_flags[index] = flags;
			platform->virtio_mmio_count++;
		}
	}
}

bool platform_discover(
	const dtb_t *dtb,
	platform_t *platform
)
{
	if (dtb == 0 || platform == 0) {
		return false;
	}

	*platform = (platform_t){0};

	platform_discovery_context_t context = {
		.platform = platform,
		.failed = false
	};

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

	return platform->memory_region_count != 0U
		&& platform->uart.size != 0U
		&& platform->gic_distributor.size != 0U
		&& platform->gic_redistributor.size != 0U;
}

bool platform_bootstrap(const dtb_t *dtb)
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

const platform_t *platform_get(void)
{
	return g_platform_ready ? &g_platform : 0;
}

static void platform_dump_region(
	const char *name,
	const platform_region_t *region
)
{
	kprintf("platform: %s base: %p\n", name, (void *)region->base);
	kverbosef("platform: %s size: %llu bytes\n", name, (unsigned long long)region->size);
}

void platform_dump(const platform_t *platform)
{
	if (platform == 0) {
		return;
	}

	kprintf("platform: memory regions: %u\n", platform->memory_region_count);

	for (
		uint32_t index = 0;
		index < platform->memory_region_count;
		index++
	) {
		kprintf("platform: RAM[%u] base: %p\n", index, (void *)platform->memory_regions[index].base);
		kverbosef("platform: RAM[%u] size: %llu bytes\n", index, (unsigned long long)platform->memory_regions[index].size);
	}

	platform_dump_region("UART", &platform->uart);

	if (platform->fw_cfg.size != 0ULL) {
		platform_dump_region("fw_cfg", &platform->fw_cfg);
	}

	platform_dump_region(
		"GIC distributor",
		&platform->gic_distributor
	);

	platform_dump_region(
		"GIC redistributor",
		&platform->gic_redistributor
	);

	if (platform->rtc.size != 0ULL) {
		platform_dump_region("RTC", &platform->rtc);
	}

	if (platform->pcie_ecam.size != 0U) {
		platform_dump_region(
			"PCI ECAM",
			&platform->pcie_ecam
		);

		kprintf("platform: PCI bus range: %u-%u\n", (unsigned)platform->pcie_bus_start, (unsigned)platform->pcie_bus_end);
	}

	kprintf("platform: VirtIO MMIO transports: %u\n", platform->virtio_mmio_count);

	/* One line per transport slot: a table for `-v`, noise otherwise. */
	for (
		uint32_t index = 0;
		index < platform->virtio_mmio_count;
		index++
	) {
		kverbosef(
			"platform: VirtIO[%u] base: %p, INTID: %u\n",
			index,
			(void *)platform->virtio_mmio[index].base,
			(unsigned)platform->virtio_mmio_intid[index]
		);
	}
}
