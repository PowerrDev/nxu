/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/trap.c
 *
 * Decodes long-mode exceptions and reports the complete saved processor
 * state before stopping on an unrecoverable kernel event.
 */

#include <kern/x86_64/trap.h>

#include <kern/console/console.h>
#include <kern/machine/vm_param.h>

#include <stdbool.h>
#include <stdint.h>

#define X86_TRAP_BACKTRACE_DEPTH 16U

static const char *const g_x86_exception_names[X86_EXCEPTION_COUNT] = {
	"#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint", "#OF Overflow", "#BR Bound Range",
	"#UD Invalid Opcode", "#NM Device Not Available", "#DF Double Fault", "Coprocessor Segment Overrun",
	"#TS Invalid TSS", "#NP Segment Not Present", "#SS Stack-Segment Fault", "#GP General Protection",
	"#PF Page Fault", "Reserved", "#MF x87 Floating-Point", "#AC Alignment Check", "#MC Machine Check",
	"#XM SIMD Floating-Point", "#VE Virtualization", "#CP Control Protection", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved", "#VC VMM Communication", "#SX Security Exception", "Reserved",
};

static volatile uint64_t g_x86_breakpoint_count;
static volatile bool g_x86_trap_panicking;
static volatile bool g_x86_page_fault_probe_armed;
static volatile bool g_x86_page_fault_probe_seen;

extern uint8_t x86_trap_page_fault_instruction[];
extern uint8_t x86_trap_page_fault_resume[];
extern void x86_trap_page_fault_probe(void);

/*
 * Routine:     x86_trap_name
 * Purpose:
 *              Return the architectural exception name or identify an
 *              external vector outside the processor exception range.
 */
static const char *
x86_trap_name(uint64_t vector)
{
	if (vector < X86_EXCEPTION_COUNT) {
		return g_x86_exception_names[vector];
	}

	if (vector == X86_TRAP_SPURIOUS) {
		return "Local APIC spurious interrupt";
	}

	return "external interrupt or software vector";
}

/*
 * Routine:     x86_trap_has_error_code
 * Purpose:
 *              Identify exceptions for which long mode places an error code
 *              on the hardware stack before entering the assembly stub.
 */
static bool
x86_trap_has_error_code(uint64_t vector)
{
	return vector == 8ULL || (vector >= 10ULL && vector <= 14ULL) || vector == 17ULL || vector == 21ULL ||
		vector == 29ULL || vector == 30ULL;
}

/*
 * Routine:     x86_read_cr0
 * Purpose:
 *              Read CR0 for inclusion in an unrecoverable exception report.
 */
static uintptr_t
x86_read_cr0(void)
{
	uintptr_t value;
	__asm__ volatile("mov %%cr0, %0" : "=r"(value));
	return value;
}

/*
 * Routine:     x86_read_cr2
 * Purpose:
 *              Capture the faulting linear address before diagnostic output
 *              can trigger another exception.
 */
static uintptr_t
x86_read_cr2(void)
{
	uintptr_t value;
	__asm__ volatile("mov %%cr2, %0" : "=r"(value));
	return value;
}

/*
 * Routine:     x86_read_cr3
 * Purpose:
 *              Read the active page-table root for an exception report.
 */
static uintptr_t
x86_read_cr3(void)
{
	uintptr_t value;
	__asm__ volatile("mov %%cr3, %0" : "=r"(value));
	return value;
}

/*
 * Routine:     x86_read_cr4
 * Purpose:
 *              Read active processor paging features for an exception report.
 */
static uintptr_t
x86_read_cr4(void)
{
	uintptr_t value;
	__asm__ volatile("mov %%cr4, %0" : "=r"(value));
	return value;
}

/*
 * Routine:     x86_read_cr8
 * Purpose:
 *              Read the task-priority register for an interrupt report.
 */
static uintptr_t
x86_read_cr8(void)
{
	uintptr_t value;
	__asm__ volatile("mov %%cr8, %0" : "=r"(value));
	return value;
}

/*
 * Routine:     x86_trap_error_description
 * Purpose:
 *              Decode the page-fault access type and architectural status
 *              bits into individual diagnostics.
 */
