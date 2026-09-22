/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/apic.c
 *
 * See apic.h.
 */

#include <kern/i386/apic.h>

#include <kern/i386/pmap.h>
#include <kern/i386/timer.h>
#include <kern/i386/trap.h>

#include <kern/console/console.h>
#include <kern/machine/cpu.h>

#include <stdbool.h>
#include <stdint.h>

#define APIC_MMIO_SIZE 4096U

#define APIC_REG_ID 0x020U
#define APIC_REG_SPURIOUS 0x0F0U
#define APIC_REG_ICR_LOW 0x300U
#define APIC_REG_ICR_HIGH 0x310U
#define APIC_REG_EOI 0x0B0U

#define APIC_SPURIOUS_SOFTWARE_ENABLE 0x100U

#define APIC_ICR_DELIVERY_FIXED 0x00000U
#define APIC_ICR_DELIVERY_INIT 0x04500U
#define APIC_ICR_DELIVERY_SIPI 0x04600U
#define APIC_ICR_LEVEL_ASSERT 0x04000U
#define APIC_ICR_TRIGGER_LEVEL 0x08000U
#define APIC_ICR_DEST_SHORTHAND_ALL_BUT_SELF 0xC0000U
#define APIC_ICR_DELIVERY_STATUS_PENDING 0x01000U

static volatile uint8_t *g_apic_base;

static uint32_t apic_read(uint32_t offset)
{
	return *(volatile uint32_t *)(g_apic_base + offset);
}

static void apic_write(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(g_apic_base + offset) = value;
}

/* ICR writes only take effect once the previous one has been sent (bit 12 clears). Bounded: a stuck ICR must not hang boot forever. */
static void apic_wait_icr_idle(void)
{
	uint64_t deadline = timer_get_microseconds() + 100000ULL;

	while ((apic_read(APIC_REG_ICR_LOW) & APIC_ICR_DELIVERY_STATUS_PENDING) != 0U) {
		if (timer_get_microseconds() > deadline) return;
		cpu_relax();
	}
}

bool apic_init(uint32_t physical_address)
{
	void *virtual_address;

	if (!pmap_map_mmio((uint64_t)physical_address, APIC_MMIO_SIZE, &virtual_address)) {
		kputln("apic_init: cannot map the Local APIC MMIO window");
		return false;
	}

	g_apic_base = (volatile uint8_t *)virtual_address;

	apic_enable_local();

	kprintf(
		"apic_init: Local APIC at phys 0x%x mapped to %p, id %u, spurious vector 0x%x\n",
		physical_address,
		(void *)g_apic_base,
		apic_id(),
		T_LAPIC_SPURIOUS
	);

	return true;
}

void apic_enable_local(void)
{
	apic_write(APIC_REG_SPURIOUS, APIC_SPURIOUS_SOFTWARE_ENABLE | T_LAPIC_SPURIOUS);
}

uint32_t apic_id(void)
{
	return apic_read(APIC_REG_ID) >> 24U;
}

void apic_eoi(void)
{
	apic_write(APIC_REG_EOI, 0U);
}

void apic_start_cpu(uint8_t target_apic_id, uint32_t entry_physical)
{
	uint32_t vector = entry_physical >> 12U;

	/* INIT assert. */
	apic_wait_icr_idle();
	apic_write(APIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24U);
	apic_write(APIC_REG_ICR_LOW, APIC_ICR_DELIVERY_INIT | APIC_ICR_LEVEL_ASSERT | APIC_ICR_TRIGGER_LEVEL);
	apic_wait_icr_idle();

	/* INIT deassert. */
	apic_write(APIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24U);
	apic_write(APIC_REG_ICR_LOW, APIC_ICR_DELIVERY_INIT | APIC_ICR_TRIGGER_LEVEL);
	apic_wait_icr_idle();

	uint64_t deadline = timer_get_microseconds() + 10000ULL;

	while (timer_get_microseconds() < deadline) cpu_relax();

	/* SIPI, twice (the second is a no-op on hardware that started on the first, required on some that has not). */
	for (uint32_t attempt = 0U; attempt < 2U; attempt++) {
		apic_wait_icr_idle();
		apic_write(APIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24U);
		apic_write(APIC_REG_ICR_LOW, APIC_ICR_DELIVERY_SIPI | vector);
		apic_wait_icr_idle();

		deadline = timer_get_microseconds() + 200ULL;
		while (timer_get_microseconds() < deadline) cpu_relax();
	}
}

void apic_send_ipi(uint8_t target_apic_id, uint8_t vector)
{
	apic_wait_icr_idle();
	apic_write(APIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24U);
	apic_write(APIC_REG_ICR_LOW, APIC_ICR_DELIVERY_FIXED | vector);
}

void apic_send_ipi_all_but_self(uint8_t vector)
{
	apic_wait_icr_idle();
	apic_write(APIC_REG_ICR_HIGH, 0U);
	apic_write(APIC_REG_ICR_LOW, APIC_ICR_DELIVERY_FIXED | APIC_ICR_DEST_SHORTHAND_ALL_BUT_SELF | vector);
}
