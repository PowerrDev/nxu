/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/irq.c
 *
 * Interrupts phase of the i386 boot and the strong i386_trap_irq. See irq.h
 * for the path an interrupt takes and what is expected from other areas.
 */

#include <kern/i386/irq.h>

#include <kern/i386/boot_info.h>
#include <kern/i386/pic.h>
#include <kern/i386/timer.h>
#include <kern/i386/trap.h>

#include <kern/machine/machine_routines.h>

#include <kern/console/console.h>
#include <kern/irq/irq.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Scheduler entry point (kern/sched_prism/sched.h). Weak so this area builds
 * and boots without the scheduler linked; the tick is then only counted.
 */
__attribute__((weak)) void sched_tick(void);

static volatile uint32_t g_irq_unhandled_count;

uint32_t i386_irq_unhandled_count(void)
{
	return g_irq_unhandled_count;
}

/*
 * IRQ0 handler: what arm64's irq_handle() does for the physical timer,
 * count the tick and then charge the scheduler.
 */
static void i386_timer_irq(uint32_t intid, void *context)
{
	(void)intid;
	(void)context;

	timer_handle_interrupt();

	if (sched_tick != 0) sched_tick();
}

bool i386_trap_irq(x86_saved_state_t *state)
{
	if (state->trapno < T_IRQ_BASE || state->trapno >= T_IRQ_BASE + T_IRQ_COUNT) return false;

	uint32_t irq = state->trapno - T_IRQ_BASE;

	/*
	 * Vectors 39 and 47 double as the controller's "the interrupt went
	 * away" answer. A spurious one is not in service, so it must not be
	 * acknowledged and no handler may run for it.
	 */
	if (pic_irq_is_spurious(irq)) return true;

	bool handled = irq_dispatch(irq);

	if (!handled) {
		g_irq_unhandled_count++;
		kprintf("i386_trap_irq: no handler for IRQ %u\n", irq);
	}

	/*
	 * Acknowledge after the handlers, as arm64 does, so the line stays in
	 * service while they run. IF is clear throughout, so nothing nests.
	 */
	pic_eoi(irq);
	return handled;
}

bool i386_init_interrupts(const i386_boot_info_t *boot)
{
	(void)boot;

	ml_irq_disable();

	pic_init();

	if (!irq_init()) {
		kputln("i386_init_interrupts: irq_init failed");
		return false;
	}

	if (!irq_register(I386_TIMER_IRQ, i386_timer_irq, 0)) {
		kputln("i386_init_interrupts: cannot register the timer handler");
		return false;
	}

	kprintf(
		"i386_init_interrupts: pic remapped to vectors %u-%u, all lines masked\n",
		T_IRQ_BASE,
		T_IRQ_BASE + T_IRQ_COUNT - 1U
	);

	/* Also opens IRQ0 at the PIC. Nothing is delivered until IF is set. */
	timer_start_periodic(I386_TIMER_HZ);

	kprintf(
		"i386_init_interrupts: pit channel 0 armed at %u Hz on IRQ %u, interrupts disabled\n",
		timer_get_interrupt_rate(),
		I386_TIMER_IRQ
	);

	return true;
}
