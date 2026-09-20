/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/trap.c
 *
 * Central trap handler for 32-bit x86: dispatches the vectors that have a
 * meaning today (breakpoint, the system-call gate), and turns everything
 * else into a full kernel panic report. Also the boot-time self-test and
 * the deliberate-fault hooks used to exercise the report paths.
 */

#include <kern/i386/trap.h>

#include <kern/i386/gdt.h>
#include <kern/i386/io.h>

#include <kern/console/console.h>
#include <string.h>

#include <stdbool.h>
#include <stdint.h>

/* QEMU's isa-debug-exit device, when present, turns this write into an exit. */
#define QEMU_DEBUG_EXIT_PORT 0xF4U

#define TRAP_BACKTRACE_DEPTH 16U

extern uint8_t i386_boot_stack_bottom[];
extern uint8_t i386_boot_stack_top[];

static const char *const g_exception_names[T_EXCEPTION_COUNT] = {
	"#DE Divide Error",
	"#DB Debug",
	"NMI",
	"#BP Breakpoint",
	"#OF Overflow",
	"#BR Bound Range Exceeded",
	"#UD Invalid Opcode",
	"#NM Device Not Available",
	"#DF Double Fault",
	"Coprocessor Segment Overrun",
	"#TS Invalid TSS",
	"#NP Segment Not Present",
	"#SS Stack-Segment Fault",
	"#GP General Protection Fault",
	"#PF Page Fault",
	"reserved",
	"#MF x87 Floating-Point Error",
	"#AC Alignment Check",
	"#MC Machine Check",
	"#XM SIMD Floating-Point Exception",
	"#VE Virtualization Exception",
	"#CP Control Protection",
	"reserved",
	"reserved",
	"reserved",
	"reserved",
	"reserved",
	"reserved",
	"#HV Hypervisor Injection",
	"#VC VMM Communication",
	"#SX Security Exception",
	"reserved"
};

static volatile uint32_t g_breakpoint_count;
static volatile uint32_t g_last_breakpoint_eip;
static volatile uint32_t g_last_breakpoint_cs;
static volatile uint32_t g_syscall_count;

static bool g_in_panic;

static inline uint32_t trap_read_cr0(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr0, %0" : "=r"(value));
	return value;
}

static inline uint32_t trap_read_cr3(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr3, %0" : "=r"(value));
	return value;
}

static inline uint32_t trap_read_cr4(void)
{
	uint32_t value;

	__asm__ volatile("movl %%cr4, %0" : "=r"(value));
	return value;
}

static void trap_halt(void) __attribute__((noreturn));

static void trap_halt(void)
{
	/* Ends a QEMU run started with isa-debug-exit; harmless if absent. */
	outb(QEMU_DEBUG_EXIT_PORT, 0x01U);

	for (;;) {
		__asm__ volatile("cli\n\thlt");
	}
}

static const char *trap_exception_name(uint32_t vector)
{
	if (vector < T_EXCEPTION_COUNT) return g_exception_names[vector];
	if (vector >= T_IRQ_BASE && vector < T_IRQ_BASE + T_IRQ_COUNT) return "hardware interrupt";
	if (vector == T_SYSCALL) return "system call";
	return "unknown vector";
}

static bool trap_has_error_code(uint32_t vector)
{
	return vector == T_DOUBLE_FAULT ||
		(vector >= T_INVALID_TSS && vector <= T_PAGE_FAULT) ||
		vector == T_ALIGNMENT_CHECK;
}

static void trap_print_flags(uint32_t efl)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} flags[] = {
		{ 1U << 0U, "CF" }, { 1U << 2U, "PF" }, { 1U << 4U, "AF" },
		{ 1U << 6U, "ZF" }, { 1U << 7U, "SF" }, { 1U << 8U, "TF" },
		{ 1U << 9U, "IF" }, { 1U << 10U, "DF" }, { 1U << 11U, "OF" },
		{ 1U << 14U, "NT" }, { 1U << 16U, "RF" }, { 1U << 17U, "VM" },
		{ 1U << 18U, "AC" }
	};

	kputs(" [");

	bool first = true;

	for (uint32_t index = 0U; index < sizeof(flags) / sizeof(flags[0]); index++) {
		if ((efl & flags[index].bit) == 0U) continue;

		if (!first) kputc(' ');
		kputs(flags[index].name);
		first = false;
	}

	kputc(']');
}

