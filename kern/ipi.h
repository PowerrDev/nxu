/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipi.h
 *
 * Inter-processor interrupts: how one CPU makes another do something now. The
 * machine layer (machine_ipi_raise, <kern/machine/smp.h>) says how a vector is
 * delivered (a GICv3 SGI on arm64); this says what the vectors mean.
 *
 *   IPI_RESCHEDULE   Set the target's need_resched (processor.preemption_pending)
 *                    and, being an interrupt, wake it from WFI. The target then
 *                    enters the scheduler at its next safe point (the return
 *                    from this very interrupt, when it was idle or running
 *                    preemptible code). Sent when a thread better than the
 *                    target's current one is queued on it, or when the target
 *                    is idle.
 *   IPI_CPU_STOP     Mask interrupts and halt for good. Sent by the CPU that is
 *                    panicking so the others stop touching memory and the
 *                    console while it reports.
 *
 * There is no TLB IPI: the arm64 TLB invalidations the kernel uses are the
 * inner-shareable forms, which the hardware broadcasts and completes with a
 * DSB, so no CPU has to be interrupted to drop its entries (see vm/vmm.c).
 *
 * An IPI is only a doorbell. Whatever the sender wants the target to see is
 * written to memory first, and the machine layer orders that store before the
 * interrupt (DSB ISHST before the SGI is generated). The handler runs in
 * interrupt context on the target and must not block.
 */
#ifndef NXU_KERN_IPI_H
#define NXU_KERN_IPI_H

#include <kern/machine/smp.h>

#include <stdint.h>

typedef enum {
	IPI_RESCHEDULE = 0,
	IPI_CPU_STOP = 1,
	IPI_TYPE_COUNT
} ipi_type_t;

/* Interrupt CPU `cpu` (a logical id) with `type`. Never call it for the calling CPU. */
static inline void ipi_send(uint32_t cpu, ipi_type_t type)
{
	machine_ipi_raise(cpu, (uint32_t)type);
}

/* Interrupt every CPU but the caller. */
static inline void ipi_send_others(ipi_type_t type)
{
	machine_ipi_raise_others((uint32_t)type);
}

#endif
