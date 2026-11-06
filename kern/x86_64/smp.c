/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/x86_64/smp.c
 *
 * Single-CPU x86-64 inter-processor interrupt stubs. This port boots one CPU
 * and has no APIC bring-up yet, so there is never another CPU to interrupt.
 * The machine-layer IPI entry points (declared in <kern/x86_64/smp.h> and used
 * through <kern/ipi.h>) are provided here as no-ops so machine-independent
 * code that may kick other CPUs -- the VM core's address-space quiesce, for
 * one -- links and runs correctly on a uniprocessor. They must be implemented
 * for real when this port gains the APIC and secondary-CPU start-up.
 */

#include <kern/x86_64/smp.h>

#include <stdint.h>

/*
 * Routine:     machine_ipi_raise
 * Purpose:
 *              Interrupt one other logical CPU. No-op on this single-CPU port
 *              (the only caller skips the calling CPU, and there is no other).
 */
void
machine_ipi_raise(uint32_t cpu, uint32_t vector)
{
	(void)cpu;
	(void)vector;
}

/*
 * Routine:     machine_ipi_raise_others
 * Purpose:
 *              Interrupt every other logical CPU. No-op on this single-CPU
 *              port.
 */
void
machine_ipi_raise_others(uint32_t vector)
{
	(void)vector;
}