/* Decode the selector-style error code #TS/#NP/#SS/#GP push. */
static void trap_print_selector_error(uint32_t err)
{
	if (err == 0U) {
		kputln("  cause: not selector-related (error code 0)");
		return;
	}

	kprintf(
		"  cause: selector 0x%x, %s, index %u%s\n",
		err & 0xFFFFU,
		(err & 2U) != 0U ? "IDT" : ((err & 4U) != 0U ? "LDT" : "GDT"),
		(err >> 3U) & 0x1FFFU,
		(err & 1U) != 0U ? ", raised by an external event" : ""
	);
}

static void trap_print_page_fault(uint32_t err, uint32_t address)
{
	kprintf(
		"  cause: %s of 0x%x by %s: %s%s\n",
		(err & PF_FETCH) != 0U ? "instruction fetch" : ((err & PF_WRITE) != 0U ? "write" : "read"),
		address,
		(err & PF_USER) != 0U ? "user code" : "kernel code",
		(err & PF_PRESENT) != 0U ? "protection violation" : "page not present",
		(err & PF_RESERVED) != 0U ? ", reserved bit set in a paging structure" : ""
	);
}

static void trap_print_backtrace(uint32_t eip, uint32_t ebp)
{
	uintptr_t low = (uintptr_t)i386_boot_stack_bottom;
	uintptr_t high = (uintptr_t)i386_boot_stack_top;

	kprintf("  backtrace: #0 0x%x", eip);

	uintptr_t frame = ebp;

	for (uint32_t depth = 1U; depth < TRAP_BACKTRACE_DEPTH; depth++) {
		/* Only walk frames that provably lie inside the boot stack. */
		if (frame < low || frame + 8U > high || (frame & 3U) != 0U) break;

		const uint32_t *words = (const uint32_t *)frame;

		if (words[1] == 0U) break;
		kprintf(" #%u 0x%x", depth, words[1]);

		if (words[0] <= frame) break;
		frame = words[0];
	}

	kputc('\n');
}

/*
 * Print the full fault report and stop the machine. `esp` is passed
 * separately because where the interrupted stack pointer lives depends on
 * the path (frame layout for an ordinary trap, saved TSS for #DF).
 */
static void trap_panic(const x86_saved_state_t *state, uint32_t esp, const char *how) __attribute__((noreturn));

