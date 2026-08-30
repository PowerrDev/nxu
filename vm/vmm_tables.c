#include <vm/vmm_internal.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Translation-table mechanics shared by every root: allocation, walking,
 * descriptor encoding and decoding, and the live page operations built on
 * top of them.
 *
 * The kernel is single-core and these paths run unlocked.
 */

uint64_t vmm_index(uint64_t address, uint32_t shift)
{
	return (address >> shift) & VMM_INDEX_MASK;
}

bool vmm_table_pointer_from_physical(
	uint64_t physical_address,
	uint64_t **table
)
{
	if (table == 0 || !vmm_physical_page_valid(physical_address)) {
		return false;
	}

	if (!g_vmm.kernel_pointers_rebased) {
		*table = (uint64_t *)physical_address;
		return true;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		physical_address,
		&virtual_address
	)) {
		return false;
	}

	*table = (uint64_t *)virtual_address;
	return true;
}

bool vmm_allocate_table(
	uint64_t **table,
	uint64_t *physical,
	uint64_t *table_count
)
{
	if (table == 0 || physical == 0 || table_count == 0) {
		return false;
	}

	if (!pmm_allocate_page(physical)) {
		return false;
	}

	/*
	 * Before the permanent transition, tables are reached through the
	 * TTBR0 identity map. After rebasing, they are reached through the
	 * TTBR1 physical direct map.
	 *
	 * pmm_allocate_page() zeroes the page.
	 */
	if (!vmm_table_pointer_from_physical(*physical, table)) {
		(void)pmm_free_page(*physical);
		return false;
	}

	(*table_count)++;

	return true;
}

/*
 * Resolve one intermediate level. Used at L1 and L2, where a valid
 * descriptor must be a table descriptor: existing blocks are never split.
 */
static bool vmm_root_next_table(
	uint64_t *table,
	uint64_t index,
	bool create,
	uint64_t *table_count,
	uint64_t **next_table
)
{
	if (
		table == 0 ||
		table_count == 0 ||
		next_table == 0 ||
		index >= VMM_TABLE_ENTRY_COUNT
	) {
		return false;
	}

	uint64_t descriptor = table[index];

	if (descriptor & VMM_DESC_VALID) {
		if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) {
			return false;
		}

		return vmm_table_pointer_from_physical(
			descriptor & VMM_DESC_ADDRESS_MASK,
			next_table
		);
	}

	if (!create) {
		return false;
	}

	uint64_t physical;

	if (!vmm_allocate_table(next_table, &physical, table_count)) {
		return false;
	}

	/*
	 * Make the zeroed child table visible before its parent descriptor
	 * becomes valid.
	 */
	__asm__ volatile("dsb ishst" : : : "memory");

	table[index] = (physical & VMM_DESC_ADDRESS_MASK) | VMM_DESC_TABLE_PAGE;

	__asm__ volatile("dsb ishst" : : : "memory");

	return true;
}

static bool vmm_root_get_l2_table(
	uint64_t *root,
	uint64_t virtual_address,
	bool create,
	uint64_t *table_count,
	uint64_t **level2
)
{
	if (root == 0 || table_count == 0 || level2 == 0) {
		return false;
	}

	return vmm_root_next_table(
		root,
		vmm_index(virtual_address, VMM_L1_SHIFT),
		create,
		table_count,
		level2
	);
}

static bool vmm_install_descriptor(uint64_t *entry, uint64_t descriptor)
{
	if (entry == 0) {
		return false;
	}

	if ((*entry & VMM_DESC_VALID) == 0ULL) {
		*entry = descriptor;
		return true;
	}

	return *entry == descriptor;
}

bool vmm_root_get_l3_entry(
	uint64_t *root,
	uint64_t virtual_address,
	bool create,
	uint64_t *table_count,
	uint64_t **entry
)
{
	if (root == 0 || table_count == 0 || entry == 0) {
		return false;
	}

	uint64_t *level2;

	if (!vmm_root_next_table(
		root,
		vmm_index(virtual_address, VMM_L1_SHIFT),
		create,
		table_count,
		&level2
	)) {
		return false;
	}

	uint64_t *level3;

	if (!vmm_root_next_table(
		level2,
		vmm_index(virtual_address, VMM_L2_SHIFT),
		create,
		table_count,
		&level3
	)) {
		return false;
	}

	*entry = &level3[vmm_index(virtual_address, VMM_L3_SHIFT)];

	return true;
}

