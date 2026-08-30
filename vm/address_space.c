#include <vm/address_space.h>
#include <vm/vmm_internal.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_USER_NULL_GUARD_SIZE VMM_L3_SIZE

/*
 * AP[2:1] values used by EL0 mappings:
 *
 * 0b01: EL1 read-write, EL0 read-write
 * 0b11: EL1 read-only,  EL0 read-only
 */
#define VM_USER_AP_READ_WRITE (1ULL << 6U)
#define VM_USER_AP_READ_ONLY (3ULL << 6U)

/* PAR_EL1.F is bit zero after an AT instruction. */
#define VM_PAR_FAULT (1ULL << 0U)

/*
 * Lesson 18 begins with one address space and ASID 0.
 *
 * Every activation invalidates the EL1 TLB. ASIDs become useful only once
 * NXU can keep multiple user address spaces alive and switch between them.
 */
static vm_address_space_t *g_active_address_space;

static bool vm_user_descriptor(
	uint64_t physical_address,
	vm_user_protection_t protection,
	uint64_t *descriptor
)
{
	if (
		descriptor == 0 ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t attributes =
		VMM_DESC_ATTR(VMM_MAIR_NORMAL_INDEX) |
		VMM_DESC_SH_INNER |
		VMM_DESC_AF |
		VMM_DESC_PXN;

	switch (protection) {
	case VM_USER_PROTECTION_READ_WRITE:
		attributes |=
			VM_USER_AP_READ_WRITE |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_ONLY:
		attributes |=
			VM_USER_AP_READ_ONLY |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_EXECUTE:
		/*
		 * EL1 may read the page but cannot execute it. EL0 may read and
		 * execute it, while writes are rejected by AP[2:1] = 0b11.
		 */
		attributes |= VM_USER_AP_READ_ONLY;
		break;

	default:
		return false;
	}

	*descriptor =
		(physical_address & VMM_DESC_ADDRESS_MASK) |
		attributes |
		VMM_DESC_TABLE_PAGE;

	return true;
}

static bool vm_user_descriptor_protection(
	uint64_t descriptor,
	vm_user_protection_t *protection
)
{
	if (protection == 0) {
		return false;
	}

	uint64_t access_permissions = descriptor & VMM_DESC_AP_MASK;

	bool privileged_execute_never = (descriptor & VMM_DESC_PXN) != 0ULL;

	bool user_execute_never = (descriptor & VMM_DESC_UXN) != 0ULL;

	if (!privileged_execute_never) {
		return false;
	}

	if (
		access_permissions == VM_USER_AP_READ_WRITE &&
		user_execute_never
	) {
		*protection = VM_USER_PROTECTION_READ_WRITE;
		return true;
	}

	if (access_permissions == VM_USER_AP_READ_ONLY) {
		*protection = user_execute_never
			? VM_USER_PROTECTION_READ_ONLY
			: VM_USER_PROTECTION_READ_EXECUTE;

		return true;
	}

	return false;
}

static bool vm_address_space_translate(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	bool write,
	uint64_t *physical_address
)
{
	if (
		!vm_address_space_is_active(space) ||
		physical_address == 0
	) {
		return false;
	}

	uint64_t par;

	if (write) {
		__asm__ volatile(
			"at s1e0w, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	} else {
		__asm__ volatile(
			"at s1e0r, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	}

	if ((par & VM_PAR_FAULT) != 0ULL) {
		return false;
	}

	*physical_address =
		(par & VMM_DESC_ADDRESS_MASK) |
		(virtual_address & (VMM_L3_SIZE - 1ULL));

	return true;
}

const vm_address_space_t *vm_address_space_current(void)
{
	return g_active_address_space;
}

bool vm_address_space_create(vm_address_space_t *space)
{
	if (
		space == 0 ||
		!g_vmm.enabled ||
		!g_vmm.higher_half_enabled ||
		!g_vmm.higher_half_direct_map_enabled ||
		!g_vmm.kernel_pointers_rebased ||
		space->root != 0 ||
		space->root_physical != 0ULL ||
		space->table_count != 0ULL ||
		space->active
	) {
		return false;
	}

	memset(space, 0, sizeof(*space));

	if (!vmm_allocate_table(
		&space->root,
		&space->root_physical,
		&space->table_count
	)) {
		memset(space, 0, sizeof(*space));
		return false;
	}

	return true;
}

bool vm_address_space_activate(vm_address_space_t *space)
{
	if (
		space == 0 ||
		space->root == 0 ||
		space->root_physical == 0ULL ||
		space->table_count == 0ULL ||
		!g_vmm.enabled ||
		!g_vmm.kernel_pointers_rebased
	) {
		return false;
	}

	if (
		g_active_address_space == space &&
		!g_vmm.ttbr0_disabled
	) {
		return true;
	}

	/* Preserve table accounting before leaving a previous address space. */
	if (g_active_address_space != 0) {
		g_active_address_space->table_count = g_vmm.table_count;
		g_active_address_space->active = false;
	}

	uint64_t tcr = vmm_read_tcr() & ~VMM_TCR_EPD0;
	uint64_t ttbr0 = space->root_physical & VMM_DESC_ADDRESS_MASK;

	/*
	 * TTBR0 was disabled at the end of Lesson 17. Install the new root,
	 * allow TTBR0 walks again, and remove translations from the previous
	 * lower address space before any user mapping can be observed.
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
		: "r"(ttbr0), "r"(tcr)
		: "memory"
	);

	g_vmm.root = space->root;
	g_vmm.root_physical = space->root_physical;
	g_vmm.table_count = space->table_count;
	g_vmm.ttbr0_disabled = false;

	space->active = true;
	g_active_address_space = space;

	return true;
}

bool vm_address_space_deactivate(void)
{
	if (!g_vmm.enabled || !g_vmm.kernel_pointers_rebased) return false;

	if (g_active_address_space != 0) {
		g_active_address_space->table_count = g_vmm.table_count;
		g_active_address_space->active = false;
	}

	if (!g_vmm.ttbr0_disabled && !vmm_disable_ttbr0()) return false;

	g_active_address_space = 0;
	return true;
}

bool vm_address_space_map_page(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address) ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t descriptor;

	if (!vm_user_descriptor(
		physical_address,
		protection,
		&descriptor
	)) {
		return false;
	}

	uint64_t *table_count =
		space == g_active_address_space
			? &g_vmm.table_count
			: &space->table_count;

	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		true,
		table_count,
		&entry
	)) {
		return false;
	}

	if ((*entry & VMM_DESC_VALID) != 0ULL) {
		return false;
	}

	*entry = descriptor;
	vmm_publish_new_mapping();

	if (space == g_active_address_space) {
		space->table_count = g_vmm.table_count;
	}

	return true;
}

bool vm_address_space_query_page(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	vm_user_page_mapping_t *mapping
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		mapping == 0 ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	uint64_t table_count = space->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		false,
		&table_count,
		&entry
	)) {
		return false;
	}

	uint64_t descriptor = *entry;

	if (
		(descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE ||
		((descriptor & VMM_DESC_ATTR_MASK) >> 2U) !=
		VMM_MAIR_NORMAL_INDEX
	) {
		return false;
	}

	if (!vm_user_descriptor_protection(
		descriptor,
		&mapping->protection
	)) {
		return false;
	}

	mapping->physical_address = descriptor & VMM_DESC_ADDRESS_MASK;

	return true;
}

bool vm_address_space_translate_read(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		false,
		physical_address
	);
}

bool vm_address_space_translate_write(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		true,
		physical_address
	);
}

bool vm_address_space_is_active(const vm_address_space_t *space)
{
	return
		space != 0 &&
		space == g_active_address_space &&
		space->active &&
		!g_vmm.ttbr0_disabled;
}

uint64_t vm_address_space_root_physical(
	const vm_address_space_t *space
)
{
	return space == 0 ? 0ULL : space->root_physical;
}

uint64_t vm_address_space_table_count(
	const vm_address_space_t *space
)
{
	if (space == 0) {
		return 0ULL;
	}

	if (space == g_active_address_space) {
		return g_vmm.table_count;
	}

	return space->table_count;
}