static void trap_panic(const x86_saved_state_t *state, uint32_t esp, const char *how)
{
	if (g_in_panic) trap_halt();
	g_in_panic = true;

	uint32_t vector = state->trapno;

	kputc('\n');
	kprintf(
		"panic: %s %u (%s) at 0x%x:0x%x%s\n",
		how,
		vector,
		trap_exception_name(vector),
		state->cs & 0xFFFFU,
		state->eip,
		x86_saved_state_is_user(state) ? " (user mode)" : ""
	);

	if (trap_has_error_code(vector)) {
		kprintf("  error code 0x%x\n", state->err);

		if (vector == T_INVALID_TSS || vector == T_SEGMENT_NOT_PRESENT ||
			vector == T_STACK_FAULT || vector == T_GENERAL_PROTECTION) {
			trap_print_selector_error(state->err);
		} else if (vector == T_PAGE_FAULT) {
			trap_print_page_fault(state->err, state->cr2);
		}
	}

	kprintf("  eax=");
	kputhex32(state->eax);
	kputs(" ebx=");
	kputhex32(state->ebx);
	kputs(" ecx=");
	kputhex32(state->ecx);
	kputs(" edx=");
	kputhex32(state->edx);
	kputc('\n');

	kputs("  esi=");
	kputhex32(state->esi);
	kputs(" edi=");
	kputhex32(state->edi);
	kputs(" ebp=");
	kputhex32(state->ebp);
	kputs(" esp=");
	kputhex32(esp);
	kputc('\n');

	kputs("  eip=");
	kputhex32(state->eip);
	kputs(" efl=");
	kputhex32(state->efl);
	trap_print_flags(state->efl);
	kputc('\n');

	/* A ring-0 frame carries no ss slot, so read the live one. */
	uint32_t ss = state->ss;

	if (!x86_saved_state_is_user(state)) {
		__asm__ volatile("movl %%ss, %0" : "=r"(ss));
	}

	kprintf(
		"  cs=0x%x ss=0x%x ds=0x%x es=0x%x fs=0x%x gs=0x%x\n",
		state->cs & 0xFFFFU,
		ss & 0xFFFFU,
		state->ds & 0xFFFFU,
		state->es & 0xFFFFU,
		state->fs & 0xFFFFU,
		state->gs & 0xFFFFU
	);

	kputs("  cr0=");
	kputhex32(trap_read_cr0());
	kputs(" cr2=");
	kputhex32(state->cr2);
	kputs(" cr3=");
	kputhex32(trap_read_cr3());
	kputs(" cr4=");
	kputhex32(trap_read_cr4());
	kputc('\n');

	trap_print_backtrace(state->eip, state->ebp);
	kputln("panic: halting");

	trap_halt();
}

__attribute__((weak)) bool i386_trap_irq(x86_saved_state_t *state)
{
	(void)state;
	return false;
}

__attribute__((weak)) void i386_trap_syscall(x86_saved_state_t *state)
{
	/*
	 * No dispatcher yet. Returning a recognisable error through the frame
	 * proves the register write-back path.
	 */
	g_syscall_count++;
	state->eax = I386_SYSCALL_UNIMPLEMENTED;
}

__attribute__((weak)) bool i386_trap_user_exception(x86_saved_state_t *state)
{
	(void)state;
	return false;
}

__attribute__((weak)) bool i386_trap_page_fault(x86_saved_state_t *state)
{
	(void)state;
	return false;
}

__attribute__((weak)) void i386_trap_exit(x86_saved_state_t *state)
{
	(void)state;
}

void i386_trap_handler(x86_saved_state_t *state)
{
	uint32_t vector = state->trapno;

	if (vector == T_BREAKPOINT && !x86_saved_state_is_user(state)) {
		/* A kernel breakpoint resumes after the int3; nothing is wrong. */
		g_breakpoint_count++;
		g_last_breakpoint_eip = state->eip;
		g_last_breakpoint_cs = state->cs;
		i386_trap_exit(state);
		return;
	}

	if (vector == T_SYSCALL) {
		i386_trap_syscall(state);
		i386_trap_exit(state);
		return;
	}

	if (vector >= T_IRQ_BASE && vector < T_IRQ_BASE + T_IRQ_COUNT) {
		if (i386_trap_irq(state)) {
			i386_trap_exit(state);
			return;
		}
	} else if (vector < T_EXCEPTION_COUNT) {
		if (vector == T_PAGE_FAULT && i386_trap_page_fault(state)) {
			i386_trap_exit(state);
			return;
		}

		if (x86_saved_state_is_user(state) && i386_trap_user_exception(state)) {
			i386_trap_exit(state);
			return;
		}
	}

	/*
	 * A frame from ring 0 has no uesp/ss slots: the CPU pushed nothing
	 * there, so the interrupted esp is the address just above the pushed
	 * eflags, i.e. where uesp would have been.
	 */
	uint32_t esp = x86_saved_state_is_user(state) ? state->uesp : (uint32_t)(uintptr_t)&state->uesp;

	trap_panic(state, esp, vector < T_EXCEPTION_COUNT ? "unhandled exception" : "unexpected interrupt vector");
}

