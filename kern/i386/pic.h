/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/pic.h
 *
 * Driver for the pair of cascaded 8259A interrupt controllers of a legacy PC.
 *
 * The master handles IRQ0-7 and the slave IRQ8-15; the slave's output is
 * wired to the master's IRQ2 input (the cascade). Reset maps IRQ0-7 onto
 * vectors 8-15, which collide with processor exceptions, so pic_init()
 * moves them to T_IRQ_BASE .. T_IRQ_BASE + 15 (vectors 32-47).
 *
 * Every function that touches the controllers is safe to call with
 * interrupts enabled or disabled; the read-modify-write of the mask
 * registers is done with interrupts masked internally.
 */

#ifndef NXU_KERN_I386_PIC_H
#define NXU_KERN_I386_PIC_H

#include <stdbool.h>
#include <stdint.h>

#define PIC_LINE_COUNT 16U

/* The master input the slave's output is wired to. */
#define PIC_CASCADE_LINE 2U

/*
 * pic_init:
 *
 * Reinitialise both controllers (edge triggered, 8086 mode, normal EOI),
 * remap them to vectors 32-47 and mask every line. The cascade input is
 * managed by pic_mask()/pic_unmask() and stays masked until a slave line is
 * unmasked. Interrupt delivery to the CPU is not changed.
 */
void pic_init(void);

/*
 * Mask or unmask one line (0..15). Out-of-range lines are ignored. The
 * cascade line (2) is not a device line: it is opened automatically while
 * any slave line is unmasked, and requests for it are ignored.
 */
void pic_mask(uint32_t irq);
void pic_unmask(uint32_t irq);

/* True if the line is masked, as far as this driver has programmed it. */
bool pic_is_masked(uint32_t irq);

/*
 * pic_eoi:
 *
 * Signal end of interrupt for a line with a specific EOI. For a slave line
 * the slave is acknowledged first, then the cascade input on the master.
 * A specific EOI for a line that is not in service is a no-op, so this is
 * also safe for a software-generated `int $32+irq`.
 */
void pic_eoi(uint32_t irq);

/*
 * pic_irq_is_spurious:
 *
 * Call first thing when servicing IRQ7 or IRQ15 (any other line returns
 * false). The 8259 delivers vector 7 / 15 when an interrupt vanished before
 * the CPU acknowledged it; such an interrupt has no ISR bit set and must
 * not be acknowledged. For a spurious IRQ15 this function itself sends the
 * EOI the master needs for the cascade line (the slave gets none).
 *
 * Returns true if the interrupt was spurious and needs no further handling.
 */
bool pic_irq_is_spurious(uint32_t irq);

/* Number of spurious interrupts seen by pic_irq_is_spurious(). */
uint32_t pic_spurious_count(void);

/* In-service and pending registers, slave in the high byte. */
uint16_t pic_read_isr(void);
uint16_t pic_read_irr(void);

/* The mask registers as they are in hardware, slave in the high byte. */
uint16_t pic_read_imr(void);

/*
 * pic_set_level_triggered:
 *
 * Select level (true) or edge (false) triggering for a line through the
 * ELCR (ports 0x4D0/0x4D1). PCI INTx lines are level triggered and shared,
 * so a device driver claiming one should switch it to level mode. Lines 0,
 * 1, 2, 8 and 13 are fixed edge triggered and are refused.
 *
 * Returns true if the line is now in the requested mode.
 */
bool pic_set_level_triggered(uint32_t irq, bool level);

#endif
