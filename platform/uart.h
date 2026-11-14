/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/uart.h
 *
 * The console UART: one serial port per machine, implemented once per
 * architecture (platform/arm64/uart.c, a PL011; platform/i386/uart.c, a
 * 16550 on COM1; platform/x86_64/uart.c, transmit only).
 *
 * Two clients share it:
 *
 *   - The kernel log (kern/console/console.c) writes characters with
 *     uart_putc, polled, from any context, from the first instruction of
 *     the boot on. It depends on nothing else here.
 *
 *   - The serial terminal (kern/tty/serial.c), /dev/console, writes the
 *     bytes its line discipline produced with uart_put_byte, reads input
 *     with uart_get_byte, configures the line with uart_set_line, and is
 *     told input has arrived by the receive interrupt.
 *
 * Transmitting and configuring the line are serialized by the caller: the
 * kernel console's lock is held around every one of them
 * (kconsole_serial_write, kconsole_serial_set_line), so a change of line
 * settings never meets a character half sent. Receiving has a single
 * client, the serial terminal, which serializes it with its own lock.
 */

#ifndef NXU_UART_H
#define NXU_UART_H

#include <stdbool.h>
#include <stdint.h>

bool uart_enter_higher_half(void);
bool uart_higher_half_enabled(void);

/* One character of the kernel log; a newline goes out as CR LF. */
void uart_putc(char character);

/* ---- the serial terminal's side ---------------------------------------- */

/* What the hardware said about a received byte (uart_get_byte). */
#define UART_RX_BREAK (1U << 0U)   /* the line was held in the break state; the byte is normally 0 */
#define UART_RX_PARITY (1U << 1U)  /* the byte failed its parity check */
#define UART_RX_FRAMING (1U << 2U) /* no stop bit where one belonged */
#define UART_RX_OVERRUN (1U << 3U) /* bytes before this one were lost; this one is good */

/* One byte exactly as given: no newline translation, which the terminal's output processing has done. */
void uart_put_byte(uint8_t byte);

/*
 * uart_get_byte
 *
 * Take the oldest received byte, if there is one: true with the byte and
 * its UART_RX_* status. Polled; never waits.
 */
bool uart_get_byte(uint8_t *byte, uint32_t *status);

typedef enum { UART_PARITY_NONE, UART_PARITY_EVEN, UART_PARITY_ODD } uart_parity_t;

/*
 * uart_line_t
 *
 * speed      bits per second
 * data_bits  5 to 8
 * stop_bits  1 or 2
 */
typedef struct uart_line {
	uint32_t speed;
	uint32_t data_bits;
	uint32_t stop_bits;
	uart_parity_t parity;
} uart_line_t;

/* Set the line up; false, changing nothing, for settings the hardware cannot make. */
bool uart_set_line(const uart_line_t *line);

/*
 * uart_receive_interrupt_attach
 *
 * Call `handler` in interrupt context whenever input (or a line error) is
 * waiting; the handler takes it with uart_get_byte. Receive interrupts
 * start disabled: see uart_receive_interrupt_enable. False when this
 * machine gives the UART no interrupt the kernel can take, and the client
 * polls instead. Called once.
 */
typedef void (*uart_receive_handler_t)(void *context);

bool uart_receive_interrupt_attach(uart_receive_handler_t handler, void *context);

/*
 * uart_receive_interrupt_enable
 *
 * Let the receive interrupt be raised, or not. With it off, input stays in
 * the hardware, and a sender that waits for room (QEMU's character
 * devices do) waits: that is how the serial terminal holds input back when
 * it has no room. Callable from interrupt context.
 */
void uart_receive_interrupt_enable(bool enable);

#endif
