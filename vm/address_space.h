#ifndef NXU_VM_ADDRESS_SPACE_H
#define NXU_VM_ADDRESS_SPACE_H

#include <kern/cpuset.h>
#include <kern/lock.h>

#include <stdbool.h>
#include <stdint.h>

struct vm_map_entry;

/*
 * One lower-half stage-1 address space owned by TTBR0_EL1.
 *
 * The root pointer is a TTBR1 direct-map pointer used by the kernel.
 * root_physical is the address written to TTBR0_EL1.
 *
 * lock guards page-table mutation (map/unmap/query) below, needed once a
 * task can have more than one thread touching its own address space
 * concurrently. It does not guard activate/deactivate, which run on the
 * scheduler's context-switch path under the scheduler's own serialization.
 * vm/vm_map.c reuses this same lock for mmap_entries/mmap_next_va below --
 * one lock per address space, not one per feature -- taking care never to
 * hold it while calling back into vm_address_space_map_page/unmap_page/
 * query_page, which each take and release it internally per call.
 *
 * mmap_entries / mmap_next_va back vm/vm_map.h's anonymous mmap/munmap --
 * a real (if simple: bump-allocated, never reclaiming a freed gap) region
 * list, unlike vm/vm_shm.h's opaque per-caller bump cursor.
 */
typedef struct {
	uint64_t *root;
	uint64_t root_physical;
	uint64_t table_count;
	bool active;

	/*
	 * The CPUs whose TTBR0 currently points at this space (set on activate,
	 * cleared when that CPU leaves it). Only the boot CPU runs user threads
	 * today, so it is empty or {0}; it is what a page unmap consults to know
	 * which CPUs may hold a translation, and the TLB strategy is written so
	 * nothing else changes when more CPUs are added: page invalidations are
	 * inner-shareable and reach every CPU whether or not it is in this set.
	 */
	nxu_cpuset_t active_cpus;
	nxu_spinlock_t lock;
	struct vm_map_entry *mmap_entries;
	uint64_t mmap_next_va;
} vm_address_space_t;

typedef enum {
	VM_USER_PROTECTION_READ_WRITE,
	VM_USER_PROTECTION_READ_ONLY,
	VM_USER_PROTECTION_READ_EXECUTE
} vm_user_protection_t;

/*
 * cow is set for a page shared copy-on-write. Its hardware permission is
 * read-only, so protection reads VM_USER_PROTECTION_READ_ONLY -- code that
 * writes through physical_address must therefore refuse it (or break the
 * sharing first with vm_address_space_cow_break), never write blindly.
 */
typedef struct {
	uint64_t physical_address;
	vm_user_protection_t protection;
	bool cow;
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

/*
 * Remove one EL0 page mapping without freeing the underlying physical page
 * (see vm/pmm.h's pmm_page_retain/pmm_free_page for that) -- the counterpart
 * vm_address_space_map_page never had, needed so memory mapped into more
 * than one address space (vm/vm_shm.h) can be unmapped from just one of
 * them. Safe to call whether or not space is the currently active one.
 */
bool vm_address_space_unmap_page(
	vm_address_space_t *space,
	uint64_t virtual_address
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

/*
 * vm_address_space_fork
 *
 * Give child (a zeroed struct) a copy of parent's user mappings. Every
 * private writable page becomes copy-on-write in BOTH spaces -- read-only in
 * hardware, one more pmm reference each -- and is copied only when one side
 * writes to it. Read-only and executable pages are shared as they are.
 * Pages in the shared-memory window (VM_SHM_BASE) stay genuinely shared and
 * writable in both. The region list is not copied; see vm_map_fork().
 *
 * On failure child is left zeroed and parent is unchanged in meaning (any
 * page already marked copy-on-write simply copies, or reclaims, on its next
 * write). Returns false on ports that cannot fork.
 */
bool vm_address_space_fork(
	vm_address_space_t *parent,
	vm_address_space_t *child
);

/*
 * vm_address_space_cow_break
 *
 * Make the copy-on-write page at virtual_address private and writable:
 * reclaim it in place when this space is its only owner, otherwise copy it
 * to a fresh page. Returns false when the page is absent or not
 * copy-on-write.
 */
bool vm_address_space_cow_break(
	vm_address_space_t *space,
	uint64_t virtual_address
);

/*
 * Teardown, implemented by every port that reclaims address spaces (the
 * i386 port and, since fork needs it, arm64).
 *
 * vm_address_space_release_pages() unmaps every user page in space and drops
 * one pmm reference per mapping (pmm_free_page), so a page shared through
 * vm/vm_shm.h survives until its last mapping goes. It returns how many
 * mappings it removed. Only call it for spaces whose every mapping owns a
 * reference; pages mapped without one must be unmapped individually first.
 *
 * vm_address_space_destroy() frees the page tables and the root of space and
 * zeroes it; it leaves the mapped pages alone (release them first) and
 * deactivates space if it is the active one. vm_map_destroy_all() should have
 * run for the space, since destroy does not free its region list.
 */
uint64_t vm_address_space_release_pages(vm_address_space_t *space);
bool vm_address_space_destroy(vm_address_space_t *space);

#endif
