/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/x86_64/uart.c
 *
 * Polling COM1 UART support for the early x86-64 boot path.
 */

#include <platform/x86_64/uart.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>

#define X86_64_UART_COM1 0x3F8U
#define X86_64_UART_DATA 0U
#define X86_64_UART_INTERRUPT_ENABLE 1U
#define X86_64_UART_FIFO_CONTROL 2U
#define X86_64_UART_LINE_CONTROL 3U
#define X86_64_UART_MODEM_CONTROL 4U
#define X86_64_UART_LINE_STATUS 5U
#define X86_64_UART_TX_EMPTY 0x20U

static bool g_uart_ready;

/*
 * Routine:     uart_enter_higher_half
 * Purpose:
 *              Confirm serial I/O remains available across the direct-map
 *              transition because COM1 uses x86 port space.
 */
bool
uart_enter_higher_half(void)
{
	return true;
}

/*
 * Routine:     uart_higher_half_enabled
 * Purpose:
 *              Report that the port-based UART needs no virtual remapping.
 */
bool
uart_higher_half_enabled(void)
{
	return true;
}

/*
 * Routine:     x86_outb
 * Purpose:
 *              Write one byte to an x86 I/O port for early platform setup.
 */
static inline void
x86_outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

/*
 * Routine:     x86_inb
 * Purpose:
 *              Read one byte from an x86 I/O port while polling COM1.
 */
static inline uint8_t
x86_inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
	return value;
}

/*
 * Routine:     x86_uart_initialize
 * Purpose:
 *              Program COM1 once; polling keeps this path independent of IDT,
 *              interrupt routing, and the machine-independent console.
 */
void
x86_uart_initialize(void)
{
	x86_outb(X86_64_UART_COM1 + X86_64_UART_INTERRUPT_ENABLE, 0x00U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_LINE_CONTROL, 0x80U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_DATA, 0x01U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_INTERRUPT_ENABLE, 0x00U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_LINE_CONTROL, 0x03U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_FIFO_CONTROL, 0xC7U);
	x86_outb(X86_64_UART_COM1 + X86_64_UART_MODEM_CONTROL, 0x03U);
	g_uart_ready = true;
}

/*
 * Routine:     x86_uart_write
 * Purpose:
 *              Emit characters using COM1's transmitter-ready bit and expand
 *              line feeds for terminal monitors that expect CRLF.
 */
void
x86_uart_write(const char *message)
{
	if (!g_uart_ready) {
		x86_uart_initialize();
	}

	while (*message != '\0') {
		if (*message == '\n') {
			while ((x86_inb(X86_64_UART_COM1 + X86_64_UART_LINE_STATUS) & X86_64_UART_TX_EMPTY) == 0U) {
				__asm__ volatile("pause");
			}

			x86_outb(X86_64_UART_COM1 + X86_64_UART_DATA, '\r');
		}

		while ((x86_inb(X86_64_UART_COM1 + X86_64_UART_LINE_STATUS) & X86_64_UART_TX_EMPTY) == 0U) {
			__asm__ volatile("pause");
		}

		x86_outb(X86_64_UART_COM1 + X86_64_UART_DATA, (uint8_t)*message++);
	}
}

/*
 * Routine:     uart_putc
 * Purpose:
 *              Emit one character through COM1 for the shared kernel console.
 */
void
uart_putc(char character)
{
	if (!g_uart_ready) {
		x86_uart_initialize();
	}

	if (character == '\n') {
		while ((x86_inb(X86_64_UART_COM1 + X86_64_UART_LINE_STATUS) & X86_64_UART_TX_EMPTY) == 0U) {
			__asm__ volatile("pause");
		}

		x86_outb(X86_64_UART_COM1 + X86_64_UART_DATA, '\r');
	}

	while ((x86_inb(X86_64_UART_COM1 + X86_64_UART_LINE_STATUS) & X86_64_UART_TX_EMPTY) == 0U) {
		__asm__ volatile("pause");
	}

	x86_outb(X86_64_UART_COM1 + X86_64_UART_DATA, (uint8_t)character);
}
