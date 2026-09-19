/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        mach/i386/pic.c
 *
 * See pic.h.
 */

#include <mach/i386/pic.h>

#include <mach/i386/io.h>
#include <mach/i386/trap.h>

#include <mach/machine/machine_routines.h>

#include <stdbool.h>
#include <stdint.h>

#define PIC_MASTER_COMMAND 0x20U
#define PIC_MASTER_DATA 0x21U
#define PIC_SLAVE_COMMAND 0xA0U
#define PIC_SLAVE_DATA 0xA1U

#define PIC_ELCR_MASTER 0x4D0U
#define PIC_ELCR_SLAVE 0x4D1U

/* ICW1: initialisation, ICW4 follows, cascade mode, edge triggered. */
#define PIC_ICW1_INIT 0x11U
/* ICW4: 8086/88 mode, normal EOI, non-buffered, not special fully nested. */
#define PIC_ICW4_8086 0x01U

/* OCW2 specific EOI: 0x60 | level. */
#define PIC_OCW2_SPECIFIC_EOI 0x60U

/* OCW3: read the in-service / interrupt-request register. */
#define PIC_OCW3_READ_ISR 0x0BU
#define PIC_OCW3_READ_IRR 0x0AU

/* Lines whose trigger mode is fixed to edge by the chipset. */
#define PIC_ELCR_FIXED_EDGE ((1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 8U) | (1U << 13U))

/* Shadow of the two mask registers, slave in the high byte. */
static uint16_t g_pic_mask = 0xFFFFU;
static volatile uint32_t g_pic_spurious_count;

static void pic_write_mask(void)
{
	outb(PIC_MASTER_DATA, (uint8_t)(g_pic_mask & 0xFFU));
	outb(PIC_SLAVE_DATA, (uint8_t)(g_pic_mask >> 8U));
}

/*
 * The cascade input is not a device line: it must be open exactly when some
 * slave line is, and closed otherwise, so it follows the slave's mask.
 */
static void pic_update_cascade(void)
{
	if ((g_pic_mask & 0xFF00U) == 0xFF00U) {
		g_pic_mask |= (uint16_t)(1U << PIC_CASCADE_LINE);
	} else {
		g_pic_mask &= (uint16_t)~(1U << PIC_CASCADE_LINE);
	}
}

void pic_init(void)
{
	uint64_t state = ml_irq_save();

	/* ICW1: begin initialisation of both controllers. */
	outb(PIC_MASTER_COMMAND, PIC_ICW1_INIT);
	io_wait();
	outb(PIC_SLAVE_COMMAND, PIC_ICW1_INIT);
	io_wait();

	/* ICW2: vector offsets. */
	outb(PIC_MASTER_DATA, (uint8_t)T_IRQ_BASE);
	io_wait();
	outb(PIC_SLAVE_DATA, (uint8_t)(T_IRQ_BASE + 8U));
	io_wait();

	/* ICW3: the slave hangs off master IRQ2; the slave's cascade identity is 2. */
	outb(PIC_MASTER_DATA, (uint8_t)(1U << PIC_CASCADE_LINE));
	io_wait();
	outb(PIC_SLAVE_DATA, (uint8_t)PIC_CASCADE_LINE);
	io_wait();

	/* ICW4. */
	outb(PIC_MASTER_DATA, PIC_ICW4_8086);
	io_wait();
	outb(PIC_SLAVE_DATA, PIC_ICW4_8086);
	io_wait();

	/* Everything masked, including the cascade until a slave line opens it. */
	g_pic_mask = 0xFFFFU;
	pic_write_mask();

	ml_irq_restore(state);
}

void pic_mask(uint32_t irq)
{
	if (irq >= PIC_LINE_COUNT || irq == PIC_CASCADE_LINE) return;

	uint64_t state = ml_irq_save();

	g_pic_mask |= (uint16_t)(1U << irq);
	pic_update_cascade();
	pic_write_mask();
	ml_irq_restore(state);
}

