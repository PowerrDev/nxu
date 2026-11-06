/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/vm/arch/x86_64/pmap_types.h
 *
 * x86_64 definition of the opaque pmap_t. Included only via
 * <kern/vm/machine/pmap_types.h>.
 *
 * Phase 0 shim: this mirrors the three legacy fields the x86_64 code already
 * carried on vm_address_space_t, so the x86_64 target keeps compiling after
 * the shared struct is split. The x86_64 backend phase (Phase 2) folds in the
 * state currently held in x86_vmm_state_t (NX/PCID/table bits). Only
 * translation units under kern/vm/arch/x86_64/ may name these fields.
 */
#ifndef NXU_KERN_VM_ARCH_X86_64_PMAP_TYPES_H
#define NXU_KERN_VM_ARCH_X86_64_PMAP_TYPES_H

#include <stdint.h>

typedef struct pmap {
	uint64_t *root;
	uint64_t  root_physical;
	uint64_t  table_count;
} pmap_t;

#endif /* NXU_KERN_VM_ARCH_X86_64_PMAP_TYPES_H */
