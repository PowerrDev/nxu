/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tty_loopback.h
 *
 * A terminal driver for tests: whatever is written to the terminal comes
 * back as its input, or, in capture mode, is kept for the test to look at
 * while the test types the input itself with tty_input.
 *
 * Moving bytes from output to input is done by one caller at a time, so two
 * (a writer's start and a reader's unthrottle) cannot interleave their
 * chunks. tty_input may call start again from inside a move (echo), so the
 * mover holds no lock across it: a call that finds a move in progress asks
 * the mover to go round once more and returns.
 *
 * Knobs for the tests: `stalled` (take nothing, as a line that is not
 * sending), `capture`, `fail_open` (refuse the first open),
 * `refuse_speed` (refuse a configuration with that output speed, as a line
 * that cannot do it). Counters say what the terminal asked of the driver.
 */
#ifndef NXU_KERN_TESTS_TTY_LOOPBACK_H
#define NXU_KERN_TESTS_TTY_LOOPBACK_H

#include <kern/lock.h>
#include <kern/tty/tty.h>

#include <stdbool.h>
#include <stdint.h>

#define TTY_LOOPBACK_CAPTURE_SIZE 1024U

typedef struct tty_loopback {
	tty_t tty;
	nxu_spinlock_t lock;
	bool moving;
	bool again;
	bool stalled;
	bool capture;
	bool fail_open;
	uint32_t refuse_speed;
	uint32_t opens;
	uint32_t closes;
	uint32_t unthrottles;
	uint32_t params;
	bool released;
	uint8_t captured[TTY_LOOPBACK_CAPTURE_SIZE];
	uint32_t captured_length;
} tty_loopback_t;

/* The driver; its context is a tty_loopback_t (or 0, for a terminal the test only names). */
extern const tty_driver_operations_t tty_loopback_operations;

/* Clear `loop` and create its terminal, which the caller then registers. False when memory runs out. */
bool tty_loopback_create(tty_loopback_t *loop);

/* Stop or resume taking output; resuming takes what is waiting. */
void tty_loopback_set_stalled(tty_loopback_t *loop, bool stalled);

/* True when the driver has been given exactly `expected` since the last call; then forget it. */
bool tty_loopback_captured_is(tty_loopback_t *loop, const char *expected);

#endif