bool vmm_attributes(
	vmm_memory_type_t type,
	vmm_protection_t protection,
	uint64_t *attributes
)
{
	if (attributes == 0) {
		return false;
	}

	uint64_t result;

	if (type == VMM_MEMORY_DEVICE) {
		/*
		 * Executing instructions from MMIO is never valid.
		 */
		if (protection == VMM_PROTECTION_READ_EXECUTE) {
			return false;
		}

		result =
			VMM_DESC_ATTR(VMM_MAIR_DEVICE_INDEX) |
			VMM_DESC_SH_OUTER |
			VMM_DESC_AF |
			VMM_DESC_PXN |
			VMM_DESC_UXN;

		if (protection == VMM_PROTECTION_READ_ONLY) {
			result |= VMM_DESC_AP_READ_ONLY;
		}

		*attributes = result;

		return true;
	}

	result =
		VMM_DESC_ATTR(VMM_MAIR_NORMAL_INDEX) |
		VMM_DESC_SH_INNER |
		VMM_DESC_AF |
		VMM_DESC_UXN;

	switch (protection) {
	case VMM_PROTECTION_READ_WRITE:
		/*
		 * AP remains 0b00:
		 *
		 * EL1: read-write
		 * EL0: no access
		 */
		result |= VMM_DESC_PXN;
		break;

	case VMM_PROTECTION_READ_ONLY:
		result |= VMM_DESC_AP_READ_ONLY | VMM_DESC_PXN;
		break;

	case VMM_PROTECTION_READ_EXECUTE:
		/*
		 * Read-only at EL1.
		 *
		 * PXN remains clear, allowing EL1 instruction fetches.
		 * UXN remains set, preventing EL0 execution.
		 */
		result |= VMM_DESC_AP_READ_ONLY;
		break;

	default:
		return false;
	}

	*attributes = result;

	return true;
}