void pic_unmask(uint32_t irq)
{
	if (irq >= PIC_LINE_COUNT || irq == PIC_CASCADE_LINE) return;

	uint64_t state = ml_irq_save();

	g_pic_mask &= (uint16_t)~(1U << irq);
	pic_update_cascade();
	pic_write_mask();
	ml_irq_restore(state);
}

bool pic_is_masked(uint32_t irq)
{
	if (irq >= PIC_LINE_COUNT) return true;

	return (g_pic_mask & (1U << irq)) != 0U;
}

void pic_eoi(uint32_t irq)
{
	if (irq >= PIC_LINE_COUNT) return;

	if (irq >= 8U) {
		outb(PIC_SLAVE_COMMAND, (uint8_t)(PIC_OCW2_SPECIFIC_EOI | (irq & 7U)));
		outb(PIC_MASTER_COMMAND, (uint8_t)(PIC_OCW2_SPECIFIC_EOI | PIC_CASCADE_LINE));
	} else {
		outb(PIC_MASTER_COMMAND, (uint8_t)(PIC_OCW2_SPECIFIC_EOI | irq));
	}
}

static uint16_t pic_read_register(uint8_t ocw3)
{
	uint64_t state = ml_irq_save();

	outb(PIC_MASTER_COMMAND, ocw3);
	outb(PIC_SLAVE_COMMAND, ocw3);

	uint16_t value = (uint16_t)(inb(PIC_MASTER_COMMAND) | ((uint16_t)inb(PIC_SLAVE_COMMAND) << 8U));

	ml_irq_restore(state);
	return value;
}

uint16_t pic_read_isr(void)
{
	return pic_read_register(PIC_OCW3_READ_ISR);
}

uint16_t pic_read_irr(void)
{
	return pic_read_register(PIC_OCW3_READ_IRR);
}

uint16_t pic_read_imr(void)
{
	return (uint16_t)(inb(PIC_MASTER_DATA) | ((uint16_t)inb(PIC_SLAVE_DATA) << 8U));
}

bool pic_irq_is_spurious(uint32_t irq)
{
	if (irq != 7U && irq != 15U) return false;

	/* A real interrupt on this line is in service; a spurious one is not. */
	if ((pic_read_isr() & (1U << irq)) != 0U) return false;

	g_pic_spurious_count++;

	/*
	 * The master really did signal the cascade line for a spurious IRQ15,
	 * so it holds IRQ2 in service and needs its EOI; the slave does not.
	 */
	if (irq == 15U) outb(PIC_MASTER_COMMAND, (uint8_t)(PIC_OCW2_SPECIFIC_EOI | PIC_CASCADE_LINE));

	return true;
}

uint32_t pic_spurious_count(void)
{
	return g_pic_spurious_count;
}

bool pic_set_level_triggered(uint32_t irq, bool level)
{
	if (irq >= PIC_LINE_COUNT || (PIC_ELCR_FIXED_EDGE & (1U << irq)) != 0U) return false;

	uint64_t state = ml_irq_save();
	uint16_t elcr = (uint16_t)(inb(PIC_ELCR_MASTER) | ((uint16_t)inb(PIC_ELCR_SLAVE) << 8U));

	if (level) {
		elcr |= (uint16_t)(1U << irq);
	} else {
		elcr &= (uint16_t)~(1U << irq);
	}

	outb(PIC_ELCR_MASTER, (uint8_t)(elcr & 0xFFU));
	outb(PIC_ELCR_SLAVE, (uint8_t)(elcr >> 8U));

	uint16_t readback = (uint16_t)(inb(PIC_ELCR_MASTER) | ((uint16_t)inb(PIC_ELCR_SLAVE) << 8U));

	ml_irq_restore(state);
	return ((readback >> irq) & 1U) == (level ? 1U : 0U);
}
