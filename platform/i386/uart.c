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
 */

#include <platform/uart.h>

#include <kern/i386/io.h>

#include <stdbool.h>
#include <stdint.h>

#define UART_COM1 0x3F8U

#define UART_DATA 0U
#define UART_INTERRUPT_ENABLE 1U
#define UART_FIFO_CONTROL 2U
#define UART_LINE_CONTROL 3U
#define UART_MODEM_CONTROL 4U
#define UART_LINE_STATUS 5U

#define UART_LSR_TX_EMPTY 0x20U

static bool g_uart_ready;

static void uart_init(void)
{
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, 0x00U);
	outb(UART_COM1 + UART_LINE_CONTROL, 0x80U); /* DLAB on */
	outb(UART_COM1 + UART_DATA, 0x01U); /* divisor 1: 115200 baud */
	outb(UART_COM1 + UART_INTERRUPT_ENABLE, 0x00U);
	outb(UART_COM1 + UART_LINE_CONTROL, 0x03U); /* 8N1, DLAB off */
	outb(UART_COM1 + UART_FIFO_CONTROL, 0xC7U); /* enable and clear FIFOs */
	outb(UART_COM1 + UART_MODEM_CONTROL, 0x03U); /* DTR + RTS */

	g_uart_ready = true;
}

bool uart_enter_higher_half(void)
{
	return true;
}

bool uart_higher_half_enabled(void)
{
	return true;
}

void uart_putc(char character)
{
	if (!g_uart_ready) uart_init();

	if (character == '\n') uart_putc('\r');

	while ((inb(UART_COM1 + UART_LINE_STATUS) & UART_LSR_TX_EMPTY) == 0U) {
		__asm__ volatile("pause");
	}

	outb(UART_COM1 + UART_DATA, (uint8_t)character);
}
