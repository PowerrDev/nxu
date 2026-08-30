#ifndef NXU_VM_ADDRESS_SPACE_H
#define NXU_VM_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * One lower-half stage-1 address space owned by TTBR0_EL1.
 *
 * The root pointer is a TTBR1 direct-map pointer used by the kernel.
 * root_physical is the address written to TTBR0_EL1.
 */
typedef struct {
	uint64_t *root;
	uint64_t root_physical;
	uint64_t table_count;
	bool active;
} vm_address_space_t;

typedef enum {
	VM_USER_PROTECTION_READ_WRITE,
	VM_USER_PROTECTION_READ_ONLY,
	VM_USER_PROTECTION_READ_EXECUTE
} vm_user_protection_t;

typedef struct {
	uint64_t physical_address;
	vm_user_protection_t protection;
} vm_user_page_mapping_t;

typedef struct {
	uint64_t code_physical;
	uint64_t data_physical;
	uint64_t stack_physical;
} vm_bootstrap_user_pages_t;

/* Allocate an empty L1 translation-table root. */
bool vm_address_space_create(vm_address_space_t *space);

/* Install the address space in TTBR0_EL1 and enable TTBR0 walks. */
bool vm_address_space_activate(vm_address_space_t *space);

/*
 * Disable TTBR0 and return the CPU to a kernel-only translation regime.
 * Any previously active userspace object remains allocated but is marked
 * inactive until selected again by the scheduler.
 */
bool vm_address_space_deactivate(void);

/* Map one Normal-memory page with EL0 permissions. */
bool vm_address_space_map_page(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
);

/* Inspect one existing EL0 page mapping. */
bool vm_address_space_query_page(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	vm_user_page_mapping_t *mapping
);

/* Test an active address-space translation as an EL0 read or write. */
bool vm_address_space_translate_read(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
);

bool vm_address_space_translate_write(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
);

bool vm_address_space_is_active(const vm_address_space_t *space);

const vm_address_space_t *vm_address_space_current(void);

uint64_t vm_address_space_root_physical(
	const vm_address_space_t *space
);

uint64_t vm_address_space_table_count(
	const vm_address_space_t *space
);

#endif
