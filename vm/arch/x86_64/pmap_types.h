/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/arch/x86_64/pmap_types.h
 *
 * x86_64 definition of the opaque pmap_t. Included only via
 * <vm/machine/pmap_types.h>.
 *
 * The four-level PML4 translation root, its physical form (loaded into CR3),
 * and the count of tables the space owns. NX/PCID and the shared kernel-half
 * branches are mode-wide state held in x86_vmm_state_t (g_x86_vmm), not
 * per-pmap, so nothing more is needed here. Only translation units under
 * vm/arch/x86_64/ may name these fields.
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
