/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/tty_ioctl.c
 *
 * The ioctl requests a terminal answers itself (kern/syscall/termios_defs.h),
 * and the hand-off of every other request to its driver. Each request is a
 * thin translation onto an operation of kern/tty/tty.c, which owns the
 * state; nothing here touches the terminal except to read a count.
 *
 * The file layer has already copied the argument in, sized by the request
 * number, and copies it back out after a success; each request here reads
 * or fills a kernel buffer of exactly that type. A request neither the
 * terminal nor its driver knows fails with NOT_TTY, the answer POSIX gives
 * for an ioctl that does not apply to the device.
 *
 * Locking: queries take the terminal's lock just long enough to read; the
 * driver is called with no lock held, and not at all once it has detached.
 * TIOCSETAW, TIOCSETAF and TIOCDRAIN sleep, with no lock held.
 *
 * Not yet: the background-process rules (a process outside the foreground
 * group that changes the settings is stopped with SIGTTOU) come with job
 * control, which is what gives a terminal a foreground group.
 */

#include <kern/tty/tty_internal.h>

#include <kern/syscall/termios_defs.h>

#include <stdbool.h>
#include <stdint.h>

vfs_status_t
tty_ioctl(tty_t tty, uint32_t command, void *argument)
{
	if (tty == TTY_NULL) return VFS_STATUS_INVALID;

	uint64_t state;
	vfs_status_t status;

	switch (command) {
	case NXU_TIOCGETA:
		tty_get_termios(tty, argument);
		return VFS_STATUS_OK;

	case NXU_TIOCSETA:
		return tty_set_termios(tty, argument);

	case NXU_TIOCSETAW:
	case NXU_TIOCSETAF:
		/* Output written under the old settings goes out under them. */
		status = tty_drain(tty);
		if (status != VFS_STATUS_OK) return status;

		return tty_change_termios(tty, argument, command == NXU_TIOCSETAF);

	case NXU_TIOCFLUSH: {
		uint32_t which = *(const uint32_t *)argument;

		if ((which & ~(NXU_FREAD | NXU_FWRITE)) != 0U) return VFS_STATUS_INVALID;

		tty_flush(tty, which == 0U ? NXU_FREAD | NXU_FWRITE : which);
		return VFS_STATUS_OK;
	}

	case NXU_TIOCDRAIN:
		return tty_drain(tty);

	case NXU_TIOCSTOP:
	case NXU_TIOCSTART:
		tty_stop_output(tty, command == NXU_TIOCSTOP);
		return VFS_STATUS_OK;

	case NXU_TIOCGWINSZ:
		tty_get_winsize(tty, argument);
		return VFS_STATUS_OK;

	case NXU_TIOCSWINSZ:
		tty_set_winsize(tty, argument);
		return VFS_STATUS_OK;

	case NXU_FIONREAD:
		state = tty_lock(tty);
		*(uint32_t *)argument = n_tty_available(&tty->t_ldisc);
		tty_unlock(tty, state);
		return VFS_STATUS_OK;

	case NXU_TIOCOUTQ:
		state = tty_lock(tty);
		*(uint32_t *)argument = tty->t_output.count;
		tty_unlock(tty, state);
		return VFS_STATUS_OK;

	default:
		break;
	}

	state = tty_lock(tty);
	bool call = tty->t_driver->ioctl != 0 && !tty->t_detached;
	tty_unlock(tty, state);

	if (!call) return VFS_STATUS_NOT_TTY;

	return tty->t_driver->ioctl(tty, tty->t_driver_context, command, argument);
}
