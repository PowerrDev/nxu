/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/apic.h
 *
 * Local APIC: IPIs (kern/ipi.h), the universal INIT-SIPI-SIPI startup
 * algorithm that brings a secondary CPU out of real mode (kern/i386/smp.c),
 * and each CPU's own timer (kern/i386/lapic_timer.c, which programs the
 * timer registers through apic_read_register/apic_write_register). No
 * IOAPIC: every device IRQ stays on the legacy 8259 PIC, delivered to the
 * boot CPU only (see doc/i386/smp.md's known gaps).
 */

#ifndef NXU_KERN_I386_APIC_H
#define NXU_KERN_I386_APIC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Map the Local APIC's MMIO window at `physical_address` (from the MP
 * configuration table header, the same address on every CPU) and enable it
 * with T_LAPIC_SPURIOUS as its spurious vector. Called once by the boot CPU;
 * a secondary just needs the software-enable bit set in its own copy of the
 * (identically MMIO-mapped, since the direct map is shared) register window,
 * which apic_enable_local() does.
 */
bool apic_init(uint32_t physical_address);

/*
 * apic_map_local
 *
 * Make sure the Local APIC is usable on a machine apic_init has not set up
 * (a uniprocessor boot, which never reads an MP table): find it from the
 * IA32_APIC_BASE MSR, globally enable it, map its window and software-enable
 * it on the boot CPU. Returns false if the CPU has no Local APIC (CPUID).
 * Does nothing more once the window is mapped.
 */
bool apic_map_local(void);

/* Whether the Local APIC window is mapped (apic_init or apic_map_local succeeded). */
bool apic_is_mapped(void);

/* A Local APIC register of the calling CPU, by its offset in the window. The window must be mapped. */
uint32_t apic_read_register(uint32_t offset);
void apic_write_register(uint32_t offset, uint32_t value);

/* Set the spurious-vector register's software-enable bit on the calling CPU. */
void apic_enable_local(void);

/* This CPU's Local APIC ID, straight from the ID register (bits 24-31, xAPIC layout). */
uint32_t apic_id(void);

/* Signal end-of-interrupt for the vector currently in service on this CPU. */
void apic_eoi(void);

/*
 * apic_start_cpu
 *
 * The universal INIT-SIPI-SIPI startup algorithm (Intel MP spec / SDM vol
 * 3A): INIT-assert, INIT-deassert, wait 10 ms, SIPI with the trampoline's
 * page number as its vector (entry_physical must be page-aligned and below 1
 * MiB -- real mode addressing, see smp.c), wait 200 us, SIPI again (some
 * hardware needs it twice; modern ones ignore the redundant one once
 * started), wait 200 us. Fire-and-forget: the caller polls the target CPU's
 * own "online" flag to know whether it actually came up.
 */
void apic_start_cpu(uint8_t target_apic_id, uint32_t entry_physical);

/* Interrupt CPU `target_apic_id` with a fixed-vector IPI (kern/i386/smp.c's IPI plumbing). */
void apic_send_ipi(uint8_t target_apic_id, uint8_t vector);

/* The same, to every other CPU: "all excluding self" destination shorthand, one ICR write. */
void apic_send_ipi_all_but_self(uint8_t vector);

#endif
