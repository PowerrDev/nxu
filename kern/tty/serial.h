/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/serial.h
 *
 * The serial terminal: /dev/console, a terminal (kern/tty/tty.h) whose line
 * is the console UART (platform/uart.h).
 *
 * The UART is shared with the kernel log, which writes to it directly and
 * does not depend on this terminal at all: the log works before this exists,
 * and goes on working if it fails. What a program writes to /dev/console goes
 * out on the same wire, between log lines, never inside one.
 *
 * Input arrives by the UART's receive interrupt, or, on a machine where the
 * UART has none the kernel can take, by polling every 10 ms. Either way it
 * reaches the terminal from a kernel thread of this file, since the line
 * discipline runs in thread context only.
 */

#ifndef NXU_KERN_TTY_SERIAL_H
#define NXU_KERN_TTY_SERIAL_H

#include <kern/tty/tty.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * serial_init
 *
 * Make the serial terminal, publish it as /dev/console, and start its
 * receive thread. Called once at boot, after tty_init and once kernel
 * threads can be started. False when it could not be made; the kernel log is
 * unaffected either way.
 */
bool serial_init(void);

/*
 * serial_stats_t / serial_stats
 *
 * What the line has seen since boot, for diagnostics and tests.
 *
 * received      bytes taken from the UART
 * delivered     of those, given to the terminal (the rest are still queued)
 * breaks        break conditions; parity, framing: bytes with those errors
 * overruns      bytes the UART reported it lost before a received one
 * held          times input was held back in the UART for want of room
 * interrupts    receive interrupts taken; zero when the line is polled
 * interrupt     whether the line is interrupt driven
 */
typedef struct {
	uint64_t received;
	uint64_t delivered;
	uint64_t breaks;
	uint64_t parity;
	uint64_t framing;
	uint64_t overruns;
	uint64_t held;
	uint64_t interrupts;
	bool interrupt;
} serial_stats_t;

bool serial_stats(serial_stats_t *stats);

/* The serial terminal, with a reference, or TTY_NULL before serial_init. */
tty_t serial_tty(void);

#endif