void i386_double_fault_task(void)
{
	/*
	 * The task switch stored the context that faulted in the main TSS. Recast
	 * it as a saved-state frame so the ordinary report code can print it.
	 */
	const i386_tss_t *saved = i386_gdt_main_tss();
	x86_saved_state_t state;

	state.gs = saved->gs;
	state.fs = saved->fs;
	state.es = saved->es;
	state.ds = saved->ds;
	state.edi = saved->edi;
	state.esi = saved->esi;
	state.ebp = saved->ebp;
	state.ebx = saved->ebx;
	state.edx = saved->edx;
	state.ecx = saved->ecx;
	state.eax = saved->eax;
	state.trapno = T_DOUBLE_FAULT;
	state.err = 0U;
	state.eip = saved->eip;
	state.cs = saved->cs;
	state.efl = saved->eflags;
	state.uesp = saved->esp;
	state.ss = saved->ss;

	__asm__ volatile("movl %%cr2, %0" : "=r"(state.cr2));

	trap_panic(&state, saved->esp, "unhandled exception");
}

bool i386_trap_self_test(void)
{
	/*
	 * int3 is the one exception that resumes, so it exercises the whole
	 * path -- stub, frame construction, handler, register restore, iret --
	 * without stopping the boot.
	 */
	uint32_t after = 0U;
	uint32_t before_count = g_breakpoint_count;

	__asm__ volatile(
		"int3\n"
		"1:\n\t"
		"movl $1b, %0"
		: "=r"(after)
		:
		: "memory"
	);

	if (g_breakpoint_count != before_count + 1U) return false;
	if (g_last_breakpoint_eip != after) return false;
	if (g_last_breakpoint_cs != GDT_KERNEL_CODE_SEL) return false;

	/*
	 * A system call through the DPL 3 gate: eax must come back rewritten by
	 * the handler while every other register the handler did not touch is
	 * restored exactly.
	 */
	uint32_t eax = 0x12345678U;
	uint32_t ecx = 0xC0DEC0DEU;
	uint32_t edx = 0xD00DD00DU;
	uint32_t esi = 0x51515151U;
	uint32_t edi = 0xD1D1D1D1U;

	__asm__ volatile(
		"int $0x80"
		: "+a"(eax), "+c"(ecx), "+d"(edx), "+S"(esi), "+D"(edi)
		:
		: "memory"
	);

	if (eax != I386_SYSCALL_UNIMPLEMENTED) return false;
	if (ecx != 0xC0DEC0DEU || edx != 0xD00DD00DU) return false;
	if (esi != 0x51515151U || edi != 0xD1D1D1D1U) return false;

	return true;
}

void i386_trap_test(const char *name)
{
	kprintf("i386_trap_test: raising %s\n", name);

	if (strcmp(name, "div0") == 0) {
		uint32_t dividend = 1U;
		uint32_t divisor = 0U;

		__asm__ volatile("div %2" : "+a"(dividend), "+d"(divisor) : "r"(divisor));
	} else if (strcmp(name, "ud2") == 0) {
		__asm__ volatile("ud2");
	} else if (strcmp(name, "gp") == 0) {
		/* A GDT selector far beyond the table limit. */
		__asm__ volatile("movw $0x1238, %%ax\n\tmovw %%ax, %%ds" : : : "eax");
	} else if (strcmp(name, "np") == 0) {
		/* An in-limit descriptor whose present bit is clear. */
		__asm__ volatile("movw $0x38, %%ax\n\tmovw %%ax, %%ds" : : : "eax");
	} else if (strcmp(name, "df") == 0) {
		/* Software interrupt through the #DF task gate. */
		__asm__ volatile("int $8");
	} else if (strcmp(name, "vector") == 0) {
		/* A vector with no gate raises #GP with an IDT-flagged error code. */
		__asm__ volatile("int $0x81");
	} else if (strcmp(name, "irq") == 0) {
		/* An IRQ vector nothing has claimed. */
		__asm__ volatile("int $33");
	} else {
		kprintf("i386_trap_test: unknown test \"%s\"\n", name);
		return;
	}

	kprintf("i386_trap_test: %s returned unexpectedly\n", name);
}
