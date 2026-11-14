/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/uart.c
 *
 * 16550 UART on COM1, the x86 PC counterpart of platform/arm64/uart.c and an
 * implementation of the same <platform/uart.h> interface. Registers live in
 * I/O port space, so there is nothing to remap when the kernel moves to a
 * higher-half mapping.
 *
 * Receiving. The FIFOs are on, with the receive trigger at 14 bytes; fewer
 * bytes than that raise the character-timeout interrupt instead, four
 * character times after the last one, so a single key press is not held
 * back. The receive interrupt (IER bit 0) and the line-status interrupt
 * (IER bit 2: break, parity, framing, overrun) are enabled and disabled
 * together. On a PC the UART's interrupt output reaches the PIC only while
 * OUT2 is set in the modem control register; COM1 is ISA line 4, edge
 * triggered.
 *
 * Line status is read before the data register: in FIFO mode its error
 * bits describe the byte at the head of the FIFO, the one the data register
 * returns next, and reading it clears them.
 */

#include <platform/uart.h>

#include <kern/i386/io.h>
#include <kern/i386/pic.h>
#include <kern/irq/irq.h>

#include <stdbool.h>
#include <stdint.h>

#define UART_COM1 0x3F8U
#define UART_COM1_IRQ 4U

/* The divisor latch counts from a 1.8432 MHz clock divided by 16. */
#define UART_BASE_SPEED 115200U

#define UART_DATA 0U
#define UART_INTERRUPT_ENABLE 1U
#define UART_INTERRUPT_ID 2U /* read */
#define UART_FIFO_CONTROL 2U /* write */
#define UART_LINE_CONTROL 3U
#define UART_MODEM_CONTROL 4U
#define UART_LINE_STATUS 5U

#define UART_IER_RECEIVE 0x01U
#define UART_IER_LINE_STATUS 0x04U

#define UART_IIR_NONE 0x01U /* no interrupt pending */

#define UART_FCR_ENABLE_CLEAR_14 0xC7U /* enable, clear both FIFOs, receive trigger at 14 bytes */

#define UART_LCR_STOP2 0x04U
#define UART_LCR_PARITY 0x08U
#define UART_LCR_EVEN 0x10U
#define UART_LCR_DLAB 0x80U

#define UART_MCR_DTR 0x01U
#define UART_MCR_RTS 0x02U
#define UART_MCR_OUT2 0x08U /* gates the interrupt output onto the ISA bus */

#define UART_LSR_DATA 0x01U
#define UART_LSR_OVERRUN 0x02U
#define UART_LSR_PARITY 0x04U
#define UART_LSR_FRAMING 0x08U
#define UART_LSR_BREAK 0x10U
#define UART_LSR_TX_EMPTY 0x20U

/* A handler that keeps finding interrupts pending gives up after this many rounds; edge triggering re-raises it. */
#define UART_IRQ_ROUNDS 16U

static bool g_uart_ready;

static uart_receive_handler_t g_uart_handler;
static void *g_uart_handler_context;

static void
uart_init(void)
{
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, 0x00U);
	outb(UART_COM1 + UART_LINE_CONTROL, UART_LCR_DLAB);
	outb(UART_COM1 + UART_DATA, 0x01U); /* divisor 1: 115200 baud */
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, 0x00U);
	outb(UART_COM1 + UART_LINE_CONTROL, 0x03U); /* 8N1, DLAB off */
	outb(UART_COM1 + UART_FIFO_CONTROL, UART_FCR_ENABLE_CLEAR_14);
	outb(UART_COM1 + UART_MODEM_CONTROL, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

	g_uart_ready = true;
}

bool
uart_enter_higher_half(void)
{
	return true;
}

bool
uart_higher_half_enabled(void)
{
	return true;
}

void
uart_put_byte(uint8_t byte)
{
	if (!g_uart_ready) uart_init();

	while ((inb(UART_COM1 + UART_LINE_STATUS) & UART_LSR_TX_EMPTY) == 0U) {
		__asm__ volatile("pause");
	}

	outb(UART_COM1 + UART_DATA, byte);
}

void
uart_putc(char character)
{
	if (character == '\n') uart_put_byte('\r');

	uart_put_byte((uint8_t)character);
}

bool
uart_get_byte(uint8_t *byte, uint32_t *status)
{
	if (!g_uart_ready) uart_init();

	uint8_t line = inb(UART_COM1 + UART_LINE_STATUS);

	if ((line & UART_LSR_DATA) == 0U) return false;

	uint32_t flags = 0U;

	if ((line & UART_LSR_BREAK) != 0U) flags |= UART_RX_BREAK;
	if ((line & UART_LSR_PARITY) != 0U) flags |= UART_RX_PARITY;
	if ((line & UART_LSR_FRAMING) != 0U) flags |= UART_RX_FRAMING;
	if ((line & UART_LSR_OVERRUN) != 0U) flags |= UART_RX_OVERRUN;

	*byte = inb(UART_COM1 + UART_DATA);
	*status = flags;
	return true;
}

bool
uart_set_line(const uart_line_t *line)
{
	if (line->speed == 0U || line->speed > UART_BASE_SPEED || UART_BASE_SPEED % line->speed != 0U) return false;
	if (line->data_bits < 5U || line->data_bits > 8U) return false;
	if (line->stop_bits != 1U && line->stop_bits != 2U) return false;

	uint32_t divisor = UART_BASE_SPEED / line->speed;

	if (divisor > 0xFFFFU) return false;

	uint8_t control = (uint8_t)(line->data_bits - 5U);

	if (line->stop_bits == 2U) control |= UART_LCR_STOP2;
	if (line->parity != UART_PARITY_NONE) control |= UART_LCR_PARITY;
	if (line->parity == UART_PARITY_EVEN) control |= UART_LCR_EVEN;

	if (!g_uart_ready) uart_init();

	/* Let the last character leave at the old settings first. */
	while ((inb(UART_COM1 + UART_LINE_STATUS) & UART_LSR_TX_EMPTY) == 0U) {
		__asm__ volatile("pause");
	}

	/* With DLAB set, ports 0 and 1 are the divisor latch; the interrupt enable register is untouched. */
	outb(UART_COM1 + UART_LINE_CONTROL, UART_LCR_DLAB);
	outb(UART_COM1 + UART_DATA, (uint8_t)(divisor & 0xFFU));
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, (uint8_t)(divisor >> 8U));
	outb(UART_COM1 + UART_LINE_CONTROL, control);
	return true;
}

/* IRQ 4: run the client's handler for as long as the UART reports something pending. */
static void
uart_irq(uint32_t intid, void *context)
{
	(void)intid;
	(void)context;

	for (uint32_t round = 0U; round < UART_IRQ_ROUNDS; round++) {
		if ((inb(UART_COM1 + UART_INTERRUPT_ID) & UART_IIR_NONE) != 0U) return;

		g_uart_handler(g_uart_handler_context);
	}
}

bool
uart_receive_interrupt_attach(uart_receive_handler_t handler, void *context)
{
	if (handler == 0 || g_uart_handler != 0) return false;

	if (!g_uart_ready) uart_init();

	g_uart_handler = handler;
	g_uart_handler_context = context;

	if (!irq_register(UART_COM1_IRQ, uart_irq, 0)) {
		g_uart_handler = 0;
		return false;
	}

	pic_unmask(UART_COM1_IRQ);
	return true;
}

void
uart_receive_interrupt_enable(bool enable)
{
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, enable ? (UART_IER_RECEIVE | UART_IER_LINE_STATUS) : 0x00U);
}
