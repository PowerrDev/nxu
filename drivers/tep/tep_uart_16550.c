/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_uart_16550.c
 *
 * i386 mailbox port: the 16550 on COM2 (I/O ports 0x2F8-0x2FF), which QEMU's
 * pc machine has when it is given a second -serial. COM1 stays the console
 * (platform/i386/uart.c). Presence is probed through the scratch register:
 * with no UART behind them the ports read back 0xFF.
 */

#include <drivers/tep/tep_uart.h>

#include <kern/i386/io.h>
#include <kern/machine/cpu.h>

#include <stdbool.h>
#include <stdint.h>

#define UART_COM2 0x2F8U

#define UART_DATA 0U
#define UART_INTERRUPT_ENABLE 1U
#define UART_FIFO_CONTROL 2U
#define UART_LINE_CONTROL 3U
#define UART_MODEM_CONTROL 4U
#define UART_LINE_STATUS 5U
#define UART_SCRATCH 7U

#define UART_LSR_DATA_READY 0x01U
#define UART_LSR_TX_EMPTY 0x20U

static bool tep_uart_16550_present(void)
{
	static const uint8_t patterns[2] = { 0x5AU, 0xA5U };

	for (uint32_t index = 0U; index < 2U; index++) {
		outb(UART_COM2 + UART_SCRATCH, patterns[index]);
		if (inb(UART_COM2 + UART_SCRATCH) != patterns[index]) return false;
	}
	return inb(UART_COM2 + UART_LINE_STATUS) != 0xFFU;
}

bool tep_uart_init(const char **where, uint64_t *address)
{
	if (!tep_uart_16550_present()) return false;

	outb(UART_COM2 + UART_INTERRUPT_ENABLE, 0x00U); /* polled */
	outb(UART_COM2 + UART_LINE_CONTROL, 0x80U); /* DLAB on */
	outb(UART_COM2 + UART_DATA, 0x01U); /* divisor 1: 115200 baud */
	outb(UART_COM2 + UART_INTERRUPT_ENABLE, 0x00U);
	outb(UART_COM2 + UART_LINE_CONTROL, 0x03U); /* 8N1, DLAB off */
	outb(UART_COM2 + UART_FIFO_CONTROL, 0xC7U); /* enable and clear FIFOs */
	outb(UART_COM2 + UART_MODEM_CONTROL, 0x03U); /* DTR + RTS */

	*where = "16550 COM2";
	*address = UART_COM2;
	return true;
}

int tep_uart_getc(void)
{
	if ((inb(UART_COM2 + UART_LINE_STATUS) & UART_LSR_DATA_READY) == 0U) return -1;
	return (int)inb(UART_COM2 + UART_DATA);
}

void tep_uart_putc(uint8_t byte)
{
	while ((inb(UART_COM2 + UART_LINE_STATUS) & UART_LSR_TX_EMPTY) == 0U) cpu_relax();
	outb(UART_COM2 + UART_DATA, byte);
}
