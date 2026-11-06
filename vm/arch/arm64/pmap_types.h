/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/arch/arm64/pmap_types.h
 *
 * arm64 definition of the opaque pmap_t. Included only via
 * <vm/machine/pmap_types.h>; neutral code never includes it directly.
 *
 * Only translation units under vm/arch/arm64/ may name these fields.
 * Neutral code treats pmap_t as opaque (practical, discipline-and-CI-enforced
 * opacity, since pmap_t is embedded by value in vm_address_space_t). See the
 * "opaque pmap_t rule" in doc/vm/unified-pmap.md.
 */
#ifndef NXU_KERN_VM_ARCH_ARM64_PMAP_TYPES_H
#define NXU_KERN_VM_ARCH_ARM64_PMAP_TYPES_H

#include <stdint.h>

typedef struct pmap {
	/* TTBR1 direct-map pointer to the L1 translation-table root (kernel-side
	 * pointer used to edit the tables). */
	uint64_t *root;
	/* Physical address written to TTBR0_EL1 when this pmap is activated. */
	uint64_t  root_physical;
	/* Count of translation tables this pmap owns (accounting only). */
	uint64_t  table_count;
} pmap_t;

#endif /* NXU_KERN_VM_ARCH_ARM64_PMAP_TYPES_H */
