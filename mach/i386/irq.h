/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/irq.h
 *
 * Interrupt bring-up and dispatch glue for the legacy PC interrupt path:
 *
 *   device -> 8259 PIC -> vector 32+n -> i386_trap_handler -> i386_trap_irq
 *          -> irq_dispatch(n) -> handlers chained by irq_register(n, ...)
 *
 * The interrupt ID handed to irq_register() and irq_dispatch() is the PIC
 * line, 0..15. Handlers run with IF clear (interrupt gates) and must follow
 * the rules in <kern/irq/irq.h>.
 *
 * Expectations on the rest of the kernel (all optional; see irq.c):
 *
 *   void sched_tick(void)      Weak. Called from the IRQ0 handler after the
 *                              tick is counted, in interrupt context with IF
 *                              clear. It only accounts and requests
 *                              preemption; the switch itself belongs on the
 *                              way out of the trap (i386_trap_exit), as in
 *                              arm64's exception_handle().
 *
 * The IRQ0 tick is not started with IF set: i386_init_interrupts() leaves
 * interrupts disabled, and whoever finishes bring-up (the threads phase)
 * enables them once the scheduler can take a tick.
 */

#ifndef NXU_MACH_I386_IRQ_H
#define NXU_MACH_I386_IRQ_H

#include <stdint.h>

/* IRQ line of the PIT, and the periodic rate started at boot (as arm64). */
#define I386_TIMER_IRQ 0U
#define I386_TIMER_HZ 100U

/* IRQ line of the CMOS real-time clock, on the slave PIC. */
#define I386_RTC_IRQ 8U

/* Hardware interrupts that arrived with no handler registered. */
uint32_t i386_irq_unhandled_count(void);

#endif
