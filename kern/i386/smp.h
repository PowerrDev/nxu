/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/smp.h
 *
 * i386 implementation of <kern/machine/smp.h>, and the secondary-CPU
 * bring-up interface (kern/i386/smp.c, apic.c, mp_table.c).
 *
 * i386 has nothing like arm64's TPIDR_EL1 (a register the hardware banks per
 * CPU for exactly this purpose): the per-CPU pointer instead lives at %fs:0,
 * where %fs is a fixed selector (GDT_PERCPU_SEL) that resolves to a
 * different base address on every CPU only because every CPU loads its own,
 * separate copy of the GDT -- see gdt.h's file comment for the whole
 * mechanism, including how it survives interrupts and context switches.
 */
#ifndef NXU_KERN_I386_SMP_H
#define NXU_KERN_I386_SMP_H

#include <stdbool.h>
#include <stdint.h>

static inline void *machine_cpu_local(void)
{
	void *value;

	__asm__ volatile("movl %%fs:0, %0" : "=r"(value));
	return value;
}

static inline void machine_cpu_local_set(void *local)
{
	__asm__ volatile("movl %0, %%fs:0" : : "r"(local) : "memory");
}

static inline uint32_t machine_cpu_id(void)
{
	const uint32_t *local = (const uint32_t *)machine_cpu_local();

	return local != 0 ? *local : 0U;
}

/*
 * This CPU's Local APIC id, as smp_cpu_mpidr()/machine_ipi_raise name a CPU
 * (see kern/ipi.h) -- CPUID's initial APIC id (leaf 1, EBX[31:24]), not a
 * read of the Local APIC's own ID register (apic_id(), apic.c): this is
 * called unconditionally by processor_register() (kern/sched_prism/
 * processor.c, always linked, even by test-i386-threads alone with no APIC
 * anywhere in the build), so it must not depend on apic.c being linked at
 * all. The two agree in practice (hardware does not reprogram the LAPIC ID
 * away from its CPUID-reported reset value); apic.c's own bring-up code
 * still reads the real register directly rather than relying on that.
 */
static inline uint64_t machine_cpu_mpidr(void)
{
	uint32_t eax = 1U;
	uint32_t ebx;

	/* cpuid overwrites eax too; "+a" models that instead of leaving it as a stale input constraint. */
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx) : : "ecx", "edx");
	return (uint64_t)(ebx >> 24U);
}

/*
 * machine_ipi_raise / machine_ipi_raise_others
 *
 * Deliver IPI vector `vector` (an ipi_type_t, see kern/ipi.h) to logical CPU
 * `cpu`, or to every CPU but this one. On i386 a vector is T_IPI_RESCHEDULE
 * or T_IPI_CPU_STOP (trap.h), sent as a fixed-vector Local APIC IPI.
 */
void machine_ipi_raise(uint32_t cpu, uint32_t vector);
void machine_ipi_raise_others(uint32_t vector);

/*
 * smp_handle_ipi
 *
 * The trap handler's entry for an acknowledged IPI vector (T_IPI_RESCHEDULE
 * or T_IPI_CPU_STOP), on the CPU it was sent to. Interrupt context.
 */
void smp_handle_ipi(uint32_t vector);

/*
 * smp_boot_secondaries
 *
 * Parse the MP configuration table, bring up every CPU it lists besides the
 * boot CPU with INIT-SIPI-SIPI, wait for each to report online, and leave it
 * idling in the scheduler. Called once, on the boot CPU, after the "threads"
 * phase (the scheduler, and paging, are both up by then -- see
 * kern/i386/smp.c's own file comment for exactly why that ordering).
 * Returns the number of CPUs online, boot CPU included.
 */
uint32_t smp_boot_secondaries(void);

/* CPUs the MP table lists (capped at NXU_MAX_CPUS), and CPUs that have reported online. */
uint32_t smp_cpu_count(void);
uint32_t smp_online_count(void);

/* The Local APIC id of logical CPU `cpu`, or 0 if there is no such CPU (0 is also a real APIC id, so range-check with smp_cpu_count() first). */
uint64_t smp_cpu_mpidr(uint32_t cpu);

/*
 * smp_stop_other_cpus
 *
 * Halt every other CPU (a panic is about to print). Does not wait for them
 * beyond a short bounded pause, and is safe to call more than once.
 */
void smp_stop_other_cpus(void);

/*
 * smp_tick_others
 *
 * Called from the boot CPU's own PIT interrupt handler (kern/i386/irq.c),
 * after it has counted the tick and charged its own scheduler: broadcasts a
 * reschedule IPI to every other online CPU so they run sched_tick() too. This
 * port has no per-CPU LAPIC timer (see apic.h's file comment), so a
 * secondary's quantum accounting is entirely driven by this broadcast, at
 * whatever rate the boot CPU's PIT is programmed for.
 */
void smp_tick_others(void);

#endif
