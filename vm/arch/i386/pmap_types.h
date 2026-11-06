/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/arch/i386/pmap_types.h
 *
 * i386 definition of the opaque pmap_t. Included only via
 * <vm/machine/pmap_types.h>.
 *
 * Phase 0 shim: this mirrors the three legacy fields the i386 code already
 * carried on vm_address_space_t, so the i386 target keeps compiling after the
 * shared struct is split. The i386 backend phase (Phase 1) replaces/extends
 * this with whatever i386-specific state its pmap.c needs. Only translation
 * units under vm/arch/i386/ may name these fields.
 */
#ifndef NXU_KERN_VM_ARCH_I386_PMAP_TYPES_H
#define NXU_KERN_VM_ARCH_I386_PMAP_TYPES_H

#include <stdint.h>

typedef struct pmap {
	uint64_t *root;
	uint64_t  root_physical;
	uint64_t  table_count;
} pmap_t;

#endif /* NXU_KERN_VM_ARCH_I386_PMAP_TYPES_H */
