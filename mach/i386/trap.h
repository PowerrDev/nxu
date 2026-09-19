/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/trap.h
 *
 * Trap numbers and the saved-state frame for 32-bit x86, modelled on XNU's
 * x86_saved_state32_t.
 *
 * Every exception, interrupt and system call enters through a per-vector
 * stub in trap_vectors.S that builds one x86_saved_state32_t on the kernel
 * stack and calls i386_trap_handler() with a pointer to it. The handler may
 * edit the frame (a system call stores its result in eax); the stub reloads
 * everything from it on the way out, so the frame is the single source of
 * truth for the interrupted context.
 */

#ifndef NXU_MACH_I386_TRAP_H
#define NXU_MACH_I386_TRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__i386__)
#error "mach/i386/trap.h: the x86_64 saved-state frame is not implemented yet"
#endif

/* Processor exception vectors. */
#define T_DIVIDE_ERROR 0U
#define T_DEBUG 1U
#define T_NMI 2U
#define T_BREAKPOINT 3U
#define T_OVERFLOW 4U
#define T_BOUND_RANGE 5U
#define T_INVALID_OPCODE 6U
#define T_DEVICE_NOT_AVAILABLE 7U
#define T_DOUBLE_FAULT 8U
#define T_INVALID_TSS 10U
#define T_SEGMENT_NOT_PRESENT 11U
#define T_STACK_FAULT 12U
#define T_GENERAL_PROTECTION 13U
#define T_PAGE_FAULT 14U
#define T_X87_FAULT 16U
#define T_ALIGNMENT_CHECK 17U
#define T_MACHINE_CHECK 18U
#define T_SIMD_FAULT 19U

#define T_EXCEPTION_COUNT 32U

/* Hardware IRQs land here once the interrupt controller is remapped. */
#define T_IRQ_BASE 32U
#define T_IRQ_COUNT 16U

#define T_SYSCALL 0x80U

/* Vectors with a stub in i386_isr_table: exceptions plus the 16 IRQs. */
#define I386_ISR_COUNT (T_IRQ_BASE + T_IRQ_COUNT)

/* Result an unimplemented system call leaves in eax. */
#define I386_SYSCALL_UNIMPLEMENTED 0xFFFFFFFFU

/* Page-fault error-code bits. */
#define PF_PRESENT (1U << 0U)
#define PF_WRITE (1U << 1U)
#define PF_USER (1U << 2U)
#define PF_RESERVED (1U << 3U)
#define PF_FETCH (1U << 4U)

/*
 * Saved state, lowest address first (the order the stub pushes it in, read
 * backwards). The hardware pushes eip, cs, efl and, only when the trap came
 * from ring 3, uesp and ss; the stub pushes err (a dummy 0 where the CPU
 * pushes none) and trapno, then the general registers and the segment
 * registers. The slot pushal reserves for esp is reused to carry cr2, as
 * XNU does, since the interrupted esp is recoverable from the frame itself.
 */
typedef struct x86_saved_state32 {
	uint32_t gs;
	uint32_t fs;
	uint32_t es;
	uint32_t ds;
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t cr2;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t trapno;
	uint32_t err;
	uint32_t eip;
	uint32_t cs;
	uint32_t efl;
	uint32_t uesp;
	uint32_t ss;
} x86_saved_state32_t;

/*
 * The stubs in trap_vectors.S hard-code these offsets; if the layout above
 * changes, they must change with it.
 */
_Static_assert(sizeof(x86_saved_state32_t) == 76U, "x86_saved_state32_t size changed");
_Static_assert(offsetof(x86_saved_state32_t, cr2) == 28U, "cr2 slot offset changed");
_Static_assert(offsetof(x86_saved_state32_t, trapno) == 48U, "trapno offset changed");
_Static_assert(offsetof(x86_saved_state32_t, eip) == 56U, "eip offset changed");

typedef x86_saved_state32_t x86_saved_state_t;

/* True if the trap interrupted ring 3, i.e. the frame carries uesp and ss. */
static inline bool x86_saved_state_is_user(const x86_saved_state_t *state)
{
	return (state->cs & 3U) == 3U;
}

/* Per-vector entry stubs, defined in trap_vectors.S. */
extern const uint32_t i386_isr_table[I386_ISR_COUNT];
void i386_isr_syscall(void);

/* Common C entry point for every stub. */
void i386_trap_handler(x86_saved_state_t *state);

/*
 * Extension points. Each has a weak default in trap.c so the trap layer works
 * on its own, and is overridden by the subsystem that owns the behaviour,
 * which keeps trap.c itself out of every area's diff.
 *
 *   i386_trap_irq      A hardware interrupt (vectors T_IRQ_BASE ..
 *                      T_IRQ_BASE + T_IRQ_COUNT - 1). Acknowledge the
 *                      controller, dispatch the handler. Return false if the
 *                      interrupt was not handled (it is then fatal).
 *   i386_trap_syscall  An int 0x80 from any ring. Read the request from the
 *                      frame, run it, and store the result in the frame.
 *   i386_trap_user_exception
 *                      A processor exception taken while the CPU was in
 *                      ring 3. Return true if it was handled (the frame is
 *                      resumed) or the offender was dealt with; false makes
 *                      it a fatal panic report as for a kernel fault.
 *   i386_trap_page_fault
 *                      A #PF from either ring. Return true if it was
 *                      resolved and the instruction should be retried.
 *   i386_trap_exit     Called last on every path that resumes the frame,
 *                      with interrupts still masked. Where preemption and
 *                      deferred work run, before returning to the
 *                      interrupted context.
 */
bool i386_trap_irq(x86_saved_state_t *state);
void i386_trap_syscall(x86_saved_state_t *state);
bool i386_trap_user_exception(x86_saved_state_t *state);
bool i386_trap_page_fault(x86_saved_state_t *state);
void i386_trap_exit(x86_saved_state_t *state);

/*
 * Body of the double-fault task. Entered by a hardware task switch, never
 * called; it reports the state saved in the main TSS and does not return.
 */
void i386_double_fault_task(void) __attribute__((noreturn));

/* Boot-time check that the trap path saves and restores state correctly. */
bool i386_trap_self_test(void);

/* Deliberately raise the named fault (see trap.c for the list). */
void i386_trap_test(const char *name);

#endif
