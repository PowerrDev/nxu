/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/tty_loopback.c
 *
 * The loopback test driver. See kern/tests/tty_loopback.h.
 */

#include <kern/tests/tty_loopback.h>

#include <kern/lock.h>
#include <kern/tty/tty.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void
loop_move_once(tty_loopback_t *loop, bool capture)
{
	uint8_t chunk[256];

	for (;;) {
		uint32_t room = capture ? TTY_LOOPBACK_CAPTURE_SIZE - loop->captured_length : tty_input_room(loop->tty);
		if (room == 0U) return;

		uint32_t want = room < sizeof(chunk) ? room : (uint32_t)sizeof(chunk);
		uint32_t count = tty_output_take(loop->tty, chunk, want);
		if (count == 0U) return;

		if (capture) {
			memcpy(loop->captured + loop->captured_length, chunk, count);
			loop->captured_length += count;
		} else {
			(void)tty_input(loop->tty, chunk, count);
		}
	}
}

static void
loop_move(tty_loopback_t *loop)
{
	nxu_spin_lock(&loop->lock);

	if (loop->moving) {
		loop->again = true;
		nxu_spin_unlock(&loop->lock);
		return;
	}

	loop->moving = true;

	do {
		loop->again = false;

		bool stalled = loop->stalled;
		bool capture = loop->capture;

		nxu_spin_unlock(&loop->lock);
		if (!stalled) loop_move_once(loop, capture);
		nxu_spin_lock(&loop->lock);
	} while (loop->again);

	loop->moving = false;
	nxu_spin_unlock(&loop->lock);
}

static vfs_status_t
loop_open(tty_t tty, void *context)
{
	(void)tty;

	tty_loopback_t *loop = context;

	if (loop->fail_open) return VFS_STATUS_BUSY;

	loop->opens++;
	return VFS_STATUS_OK;
}

static void
loop_close(tty_t tty, void *context)
{
	(void)tty;
	((tty_loopback_t *)context)->closes++;
}

static void
loop_start(tty_t tty, void *context)
{
	(void)tty;
	loop_move(context);
}

static void
loop_unthrottle(tty_t tty, void *context)
{
	(void)tty;

	tty_loopback_t *loop = context;

	loop->unthrottles++;
	loop_move(loop);
}

static vfs_status_t
loop_param(tty_t tty, void *context, const struct nxu_termios *termios)
{
	(void)tty;

	tty_loopback_t *loop = context;

	loop->params++;

	if (loop->refuse_speed != 0U && termios->c_ospeed == loop->refuse_speed) return VFS_STATUS_INVALID;

	return VFS_STATUS_OK;
}

static void
loop_release(void *context)
{
	if (context != 0) ((tty_loopback_t *)context)->released = true;
}

const tty_driver_operations_t tty_loopback_operations = {.open = loop_open,
														 .close = loop_close,
														 .start = loop_start,
														 .unthrottle = loop_unthrottle,
														 .param = loop_param,
														 .release = loop_release};

bool
tty_loopback_create(tty_loopback_t *loop)
{
	memset(loop, 0, sizeof(*loop));
	return tty_create(&tty_loopback_operations, loop, &loop->tty);
}

void
tty_loopback_set_stalled(tty_loopback_t *loop, bool stalled)
{
	nxu_spin_lock(&loop->lock);
	loop->stalled = stalled;
	nxu_spin_unlock(&loop->lock);

	if (!stalled) loop_move(loop);
}

bool
tty_loopback_captured_is(tty_loopback_t *loop, const char *expected)
{
	size_t length = strlen(expected);
	bool match = loop->captured_length == length && memcmp(loop->captured, expected, length) == 0;

	loop->captured_length = 0U;
	return match;
}