bool vmm_root_map_range(
	uint64_t *root,
	uint64_t virtual_address,
	uint64_t physical_address,
	uint64_t size,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection,
	uint64_t *table_count
)
{
	if (root == 0 || table_count == 0) {
		return false;
	}

	if (size == 0ULL) {
		return true;
	}

	if (
		(virtual_address & (VMM_L3_SIZE - 1ULL)) != 0ULL ||
		(physical_address & (VMM_L3_SIZE - 1ULL)) != 0ULL ||
		(size & (VMM_L3_SIZE - 1ULL)) != 0ULL
	) {
		return false;
	}

	if (
		virtual_address > UINT64_MAX - size ||
		physical_address > UINT64_MAX - size
	) {
		return false;
	}

	uint64_t attributes;

	if (!vmm_attributes(memory_type, protection, &attributes)) {
		return false;
	}

	uint64_t remaining = size;

	while (remaining > 0ULL) {
		bool l1_aligned =
			(virtual_address & (VMM_L1_SIZE - 1ULL)) == 0ULL &&
			(physical_address & (VMM_L1_SIZE - 1ULL)) == 0ULL;

		if (l1_aligned && remaining >= VMM_L1_SIZE) {
			uint64_t *entry = &root[vmm_index(virtual_address, VMM_L1_SHIFT)];

			uint64_t descriptor =
				(physical_address & VMM_DESC_ADDRESS_MASK) |
				attributes |
				VMM_DESC_BLOCK;

			if ((*entry & VMM_DESC_VALID) == 0ULL || *entry == descriptor) {
				if (!vmm_install_descriptor(entry, descriptor)) {
					return false;
				}

				virtual_address += VMM_L1_SIZE;
				physical_address += VMM_L1_SIZE;
				remaining -= VMM_L1_SIZE;
				continue;
			}

			if ((*entry & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) {
				return false;
			}
		}

		uint64_t *level2;

		if (!vmm_root_get_l2_table(
			root,
			virtual_address,
			true,
			table_count,
			&level2
		)) {
			return false;
		}

		bool l2_aligned =
			(virtual_address & (VMM_L2_SIZE - 1ULL)) == 0ULL &&
			(physical_address & (VMM_L2_SIZE - 1ULL)) == 0ULL;

		uint64_t *level2_entry = &level2[vmm_index(virtual_address, VMM_L2_SHIFT)];

		if (l2_aligned && remaining >= VMM_L2_SIZE) {
			uint64_t descriptor =
				(physical_address & VMM_DESC_ADDRESS_MASK) |
				attributes |
				VMM_DESC_BLOCK;

			if (
				(*level2_entry & VMM_DESC_VALID) == 0ULL ||
				*level2_entry == descriptor
			) {
				if (!vmm_install_descriptor(level2_entry, descriptor)) {
					return false;
				}

				virtual_address += VMM_L2_SIZE;
				physical_address += VMM_L2_SIZE;
				remaining -= VMM_L2_SIZE;
				continue;
			}

			if (
				(*level2_entry & VMM_DESC_TYPE_MASK) !=
				VMM_DESC_TABLE_PAGE
			) {
				return false;
			}
		}

		uint64_t *entry;

		if (!vmm_root_get_l3_entry(
			root,
			virtual_address,
			true,
			table_count,
			&entry
		)) {
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

		if (!vmm_install_descriptor(entry, descriptor)) {
			return false;
		}

		virtual_address += VMM_L3_SIZE;
		physical_address += VMM_L3_SIZE;
		remaining -= VMM_L3_SIZE;
	}

	vmm_publish_new_mapping();

	return true;
}

bool vmm_physical_page_valid(uint64_t physical_address)
{
	uint64_t limit = 1ULL << g_vmm.physical_bits;

	return (physical_address & (VMM_L3_SIZE - 1ULL)) == 0ULL &&
		physical_address <= limit - VMM_L3_SIZE;
}

bool vmm_lower_page_valid(uint64_t virtual_address)
{
	uint64_t limit = 1ULL << VMM_VA_BITS;

	return (virtual_address & (VMM_L3_SIZE - 1ULL)) == 0ULL &&
		virtual_address <= limit - VMM_L3_SIZE;
}

bool vmm_higher_page_valid(uint64_t virtual_address)
{
	return vmm_is_higher_half_address(virtual_address) &&
		(virtual_address & (VMM_L3_SIZE - 1ULL)) == 0ULL;
}

bool vmm_make_page_descriptor(
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection,
	uint64_t *descriptor
)
{
	if (descriptor == 0 || !vmm_physical_page_valid(physical_address)) {
		return false;
	}

	uint64_t attributes;

	if (!vmm_attributes(memory_type, protection, &attributes)) {
		return false;
	}

	*descriptor =
		(physical_address & VMM_DESC_ADDRESS_MASK) |
		attributes |
		VMM_DESC_TABLE_PAGE;

	return true;
}

bool vmm_descriptor_memory_type(
	uint64_t descriptor,
	vmm_memory_type_t *memory_type
)
{
	if (memory_type == 0) {
		return false;
	}

	uint64_t attribute_index = (descriptor & VMM_DESC_ATTR_MASK) >> 2U;

	if (attribute_index == VMM_MAIR_NORMAL_INDEX) {
		*memory_type = VMM_MEMORY_NORMAL;
		return true;
	}

	if (attribute_index == VMM_MAIR_DEVICE_INDEX) {
		*memory_type = VMM_MEMORY_DEVICE;
		return true;
	}

	return false;
}

bool vmm_descriptor_protection(
	uint64_t descriptor,
	vmm_protection_t *protection
)
{
	if (protection == 0) {
		return false;
	}

	uint64_t access_permissions = descriptor & VMM_DESC_AP_MASK;

	bool privileged_execute_never = (descriptor & VMM_DESC_PXN) != 0ULL;

	if (access_permissions == 0ULL) {
		/*
		 * NXU never intentionally creates writable and executable
		 * mappings.
		 */
		if (!privileged_execute_never) {
			return false;
		}

		*protection = VMM_PROTECTION_READ_WRITE;

		return true;
	}

	if (access_permissions == VMM_DESC_AP_READ_ONLY) {
		*protection = privileged_execute_never
			? VMM_PROTECTION_READ_ONLY
			: VMM_PROTECTION_READ_EXECUTE;

		return true;
	}

	return false;
}

void vmm_publish_new_mapping(void)
{
	/*
	 * The entry was previously invalid. Translation faults are not
	 * cached in the TLB, so no TLBI is required for a first mapping.
	 *
	 * The DSB makes the table writes visible before the mapping is
	 * used. ISB synchronizes subsequent execution.
	 */
	__asm__ volatile(
		"dsb ish\n"
		"isb\n"
		:
		:
		: "memory"
	);
}

void vmm_invalidate_page(uint64_t virtual_address)
{
	/*
	 * For a 4 KiB granule, the TLBI operand contains the virtual page
	 * number rather than the complete byte address.
	 */
	uint64_t operand = virtual_address >> VMM_L3_SHIFT;

	__asm__ volatile(
		"dsb ish\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:
		: "r"(operand)
		: "memory"
	);
}

static bool vmm_set_entry(uint64_t *entry, uint64_t descriptor)
{
	if ((*entry & VMM_DESC_VALID) == 0ULL) {
		*entry = descriptor;
		return true;
	}

	/*
	 * Multiple 512-byte VirtIO windows can occupy the same 4 KiB page.
	 * Reinstalling the identical mapping is valid.
	 */
	return *entry == descriptor;
}

bool vmm_map_initial_l1(uint64_t address, uint64_t attributes)
{
	uint64_t descriptor = (address & ~(VMM_L1_SIZE - 1ULL)) | attributes | VMM_DESC_BLOCK;

	uint64_t index = vmm_index(address, VMM_L1_SHIFT);

	return vmm_set_entry(&g_vmm.root[index], descriptor);
}

bool vmm_map_initial_l2(uint64_t address, uint64_t attributes)
{
	uint64_t *level2;

	if (!vmm_root_next_table(
		g_vmm.root,
		vmm_index(address, VMM_L1_SHIFT),
		true,
		&g_vmm.table_count,
		&level2
	)) {
		return false;
	}

	uint64_t descriptor = (address & ~(VMM_L2_SIZE - 1ULL)) | attributes | VMM_DESC_BLOCK;

	uint64_t index = vmm_index(address, VMM_L2_SHIFT);

	return vmm_set_entry(&level2[index], descriptor);
}

bool vmm_map_initial_l3(uint64_t address, uint64_t attributes)
{
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		g_vmm.root,
		address,
		true,
		&g_vmm.table_count,
		&entry
	)) {
		return false;
	}

	uint64_t descriptor =
		(address & VMM_DESC_ADDRESS_MASK) |
		attributes |
		VMM_DESC_TABLE_PAGE;

	return vmm_set_entry(entry, descriptor);
}

