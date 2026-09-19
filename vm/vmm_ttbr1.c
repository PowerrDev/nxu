#include <vm/vmm_internal.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * TTBR1 kernel mapping and physical direct map.
 *
 * NXU is linked at its permanent higher-half VMA while the raw image is
 * loaded at the corresponding physical LMA. During bootstrap the same bytes
 * execute through TTBR0's physical alias. TTBR1 makes the linked VMA valid
 * before the final transition.
 */

bool vmm_is_higher_half_address(uint64_t virtual_address)
{
	return (virtual_address & VMM_VA_HIGH_MASK) == VMM_VA_HIGH_MASK;
}

bool vmm_physical_to_higher_half(
	uint64_t physical_address,
	uint64_t *virtual_address
)
{
	if (virtual_address == 0 || physical_address > VMM_VA_LOW_MASK) {
		return false;
	}

	*virtual_address = VMM_HIGHER_HALF_BASE | physical_address;

	return true;
}

bool vmm_higher_half_to_physical(
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	if (
		physical_address == 0 ||
		!vmm_is_higher_half_address(virtual_address)
	) {
		return false;
	}

	*physical_address = virtual_address & VMM_VA_LOW_MASK;
	return true;
}

bool vmm_kernel_address_to_physical(
	uint64_t kernel_address,
	uint64_t *physical_address
)
{
	if (physical_address == 0) return false;

	if (vmm_is_higher_half_address(kernel_address)) {
		*physical_address = kernel_address & VMM_VA_LOW_MASK;
		return true;
	}

	if (kernel_address > VMM_VA_LOW_MASK) return false;

	*physical_address = kernel_address;
	return true;
}

bool vmm_higher_half_enabled(void)
{
	return g_vmm.higher_half_enabled;
}

uint64_t vmm_get_ttbr1_root(void)
{
	return (uint64_t)g_vmm.ttbr1_root;
}

uint64_t vmm_get_ttbr1_table_count(void)
{
	return g_vmm.ttbr1_table_count;
}

static bool vmm_map_ttbr1_page(
	uint64_t virtual_address,
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (
		g_vmm.ttbr1_root == 0 ||
		!vmm_higher_page_valid(virtual_address) ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		g_vmm.ttbr1_root,
		virtual_address,
		true,
		&g_vmm.ttbr1_table_count,
		&entry
	)) {
		return false;
	}

	if (*entry & VMM_DESC_VALID) {
		return false;
	}

	uint64_t descriptor;

	if (!vmm_make_page_descriptor(
		physical_address,
		memory_type,
		protection,
		&descriptor
	)) {
		return false;
	}

	*entry = descriptor;

	vmm_publish_new_mapping();

	return true;
}

static uint64_t vmm_ttbr1_align_down(uint64_t value)
{
	return value & ~(VMM_L3_SIZE - 1ULL);
}

static bool vmm_ttbr1_align_up(uint64_t value, uint64_t *result)
{
	if (result == 0 || value > UINT64_MAX - (VMM_L3_SIZE - 1ULL)) {
		return false;
	}

	*result =
		(value + VMM_L3_SIZE - 1ULL) &
		~(VMM_L3_SIZE - 1ULL);

	return true;
}

static bool vmm_map_ttbr1_physical_range(
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (size == 0ULL) {
		return true;
	}

	if (physical_address > UINT64_MAX - size) {
		return false;
	}

	uint64_t physical_start = vmm_ttbr1_align_down(physical_address);
	uint64_t physical_end;

	if (!vmm_ttbr1_align_up(physical_address + size, &physical_end)) {
		return false;
	}

	if (physical_end == 0ULL || physical_end - 1ULL > VMM_VA_LOW_MASK) {
		return false;
	}

	uint64_t virtual_start;

	if (!vmm_physical_to_higher_half(physical_start, &virtual_start)) {
		return false;
	}

	return vmm_root_map_range(
		g_vmm.ttbr1_root,
		virtual_start,
		physical_start,
		physical_end - physical_start,
		memory_type,
		protection,
		&g_vmm.ttbr1_table_count
	);
}

static bool vmm_map_ttbr1_region(
	const platform_region_t *region,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (region == 0 || region->size == 0ULL) {
		return true;
	}

	return vmm_map_ttbr1_physical_range(
		region->base,
		region->size,
		memory_type,
		protection
	);
}

static bool vmm_map_ttbr1_span(
	uint64_t start,
	uint64_t end,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (end < start) {
		return false;
	}

	if (end == start) {
		return true;
	}

	return vmm_map_ttbr1_physical_range(
		start,
		end - start,
		memory_type,
		protection
	);
}

static uint64_t vmm_kernel_symbol_physical(const void *symbol)
{
	uint64_t physical_address = 0ULL;

	if (!vmm_kernel_address_to_physical((uint64_t)symbol, &physical_address)) {
		return UINT64_MAX;
	}

	return physical_address;
}

