/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/vm_types.h
 *
 * Neutral VM types shared by the architecture-neutral VM core and the pmap
 * backend contract (<vm/pmap.h>). Deliberately free of any architecture
 * concept (no TTBR/CR3/ASID/PCID/EL or descriptor formats) so that
 * <vm/pmap.h> can use these types without depending on
 * <vm/address_space.h>.
 *
 * See doc/vm/unified-pmap.md.
 */
#ifndef NXU_KERN_VM_VM_TYPES_H
#define NXU_KERN_VM_VM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Page protection. Unifies the former vmm_protection_t (vm/vmm.h) and
 * vm_user_protection_t (vm/address_space.h), which named the same three
 * states. The compatibility aliases that let the as-yet-unmigrated vmm.*
 * code keep compiling are introduced together with the removal of those two
 * enums (Phase 0 step 3); defining them here before the old enums are gone
 * would rewrite their definitions, so they are intentionally absent for now.
 */
typedef enum {
	VM_PROT_RW, /* read + write   */
	VM_PROT_RO, /* read only      */
	VM_PROT_RX, /* read + execute */
} vm_prot_t;

/*
 * Neutral result of a single-page query. When `cow` is set the hardware
 * permission is read-only and `protection` reads VM_PROT_RO; a writer must
 * break the sharing before writing through `physical_address`.
 *
 * Moved here out of vm/address_space.h so that <vm/pmap.h> can return it
 * without depending on the address-space struct.
 */
typedef struct {
	uint64_t  physical_address;
	vm_prot_t protection;
	bool      cow;
} vm_user_page_mapping_t;

#endif /* NXU_KERN_VM_VM_TYPES_H */