/*
 * Select the translation-table root responsible for a virtual address.
 *
 * Lower canonical addresses belong to TTBR0. Upper canonical addresses
 * belong to TTBR1.
 */
static bool vmm_select_root(
	uint64_t virtual_address,
	uint64_t **root,
	uint64_t **table_count
)
{
	if (root == 0 || table_count == 0) {
		return false;
	}

	if (vmm_is_higher_half_address(virtual_address)) {
		if (
			!g_vmm.higher_half_enabled ||
			!vmm_higher_page_valid(virtual_address)
		) {
			return false;
		}

		*root = g_vmm.ttbr1_root;
		*table_count = &g_vmm.ttbr1_table_count;

		return true;
	}

	if (
		g_vmm.ttbr0_disabled ||
		g_vmm.root == 0 ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	*root = g_vmm.root;
	*table_count = &g_vmm.table_count;

	return true;
}

bool vmm_map_page(
	uint64_t virtual_address,
	uint64_t physical_address,
	vmm_memory_type_t memory_type,
	vmm_protection_t protection
)
{
	if (
		!g_vmm.enabled ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t *root;
	uint64_t *table_count;

	if (!vmm_select_root(
		virtual_address,
		&root,
		&table_count
	)) {
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

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		root,
		virtual_address,
		true,
		table_count,
		&entry
	)) {
		return false;
	}

	if ((*entry & VMM_DESC_VALID) != 0ULL) {
		/*
		 * Existing mappings must not be overwritten implicitly.
		 * Permission changes use vmm_protect_page().
		 */
		return false;
	}

	*entry = descriptor;
	vmm_publish_new_mapping();

	return true;
}

bool vmm_unmap_page(
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	if (!g_vmm.enabled) {
		return false;
	}

	uint64_t *root;
	uint64_t *table_count;

	if (!vmm_select_root(
		virtual_address,
		&root,
		&table_count
	)) {
		return false;
	}

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		root,
		virtual_address,
		false,
		table_count,
		&entry
	)) {
		return false;
	}

	uint64_t descriptor = *entry;

	if (
		(descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE
	) {
		return false;
	}

	uint64_t old_physical_address =
		descriptor &
		VMM_DESC_ADDRESS_MASK;

	/*
	 * Break the mapping before invalidating its cached translation.
	 */
	*entry = 0ULL;
	vmm_invalidate_page(virtual_address);

	if (physical_address != 0) {
		*physical_address = old_physical_address;
	}

	return true;
}

