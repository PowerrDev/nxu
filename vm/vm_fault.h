/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_fault.h
 *
 * User page-fault resolution. When an access to user memory cannot complete
 * -- from EL0, or from the kernel dereferencing a user address on behalf of
 * a syscall -- this decides whether the access is legitimate and, if so,
 * makes it possible so the caller can simply retry.
 *
 * Two kinds of fault are resolvable:
 *
 *   demand-zero   the page is inside a region reserved with vm_map_anon or
 *                 vm_map_reserve but has never been touched: allocate a
 *                 zeroed page and map it with the region's protection.
 *
 *   copy-on-write a write to a page shared by fork: give the writer its own
 *                 copy (vm_address_space_cow_break).
 *
 * Everything else -- a wild pointer, a write to text, a jump into data --
 * stays a genuine fault and the caller turns it into a signal.
 */

#ifndef NXU_VM_VM_FAULT_H
#define NXU_VM_VM_FAULT_H

#include <vm/address_space.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	VM_FAULT_READ,
	VM_FAULT_WRITE,
	VM_FAULT_EXECUTE
} vm_fault_access_t;

/*
 * vm_fault_user
 *
 * Try to resolve a fault at virtual_address in space. Returns true when the
 * page is now mapped so the access can be retried; false when the access is
 * genuinely invalid (or memory ran out), in which case nothing changed.
 */
bool vm_fault_user(
	vm_address_space_t *space,
	uint64_t virtual_address,
	vm_fault_access_t access
);

#endif