static bool vmm_map_ttbr1_kernel_memory(const platform_region_t *memory)
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

	if (kernel_start < memory_start || kernel_end > memory_end) {
		return false;
	}

	if (!vmm_map_ttbr1_span(
		memory_start,
		text_start,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_span(
		text_start,
		text_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_EXECUTE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_span(
		rodata_start,
		rodata_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_ONLY
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_span(
		data_start,
		data_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_span(
		bss_start,
		bss_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	return vmm_map_ttbr1_span(
		kernel_end,
		memory_end,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	);
}

bool vmm_map_higher_half_direct_map(const platform_t *platform)
{
	if (
		platform == 0 ||
		!g_vmm.higher_half_enabled ||
		g_vmm.higher_half_direct_map_enabled
	) {
		return false;
	}

	bool kernel_memory_mapped = false;
	uint64_t kernel_start = vmm_kernel_symbol_physical(__kernel_start);
	uint64_t kernel_end = vmm_kernel_symbol_physical(__kernel_end);

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
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

			if (!vmm_map_ttbr1_kernel_memory(memory)) {
				return false;
			}

			kernel_memory_mapped = true;
			continue;
		}

		if (!vmm_map_ttbr1_region(
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

	if (!vmm_map_ttbr1_region(
		&platform->uart,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_region(
		&platform->fw_cfg,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_region(
		&platform->gic_distributor,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_region(
		&platform->gic_redistributor,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_region(
		&platform->rtc,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_region(
		&platform->pcie_ecam,
		VMM_MEMORY_DEVICE,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	for (uint32_t index = 0U; index < platform->virtio_mmio_count; index++) {
		if (!vmm_map_ttbr1_region(
			&platform->virtio_mmio[index],
			VMM_MEMORY_DEVICE,
			VMM_PROTECTION_READ_WRITE
		)) {
			return false;
		}
	}

	g_vmm.higher_half_direct_map_enabled = true;

	return true;
}

bool vmm_higher_half_direct_map_enabled(void)
{
	return g_vmm.higher_half_direct_map_enabled;
}

static bool vmm_map_ttbr1_range(
	uint64_t physical_start,
	uint64_t physical_end,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (
		physical_start > physical_end ||
		(physical_start & (VMM_L3_SIZE - 1ULL)) != 0ULL ||
		(physical_end & (VMM_L3_SIZE - 1ULL)) != 0ULL
	) {
		return false;
	}

	for (
		uint64_t physical_address = physical_start;
		physical_address < physical_end;
		physical_address += VMM_L3_SIZE
	) {
		uint64_t virtual_address;

		if (!vmm_physical_to_higher_half(
			physical_address,
			&virtual_address
		)) {
			return false;
		}

		if (!vmm_map_ttbr1_page(
			virtual_address,
			physical_address,
			memory_type,
			protection
		)) {
			return false;
		}
	}

	return true;
}

static bool vmm_map_kernel_ttbr1_alias(void)
{
	if (!vmm_map_ttbr1_range(
		vmm_kernel_symbol_physical(__text_start),
		vmm_kernel_symbol_physical(__text_end),
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_EXECUTE
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_range(
		vmm_kernel_symbol_physical(__rodata_start),
		vmm_kernel_symbol_physical(__rodata_end),
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_ONLY
	)) {
		return false;
	}

	if (!vmm_map_ttbr1_range(
		vmm_kernel_symbol_physical(__data_start),
		vmm_kernel_symbol_physical(__data_end),
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		return false;
	}

	return vmm_map_ttbr1_range(
		vmm_kernel_symbol_physical(__bss_start),
		vmm_kernel_symbol_physical(__bss_end),
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	);
}

bool vmm_init_higher_half_alias(void)
{
	if (
		!g_vmm.enabled ||
		g_vmm.higher_half_enabled ||
		g_vmm.ttbr1_root != 0
	) {
		return false;
	}

	if (!vmm_allocate_table(
		&g_vmm.ttbr1_root,
		&g_vmm.ttbr1_root_physical,
		&g_vmm.ttbr1_table_count
	)) {
		return false;
	}

	/*
	 * The complete hierarchy is built before any table walk can reach
	 * it: EPD1 still disables TTBR1 translation at this point.
	 */
	if (!vmm_map_kernel_ttbr1_alias()) {
		return false;
	}

	uint64_t tcr = vmm_read_tcr();

	/*
	 * The remaining T1 fields were configured during vmm_init().
	 * Clearing EPD1 permits TTBR1 translation-table walks.
	 */
	tcr &= ~VMM_TCR_EPD1;

	/*
	 * Publish every table entry before making the root active.
	 */
	__asm__ volatile(
		"dsb ishst\n"
		"msr ttbr1_el1, %0\n"
		"isb\n"
		:
		: "r"(g_vmm.ttbr1_root_physical)
		: "memory"
	);

	/*
	 * TCR_EL1 controls how cached translations are interpreted.
	 * Invalidate the EL1 translation regime after enabling TTBR1.
	 */
	__asm__ volatile(
		"msr tcr_el1, %0\n"
		"isb\n"
		"tlbi vmalle1is\n"
		"dsb ish\n"
		"isb\n"
		:
		: "r"(tcr)
		: "memory"
	);

	g_vmm.higher_half_enabled = true;

	return true;
}
