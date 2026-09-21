#include <vm/vmm_internal.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Boot-time construction of the TTBR0 identity map and the system-register
 * configuration that enables EL1 stage-1 translation.
 *
 * Live mapping operations live in vmm_tables.c, the higher-half alias in
 * vmm_ttbr1.c and diagnostics in vmm_debug.c.
 */

vmm_state_t g_vmm;

static uint64_t arm64_read_mmfr0(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r"(value));

	return value;
}

uint64_t vmm_read_sctlr(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(value));

	return value;
}

uint64_t vmm_read_ttbr0(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TTBR0_EL1" : "=r"(value));

	return value;
}

uint64_t vmm_read_ttbr1(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TTBR1_EL1" : "=r"(value));

	return value;
}

uint64_t vmm_read_tcr(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TCR_EL1" : "=r"(value));

	return value;
}

uint64_t vmm_read_mair(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, MAIR_EL1" : "=r"(value));

	return value;
}

static uint64_t vmm_align_down(uint64_t value, uint64_t alignment)
{
	return value & ~(alignment - 1ULL);
}

static bool vmm_align_up(
	uint64_t value,
	uint64_t alignment,
	uint64_t *result
)
{
	uint64_t mask = alignment - 1ULL;

	if (result == 0 || value > UINT64_MAX - mask) {
		return false;
	}

	*result = (value + mask) & ~mask;

	return true;
}

static bool vmm_physical_bits(uint32_t encoding, uint32_t *bits)
{
	static const uint8_t widths[] = {
		32U,
		36U,
		40U,
		42U,
		44U,
		48U
	};

	if (bits == 0 || encoding >= sizeof(widths)) {
		return false;
	}

	*bits = widths[encoding];

	return true;
}

/*
 * Identity-map [base, base + size) with the largest blocks the alignment
 * allows: L1 blocks, then L2 blocks, then L3 pages.
 */
static bool vmm_map_range(
	uint64_t base,
	uint64_t size,
	vmm_memory_type_t type,
	vmm_protection_t protection
)
{
	if (size == 0ULL) {
		return true;
	}

	if (base > UINT64_MAX - size) {
		return false;
	}

	uint64_t start = vmm_align_down(base, VMM_L3_SIZE);

	uint64_t end;

	if (!vmm_align_up(base + size, VMM_L3_SIZE, &end)) {
		return false;
	}

	uint64_t virtual_limit = 1ULL << VMM_VA_BITS;
	uint64_t physical_limit = 1ULL << g_vmm.physical_bits;

	if (end > virtual_limit || end > physical_limit) {
		return false;
	}

	uint64_t attributes;

	if (!vmm_attributes(type, protection, &attributes)) {
		return false;
	}

	uint64_t address = start;

	while (address < end) {
		uint64_t remaining = end - address;

		if (
			(address & (VMM_L1_SIZE - 1ULL)) == 0ULL &&
			remaining >= VMM_L1_SIZE
		) {
			if (!vmm_map_initial_l1(address, attributes)) {
				return false;
			}

			address += VMM_L1_SIZE;
			continue;
		}

		if (
			(address & (VMM_L2_SIZE - 1ULL)) == 0ULL &&
			remaining >= VMM_L2_SIZE
		) {
			if (!vmm_map_initial_l2(address, attributes)) {
				return false;
			}

			address += VMM_L2_SIZE;
			continue;
		}

		if (!vmm_map_initial_l3(address, attributes)) {
			return false;
		}

		address += VMM_L3_SIZE;
	}

	return true;
}

static bool vmm_map_region(
	const platform_region_t *region,
	vmm_memory_type_t type,
	vmm_protection_t protection
)
{
	if (region == 0 || region->size == 0ULL) {
		return true;
	}

	return vmm_map_range(region->base, region->size, type, protection);
}

static bool vmm_map_span(
	uint64_t start,
	uint64_t end,
	vmm_memory_type_t type,
	vmm_protection_t protection
)
{
	if (end < start) {
		return false;
	}

	if (end == start) {
		return true;
	}

	return vmm_map_range(start, end - start, type, protection);
}

static bool vmm_is_page_aligned(uint64_t address)
{
	return (address & (VMM_L3_SIZE - 1ULL)) == 0ULL;
}

static uint64_t vmm_kernel_symbol_physical(const void *symbol)
{
	uint64_t physical_address = 0ULL;

	if (!vmm_kernel_address_to_physical((uint64_t)symbol, &physical_address)) {
		return UINT64_MAX;
	}

	return physical_address;
}

/*
 * Map the RAM region containing the kernel, giving each kernel section
 * its final permissions.
 */