bool vmm_protect_page(
	uint64_t virtual_address,
	vmm_protection_t protection
)
{
	if (!g_vmm.enabled) {
		return false;
	}

	uint64_t *root;
	uint64_t *table_count;

	if (!vmm_select_root(
		virtual_address,
		&root,
		&table_count
	)) {
		return false;
	}

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		root,
		virtual_address,
		false,
		table_count,
		&entry
	)) {
		return false;
	}

	uint64_t old_descriptor = *entry;

	if (
		(old_descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE
	) {
		return false;
	}

	vmm_memory_type_t memory_type;

	if (!vmm_descriptor_memory_type(
		old_descriptor,
		&memory_type
	)) {
		return false;
	}

	uint64_t physical_address =
		old_descriptor &
		VMM_DESC_ADDRESS_MASK;

	uint64_t new_descriptor;

	if (!vmm_make_page_descriptor(
		physical_address,
		memory_type,
		protection,
		&new_descriptor
	)) {
		return false;
	}

	if (new_descriptor == old_descriptor) {
		return true;
	}

	/*
	 * Break-before-make:
	 *
	 * 1. Invalidate the old descriptor.
	 * 2. Remove its cached translation.
	 * 3. Install the replacement descriptor.
	 */
	*entry = 0ULL;
	vmm_invalidate_page(virtual_address);

	*entry = new_descriptor;
	vmm_publish_new_mapping();

	return true;
}

bool vmm_query_page(
	uint64_t virtual_address,
	vmm_page_mapping_t *mapping
)
{
	if (!g_vmm.enabled || mapping == 0) {
		return false;
	}

	uint64_t *root;
	uint64_t *table_count;

	if (!vmm_select_root(
		virtual_address,
		&root,
		&table_count
	)) {
		return false;
	}

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		root,
		virtual_address,
		false,
		table_count,
		&entry
	)) {
		return false;
	}

	uint64_t descriptor = *entry;

	if (
		(descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE
	) {
		return false;
	}

	if (!vmm_descriptor_memory_type(
		descriptor,
		&mapping->memory_type
	)) {
		return false;
	}

	if (!vmm_descriptor_protection(
		descriptor,
		&mapping->protection
	)) {
		return false;
	}

	mapping->physical_address =
		descriptor &
		VMM_DESC_ADDRESS_MASK;

	return true;
}

/*
 * Ask the hardware to translate an address exactly as an EL1 access
 * would, then interpret PAR_EL1.
 */
static bool vmm_translate_access(
	uint64_t virtual_address,
	bool write,
	uint64_t *physical_address
)
{
	if (!g_vmm.enabled || physical_address == 0) {
		return false;
	}

	uint64_t par;

	if (write) {
		__asm__ volatile(
			"at S1E1W, %1\n"
			"isb\n"
			"mrs %0, PAR_EL1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	} else {
		__asm__ volatile(
			"at S1E1R, %1\n"
			"isb\n"
			"mrs %0, PAR_EL1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	}

	if ((par & 1ULL) != 0ULL) {
		return false;
	}

	*physical_address =
		(par & VMM_DESC_ADDRESS_MASK) |
		(virtual_address & (VMM_L3_SIZE - 1ULL));

	return true;
}

bool vmm_translate(uint64_t virtual_address, uint64_t *physical_address)
{
	return vmm_translate_access(virtual_address, false, physical_address);
}

bool vmm_translate_write(uint64_t virtual_address, uint64_t *physical_address)
{
	return vmm_translate_access(virtual_address, true, physical_address);
}
