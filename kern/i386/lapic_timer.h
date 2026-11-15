/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/lapic_timer.h
 *
 * Each CPU's Local APIC timer as the event timer's hardware (etimer_hw_t,
 * kern/clock/etimer.h). It only programs the device: what the deadlines are
 * for is the event timer's business.
 *
 * Two ways to program it, chosen once at boot and used on every CPU:
 *
 *   TSC-deadline  the timer interrupts when this CPU's TSC reaches the value
 *                 written to IA32_TSC_DEADLINE: an absolute deadline, exact,
 *                 one MSR write to arm or disarm. Used when CPUID says the CPU
 *                 has it (leaf 1, ECX bit 24).
 *   one-shot      the timer counts down from an initial count at a rate
 *                 measured at boot against the TSC, and interrupts at zero. A
 *                 relative deadline, so arming converts; a 32-bit count limits
 *                 how far ahead it reaches (the event timer re-arms when it
 *                 fires short of the deadline).
 *
 * The mode is not trusted from CPUID alone: before it is used, the boot CPU
 * arms it for a short deadline and checks the interrupt arrives, on time and
 * not early (a hypervisor can advertise a mode it does not deliver). If
 * neither mode works, or the machine has no Local APIC, the caller keeps the
 * PIT and the event timer runs in periodic mode.
 *
 * The boot argument timer.mode=deadline|oneshot|pit forces a mode (or the
 * PIT), for tests and for machines whose timer misbehaves; timer.mode=auto,
 * the default, picks the best that works.
 */

#ifndef NXU_KERN_I386_LAPIC_TIMER_H
#define NXU_KERN_I386_LAPIC_TIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	LAPIC_TIMER_NONE,
	LAPIC_TIMER_TSC_DEADLINE,
	LAPIC_TIMER_ONESHOT,
} lapic_timer_mode_t;

/*
 * lapic_timer_init
 *
 * Boot CPU, interrupts enabled (the mode check needs them), after
 * clock_init and timer_call_init and before any secondary CPU starts: choose
 * and check a mode, then hand the boot CPU over from the PIT to its Local
 * APIC timer (the PIT's line is masked) with the event timer in deadline
 * mode. Returns false, having changed nothing, when no mode can be used: the
 * PIT stays and the caller sets up the periodic event timer.
 */
bool lapic_timer_init(void);

/*
 * lapic_timer_cpu_start
 *
 * Set up the calling CPU's timer in the chosen mode and start its event
 * timer. The boot CPU's runs from lapic_timer_init; each secondary calls it
 * once, early in its start-up, before it enables interrupts. Does nothing
 * when the PIT was kept.
 */
void lapic_timer_cpu_start(void);

/* The mode chosen, and its name for logs ("TSC-deadline", "one-shot", or "none"). */
lapic_timer_mode_t lapic_timer_mode(void);
const char *lapic_timer_mode_name(void);

/* How many CPUs have started their timer. */
uint32_t lapic_timer_cpus_ready(void);

/* One-shot mode: the timer's count rate in Hz (0 in any other mode). */
uint64_t lapic_timer_frequency(void);

#endif