static void
x86_trap_error_description(uint64_t error, uintptr_t fault_address)
{
	kprintf("x86_trap_handler: page address 0x%llx\n", (unsigned long long)fault_address);
	kprintf("x86_trap_handler: access %s by %s code\n",
			(error & X86_PF_INSTRUCTION) != 0ULL ? "instruction fetch" :
			((error & X86_PF_WRITE) != 0ULL ? "write" : "read"),
			(error & X86_PF_USER) != 0ULL ? "user" : "supervisor");
	kprintf("x86_trap_handler: page status %s\n",
			(error & X86_PF_PRESENT) != 0ULL ? "protection violation" : "not present");

	if ((error & X86_PF_RESERVED) != 0ULL) {
		kputln("x86_trap_handler: reserved paging-structure bit was set");
	}
	if ((error & X86_PF_PROTECTION_KEY) != 0ULL) {
		kputln("x86_trap_handler: protection-key violation");
	}
	if ((error & X86_PF_SHADOW_STACK) != 0ULL) {
		kputln("x86_trap_handler: shadow-stack access violation");
	}
	if ((error & X86_PF_SGX) != 0ULL) {
		kputln("x86_trap_handler: SGX access violation");
	}
}

/*
 * Routine:     x86_trap_selector_description
 * Purpose:
 *              Decode the external, IDT, LDT, and GDT selector fields used
 *              by invalid-TSS, segment-not-present, stack, and GP faults.
 */
static void
x86_trap_selector_description(uint64_t error)
{
	if ((error & 0xFFFCULL) == 0ULL) {
		kputln("x86_trap_handler: selector fault has no selector index");
		return;
	}

	kprintf("x86_trap_handler: selector index %llu in %s\n",
			(unsigned long long)((error >> 3U) & 0x1FFFULL),
			(error & 0x02ULL) != 0ULL ? "IDT" : ((error & 0x04ULL) != 0ULL ? "LDT" : "GDT"));
	kprintf("x86_trap_handler: selector fault %s\n",
			(error & 0x01ULL) != 0ULL ? "was raised by an external event" : "was raised internally");
}

/*
 * Routine:     x86_trap_print_flags
 * Purpose:
 *              List the architecturally useful RFLAGS bits saved by the
 *              processor on exception entry.
 */
static void
x86_trap_print_flags(uint64_t flags)
{
	static const struct {
		uint64_t bit;
		const char *name;
	} names[] = {
		{1ULL << 0U, "CF"}, {1ULL << 2U, "PF"}, {1ULL << 4U, "AF"}, {1ULL << 6U, "ZF"},
		{1ULL << 7U, "SF"}, {1ULL << 8U, "TF"}, {1ULL << 9U, "IF"}, {1ULL << 10U, "DF"},
		{1ULL << 11U, "OF"}, {1ULL << 14U, "NT"}, {1ULL << 16U, "RF"}, {1ULL << 17U, "VM"},
		{1ULL << 18U, "AC"}, {1ULL << 21U, "ID"},
	};

	bool first = true;
	kputc('[');
	for (uint32_t index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
		if ((flags & names[index].bit) == 0ULL) {
			continue;
		}

		kprintf("%s%s", first ? "" : " ", names[index].name);
		first = false;
	}
	kputc(']');
}

/*
 * Routine:     x86_trap_halt
 * Purpose:
 *              Stop after an exception that cannot safely resume the
 *              interrupted instruction stream.
 */
static void __attribute__((noreturn))
x86_trap_halt(void)
{
	for (;;) {
		__asm__ volatile("cli; hlt" : : : "memory");
	}
}

/*
 * Routine:     x86_trap_panic
 * Purpose:
 *              Print the complete normalized register frame and control
 *              registers once, then halt after an unrecoverable trap.
 */
