/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/vm/machine/pmap_types.h
 *
 * Compile-time architecture dispatch for the opaque pmap_t, mirroring the
 * existing <kern/machine/vm_param.h> pattern. Neutral code reaches this header
 * transitively through <kern/vm/pmap.h> (and vm/address_space.h, which embeds
 * pmap_t by value); it never includes an arch pmap_types.h directly.
 *
 * Only the arm64 backend exists in Phase 0. The i386 and x86_64 arms point at
 * headers that are added by their respective future backend phases; building
 * those targets before then fails loudly here, which is intended.
 */
#ifndef NXU_KERN_VM_MACHINE_PMAP_TYPES_H
#define NXU_KERN_VM_MACHINE_PMAP_TYPES_H

#if defined(__aarch64__)
#include <kern/vm/arch/arm64/pmap_types.h>
#elif defined(__i386__)
#include <kern/vm/arch/i386/pmap_types.h>
#elif defined(__x86_64__)
#include <kern/vm/arch/x86_64/pmap_types.h>
#else
#error "kern/vm/machine/pmap_types.h: unsupported target architecture"
#endif

#endif /* NXU_KERN_VM_MACHINE_PMAP_TYPES_H */
