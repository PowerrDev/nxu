/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/system.h
 *
 * x86 implementation of <kern/machine/system.h>, shared by the i386 and
 * x86_64 builds. Neither operation returns on a working machine.
 */

#ifndef NXU_KERN_I386_SYSTEM_H
#define NXU_KERN_I386_SYSTEM_H

#include <kern/i386/io.h>

#include <stdint.h>

/* The 8042 keyboard controller: command port, and the "pulse reset" command. */
#define I386_KBC_COMMAND_PORT 0x64U
#define I386_KBC_PULSE_RESET 0xFEU

/* PIIX4 power-management I/O port QEMU decodes as ACPI shutdown, and Bochs' twin. */
#define I386_QEMU_SHUTDOWN_PORT 0x604U
#define I386_BOCHS_SHUTDOWN_PORT 0xB004U
#define I386_ACPI_SLEEP_S5 0x2000U

static inline void i386_outw(uint16_t port, uint16_t value)
{
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * machine_system_reset
 *
 * Pulse the CPU reset line through the keyboard controller. If the
 * platform has no 8042, fall back to a triple fault: load an empty IDT and
 * raise an exception, so the CPU cannot deliver it, the double fault
 * cannot be delivered either, and the processor resets.
 */
static inline void machine_system_reset(void)
{
	__asm__ volatile("cli" : : : "memory");

	outb((uint16_t)I386_KBC_COMMAND_PORT, (uint8_t)I386_KBC_PULSE_RESET);

	/* Give the controller time to act before falling back. */
	for (uint32_t spin = 0U; spin < 1000U; spin++) io_wait();

	static const struct {
		uint16_t limit;
		uint32_t base;
	} __attribute__((packed)) empty_idt = { 0U, 0U };

	__asm__ volatile("lidt %0\n\tint3" : : "m"(empty_idt) : "memory");

	for (;;) __asm__ volatile("hlt");
}

/*
 * machine_system_power_off
 *
 * Ask the emulated ACPI power-management block to enter S5. Real hardware
 * needs ACPI tables to find the port; without them the machine is halted
 * with interrupts masked, which is the best a bare kernel can do.
 */
static inline void machine_system_power_off(void)
{
	i386_outw((uint16_t)I386_QEMU_SHUTDOWN_PORT, (uint16_t)I386_ACPI_SLEEP_S5);
	i386_outw((uint16_t)I386_BOCHS_SHUTDOWN_PORT, (uint16_t)I386_ACPI_SLEEP_S5);

	for (;;) __asm__ volatile("cli\n\thlt");
}

#endif
