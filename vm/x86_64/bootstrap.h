/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/x86_64/bootstrap.h
 *
 * Declares the early x86-64 kernel-owned paging bootstrap.
 */
#ifndef NXU_VM_X86_64_BOOTSTRAP_H
#define NXU_VM_X86_64_BOOTSTRAP_H

#include <boot/handoff.h>

#include <stdbool.h>

/*
 * Routine:     x86_vm_bootstrap
 * Purpose:
 *              Replace the UEFI page-table root with NXU-owned identity maps
 *              for the low-memory kernel, boot handoff, and bootstrap stack.
 */
bool x86_vm_bootstrap(const struct nxu_boot_x86_64 *boot);

/*
 * Routine:     x86_vm_initialize
 * Purpose:
 *              Initialize physical memory, x86 kernel mappings, and the
 *              virtual kernel allocation arena from the UEFI handoff.
 */
bool x86_vm_initialize(const struct nxu_boot_x86_64 *boot);

#endif