static bool vmm_map_kernel_memory(const platform_region_t *memory)
{
	if (
		memory == 0 ||
		memory->size == 0ULL ||
		memory->base > UINT64_MAX - memory->size
	) {
		return false;
	}

	uint64_t memory_start = memory->base;
	uint64_t memory_end = memory->base + memory->size;

	uint64_t kernel_start = vmm_kernel_symbol_physical(__kernel_start);
	uint64_t kernel_end = vmm_kernel_symbol_physical(__kernel_end);

	uint64_t text_start = vmm_kernel_symbol_physical(__text_start);
	uint64_t text_end = vmm_kernel_symbol_physical(__text_end);

	uint64_t rodata_start = vmm_kernel_symbol_physical(__rodata_start);
	uint64_t rodata_end = vmm_kernel_symbol_physical(__rodata_end);

	uint64_t data_start = vmm_kernel_symbol_physical(__data_start);
	uint64_t data_end = vmm_kernel_symbol_physical(__data_end);

	uint64_t bss_start = vmm_kernel_symbol_physical(__bss_start);
	uint64_t bss_end = vmm_kernel_symbol_physical(__bss_end);

	if (
		!vmm_is_page_aligned(kernel_start) ||
		!vmm_is_page_aligned(kernel_end) ||
		!vmm_is_page_aligned(text_start) ||
		!vmm_is_page_aligned(text_end) ||
		!vmm_is_page_aligned(rodata_start) ||
		!vmm_is_page_aligned(rodata_end) ||
		!vmm_is_page_aligned(data_start) ||
		!vmm_is_page_aligned(data_end) ||
		!vmm_is_page_aligned(bss_start) ||
		!vmm_is_page_aligned(bss_end)
	) {
		return false;
	}

	if (
		kernel_start != text_start ||
		text_end != rodata_start ||
		rodata_end != data_start ||
		data_end != bss_start ||
		bss_end != kernel_end
	) {
		return false;
	}

	if (kernel_start < memory_start || kernel_end > memory_end) {
		return false;
	}

	/*
	 * RAM below the OS (this includes QEMU's boot stub and currently
	 * unused pages)
	 */
	if (!vmm_map_span(
		memory_start,
		text_start,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_span(
		text_start,
		text_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_EXECUTE
	)) {
		return false;
	}

	if (!vmm_map_span(
		rodata_start,
		rodata_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_ONLY
	)) {
		return false;
	}

	if (!vmm_map_span(
		data_start,
		data_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_span(
		bss_start,
		bss_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	/*
	 * The PMM bitmap, translation tables, DTB and all remaining
	 * free RAM live above the kernel
	 */
	return vmm_map_span(
		kernel_end,
		memory_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	);
}

static void vmm_enable(void)
{
	uint64_t address_size = 64U - VMM_VA_BITS;

	uint64_t mair =
		VMM_MAIR_NORMAL_WB |
		(VMM_MAIR_DEVICE_NGNRE << (VMM_MAIR_DEVICE_INDEX * 8U));

	uint64_t tcr =
		VMM_TCR_T0SZ(address_size) |
		VMM_TCR_IRGN0_WBWA |
		VMM_TCR_ORGN0_WBWA |
		VMM_TCR_SH0_INNER |
		VMM_TCR_TG0_4K |

		VMM_TCR_T1SZ(address_size) |
		VMM_TCR_EPD1 |
		VMM_TCR_IRGN1_WBWA |
		VMM_TCR_ORGN1_WBWA |
		VMM_TCR_SH1_INNER |
		VMM_TCR_TG1_4K |

		VMM_TCR_IPS(g_vmm.ips);

	/*
	 * Only enable address translation for this lesson.
	 *
	 * SCTLR_EL1.C and SCTLR_EL1.I, which enable the data and
	 * instruction caches, remain unchanged.
	 */
	uint64_t sctlr = vmm_read_sctlr() | VMM_SCTLR_M | VMM_SCTLR_WXN;

	__asm__ volatile(
		"dsb ishst\n"

		"msr MAIR_EL1, %0\n"
		"msr TCR_EL1, %1\n"
		"msr TTBR0_EL1, %2\n"

		"isb\n"

		"tlbi vmalle1\n"
		"dsb ish\n"
		"isb\n"

		"msr SCTLR_EL1, %3\n"
		"isb\n"

		:
		: "r"(mair),
		  "r"(tcr),
		  "r"(g_vmm.root_physical),
		  "r"(sctlr)
		: "memory"
	);
}

bool vmm_map_identity(
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	/*
	 * Live page-table modification is not supported yet.
	 */
	if (g_vmm.root == 0 || g_vmm.enabled) {
		return false;
	}

	return vmm_map_range(
		physical_address,
		size,
		memory_type,
		protection
	);
}

bool vmm_init(const platform_t *platform)
{
	if (platform == 0 || platform->memory_region_count == 0U) {
		return false;
	}

	memset(&g_vmm, 0, sizeof(g_vmm));

	uint64_t mmfr0 = arm64_read_mmfr0();

	uint32_t tgran4 = (uint32_t)((mmfr0 >> 28U) & 0xFULL);

	if (tgran4 == 0xFULL) {
		return false;
	}

	g_vmm.ips = (uint32_t)(mmfr0 & 0xFULL);

	if (!vmm_physical_bits(g_vmm.ips, &g_vmm.physical_bits)) {
		return false;
	}

	if (!vmm_allocate_table(
		&g_vmm.root,
		&g_vmm.root_physical,
		&g_vmm.table_count
	)) {
		return false;
	}

	bool kernel_memory_mapped = false;

	uint64_t kernel_start = vmm_kernel_symbol_physical(__kernel_start);
	uint64_t kernel_end = vmm_kernel_symbol_physical(__kernel_end);

	for (
		uint32_t index = 0;
		index < platform->memory_region_count;
		index++
	) {
		const platform_region_t *memory = &platform->memory_regions[index];

		if (memory->base > UINT64_MAX - memory->size) {
			return false;
		}

		uint64_t memory_end = memory->base + memory->size;

		bool contains_kernel =
			kernel_start >= memory->base &&
			kernel_end <= memory_end;

		if (contains_kernel) {
			if (kernel_memory_mapped) {
				return false;
			}

			if (!vmm_map_kernel_memory(memory)) {
				return false;
			}

			kernel_memory_mapped = true;
			continue;
		}

		if (!vmm_map_region(
			memory,
			VMM_MEMORY_NORMAL,
			VMM_PROTECTION_READ_WRITE
		)) {
			return false;
		}
	}

	if (!kernel_memory_mapped) {
		return false;
	}

	if (!vmm_map_region(
		&platform->uart,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_region(
		&platform->gic_distributor,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_region(
		&platform->gic_redistributor,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_region(
		&platform->pcie_ecam,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	for (
		uint32_t index = 0;
		index < platform->virtio_mmio_count;
		index++
	) {
		if (!vmm_map_region(
			&platform->virtio_mmio[index],
			VMM_MEMORY_DEVICE,
			VMM_PROTECTION_READ_WRITE
		)) {
			return false;
		}
	}

	vmm_enable();

	g_vmm.enabled = true;

	return true;
}

bool vmm_is_enabled(void)
{
	return g_vmm.enabled;
}

bool vmm_rebase_kernel_pointers(void)
{
	if (
		!g_vmm.enabled ||
		!g_vmm.higher_half_enabled ||
		!g_vmm.higher_half_direct_map_enabled ||
		g_vmm.kernel_pointers_rebased ||
		g_vmm.root_physical == 0ULL ||
		g_vmm.ttbr1_root_physical == 0ULL
	) {
		return false;
	}

	uint64_t root_virtual;
	uint64_t ttbr1_root_virtual;

	if (!vmm_physical_to_higher_half(
		g_vmm.root_physical,
		&root_virtual
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		g_vmm.ttbr1_root_physical,
		&ttbr1_root_virtual
	)) {
		return false;
	}

	g_vmm.root = (uint64_t *)root_virtual;
	g_vmm.ttbr1_root = (uint64_t *)ttbr1_root_virtual;
	g_vmm.kernel_pointers_rebased = true;

	return true;
}

bool vmm_disable_ttbr0(void)
{
	if (
		!g_vmm.enabled ||
		!g_vmm.higher_half_enabled ||
		!g_vmm.higher_half_direct_map_enabled ||
		!g_vmm.kernel_pointers_rebased ||
		g_vmm.ttbr0_disabled
	) {
		return false;
	}

	uint64_t program_counter;

	__asm__ volatile("adr %0, ." : "=r"(program_counter));

	if (!vmm_is_higher_half_address(program_counter)) {
		return false;
	}

	uint64_t tcr = vmm_read_tcr() | VMM_TCR_EPD0;
	uint64_t zero = 0ULL;

	/*
	 * Stop future TTBR0 walks, discard the old root register value and
	 * invalidate cached EL1 translations. The next instruction fetches
	 * and data accesses are served exclusively through TTBR1.
	 */
	__asm__ volatile(
		"dsb ishst\n"
		"msr ttbr0_el1, %0\n"
		"msr tcr_el1, %1\n"
		"isb\n"
		"tlbi vmalle1is\n"
		"dsb ish\n"
		"isb\n"
		:
		: "r"(zero), "r"(tcr)
		: "memory"
	);

	g_vmm.root = 0;
	g_vmm.root_physical = 0ULL;
	g_vmm.ttbr0_disabled = true;

	return true;
}

bool vmm_ttbr0_disabled(void)
{
	return g_vmm.ttbr0_disabled;
}


bool vmm_create_identity_stub(
	uint64_t physical_address,
	uint64_t size,
	uint64_t *root_physical
)
{
	if (root_physical == 0 || size == 0ULL) {
		return false;
	}

	uint64_t *root;
	uint64_t root_address;
	uint64_t table_count = 0ULL;

	if (!vmm_allocate_table(&root, &root_address, &table_count)) {
		return false;
	}

	if (!vmm_root_map_range(
		root,
		physical_address,
		physical_address,
		size,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_EXECUTE,
		&table_count
	)) {
		return false;
	}

	/*
	 * Publish the finished tables before any CPU is pointed at them: the
	 * starting CPU's table walker observes them through the inner-shareable
	 * domain, so the stores must have completed there first.
	 */
	__asm__ volatile("dsb ishst" : : : "memory");

	*root_physical = root_address;
	return true;
}