static void __attribute__((noreturn))
x86_trap_panic(const x86_trap_frame_t *frame, uintptr_t fault_address)
{
	if (g_x86_trap_panicking) {
		x86_trap_halt();
	}
	g_x86_trap_panicking = true;

	kputc('\n');
	kprintf("x86_trap_handler: fatal vector %llu %s\n", (unsigned long long)frame->vector,
			x86_trap_name(frame->vector));
	kprintf("x86_trap_handler: RIP 0x%016llx CS 0x%llx RFLAGS 0x%016llx ",
			(unsigned long long)frame->rip, (unsigned long long)frame->cs, (unsigned long long)frame->rflags);
	x86_trap_print_flags(frame->rflags);
	kputc('\n');
	kprintf("x86_trap_handler: RSP 0x%016llx SS 0x%llx\n", (unsigned long long)frame->rsp,
			(unsigned long long)frame->ss);

	if (x86_trap_has_error_code(frame->vector)) {
		kprintf("x86_trap_handler: error code 0x%016llx\n", (unsigned long long)frame->error_code);
	}
	if (frame->vector == X86_TRAP_PAGE_FAULT) {
		x86_trap_error_description(frame->error_code, fault_address);
	} else if (frame->vector >= 10ULL && frame->vector <= 13ULL) {
		x86_trap_selector_description(frame->error_code);
	}

	kprintf("x86_trap_handler: RAX 0x%016llx RBX 0x%016llx RCX 0x%016llx RDX 0x%016llx\n",
			(unsigned long long)frame->rax, (unsigned long long)frame->rbx, (unsigned long long)frame->rcx,
			(unsigned long long)frame->rdx);
	kprintf("x86_trap_handler: RSI 0x%016llx RDI 0x%016llx RBP 0x%016llx\n",
			(unsigned long long)frame->rsi, (unsigned long long)frame->rdi, (unsigned long long)frame->rbp);
	kprintf("x86_trap_handler: R8  0x%016llx R9  0x%016llx R10 0x%016llx R11 0x%016llx\n",
			(unsigned long long)frame->r8, (unsigned long long)frame->r9, (unsigned long long)frame->r10,
			(unsigned long long)frame->r11);
	kprintf("x86_trap_handler: R12 0x%016llx R13 0x%016llx R14 0x%016llx R15 0x%016llx\n",
			(unsigned long long)frame->r12, (unsigned long long)frame->r13, (unsigned long long)frame->r14,
			(unsigned long long)frame->r15);
	kprintf("x86_trap_handler: CR0 0x%016llx CR2 0x%016llx CR3 0x%016llx CR4 0x%016llx CR8 0x%llx\n",
			(unsigned long long)x86_read_cr0(), (unsigned long long)fault_address,
			(unsigned long long)x86_read_cr3(), (unsigned long long)x86_read_cr4(),
			(unsigned long long)x86_read_cr8());
	kputln("x86_trap_handler: system halted");

	x86_trap_halt();
}

/*
 * Routine:     x86_trap_handler
 * Purpose:
 *              Dispatch all 256 IDT vectors, resume breakpoints and the
 *              architectural spurious vector, and panic on unsupported events.
 */
void
x86_trap_handler(x86_trap_frame_t *frame)
{
	uintptr_t fault_address = x86_read_cr2();

	if (frame == 0 || frame->vector >= X86_IDT_VECTOR_COUNT) {
		x86_trap_halt();
	}

	if (frame->vector == X86_TRAP_BREAKPOINT) {
		g_x86_breakpoint_count++;
		kprintf("x86_trap_handler: breakpoint %llu at 0x%llx\n",
				(unsigned long long)g_x86_breakpoint_count, (unsigned long long)frame->rip);
		return;
	}

	if (frame->vector == X86_TRAP_PAGE_FAULT && g_x86_page_fault_probe_armed &&
		fault_address == 0xFFFFA00000000000ULL && frame->error_code == 0ULL &&
		frame->rip == (uint64_t)(uintptr_t)x86_trap_page_fault_instruction + VMM_HIGHER_HALF_BASE) {
		g_x86_page_fault_probe_seen = true;
		frame->rip = (uint64_t)(uintptr_t)x86_trap_page_fault_resume + VMM_HIGHER_HALF_BASE;
		return;
	}

	if (frame->vector == X86_TRAP_SPURIOUS) {
		return;
	}

	x86_trap_panic(frame, fault_address);
}

/*
 * Routine:     x86_trap_self_test
 * Purpose:
 *              Verify one breakpoint traverses the installed IDT, normalized
 *              stack frame, C handler, and IRETQ return path successfully.
 */
bool
x86_trap_self_test(void)
{
	uint64_t before = g_x86_breakpoint_count;
	__asm__ volatile("int3" : : : "memory");
	if (g_x86_breakpoint_count != before + 1ULL) {
		return false;
	}

	g_x86_page_fault_probe_seen = false;
	g_x86_page_fault_probe_armed = true;
	x86_trap_page_fault_probe();
	g_x86_page_fault_probe_armed = false;

	return g_x86_page_fault_probe_seen;
}
