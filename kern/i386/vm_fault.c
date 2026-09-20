/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/vm_fault.c
 *
 * See vm_fault.h.
 */

#include <kern/i386/vm_fault.h>

#include <kern/i386/pmap.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

extern uint8_t __kernel_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_end[];
extern uint8_t __kernel_end[];

volatile i386_vm_fault_expectation_t i386_vm_fault_expectation;

const char *i386_vm_region_name(uint32_t address)
{
	if (address < PMAP_PAGE_SIZE) return "null guard page";
	if (address < VM_MAX_USER_ADDRESS) return "user space";

	if (address >= (uintptr_t)__kernel_start && address < (uintptr_t)__text_end) return "kernel text";
	if (address >= (uintptr_t)__text_end && address < (uintptr_t)__rodata_end) return "kernel rodata";
	if (address >= (uintptr_t)__rodata_end && address < (uintptr_t)__kernel_end) return "kernel data/bss";

	if (address < VMM_HIGHER_HALF_BASE + VM_DIRECT_MAP_SIZE) return "kernel direct map";
	if (address >= VM_KERN_BASE && (uint64_t)address < (uint64_t)VM_KERN_BASE + VM_KERN_SIZE) return "vm_kern arena";
	if (address >= VM_MMIO_BASE) return "mmio window";

	return "reserved kernel range";
}

/* Read the live page tables the way the MMU would, without faulting. */
static void vm_fault_describe_tables(uint32_t address)
{
	uint32_t cr3 = pmap_current_directory_physical();
	uint32_t directory_entry = pmap_table_pointer(cr3)[pmap_directory_index(address)];

	kprintf("i386_trap_page_fault: cr3 0x%x, pde 0x%x", cr3, directory_entry);

	if ((directory_entry & PTE_PRESENT) == 0U) {
		kputln(" (page table absent)");
		return;
	}

	if ((directory_entry & PTE_LARGE) != 0U) {
		kputln(" (4 MiB page)");
		return;
	}

	uint32_t entry = pmap_table_pointer(directory_entry & PTE_FRAME)[pmap_table_index(address)];

	kprintf(", pte 0x%x", entry);

	if ((entry & PTE_PRESENT) == 0U) {
		kputln(" (page not present)");
		return;
	}

	kprintf(" (%s, %s)\n", (entry & PTE_WRITE) != 0U ? "writable" : "read-only", (entry & PTE_USER) != 0U ? "user" : "supervisor");
}

__attribute__((weak)) bool i386_vm_fault_resolve(x86_saved_state_t *state)
{
	(void)state;
	return false;
}

bool i386_trap_page_fault(x86_saved_state_t *state)
{
	volatile i386_vm_fault_expectation_t *expected = &i386_vm_fault_expectation;

	if (expected->armed != 0U && state->cr2 == expected->address && state->eip == expected->fault_eip) {
		expected->armed = 0U;
		expected->hits++;
		expected->error_code = state->err;
		state->eip = expected->resume_eip;
		return true;
	}

	if (i386_vm_fault_resolve(state)) return true;

	/*
	 * A fault from ring 3 is the owner of the process's business (the
	 * thread layer decides whether it kills the task); only a fault in the
	 * kernel itself is described here, right before the panic report.
	 */
	if (x86_saved_state_is_user(state)) return false;

	kprintf(
		"i386_trap_page_fault: unresolved fault at 0x%x in %s, eip 0x%x\n",
		state->cr2,
		i386_vm_region_name(state->cr2),
		state->eip
	);
	vm_fault_describe_tables(state->cr2);

	/* Not ours to fix: let the trap layer print its report and stop. */
	return false;
}
