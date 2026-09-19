/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/vm_fault.h
 *
 * Page-fault support behind the strong i386_trap_page_fault hook.
 *
 * Nothing in the kernel resolves page faults yet (mappings are eager), so
 * every #PF is a bug and the hook's job is to describe it: which part of the
 * address map the address is in and what the page tables say, printed just
 * before the trap layer's own panic report.
 *
 * The one exception is a *expected* fault, armed by the VM self-test around a
 * single instruction that must fault (a write to a read-only page, a read of
 * an unmapped one). When the fault address and instruction match, the hook
 * moves the frame's eip to a resume label and reports the fault handled, so a
 * boot can prove that the hardware really enforces the protections it set up
 * and carry on.
 */

#ifndef NXU_MACH_I386_VM_FAULT_H
#define NXU_MACH_I386_VM_FAULT_H

#include <mach/i386/trap.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t armed;
	uint32_t address;
	uint32_t fault_eip;
	uint32_t resume_eip;

	/* Filled in when the expected fault happens. */
	uint32_t hits;
	uint32_t error_code;
} i386_vm_fault_expectation_t;

extern volatile i386_vm_fault_expectation_t i386_vm_fault_expectation;

/*
 * Extension point for a real fault handler (demand paging, copy on write,
 * stack growth). i386_trap_page_fault calls it, after the expected-fault
 * check, for every fault from either ring; return true if the fault was
 * resolved and the instruction should be retried. The weak default resolves
 * nothing. A subsystem that wants to handle faults defines the strong
 * version in its own file, so the trap hook itself has one owner.
 */
bool i386_vm_fault_resolve(x86_saved_state_t *state);

/* Which part of the address map `address` belongs to, for reports. */
const char *i386_vm_region_name(uint32_t address);

#endif
