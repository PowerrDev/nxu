/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/pty.h
 *
 * Pseudo-terminals: a terminal whose "line" is a file held by another
 * program. Opening /dev/ptmx makes a pair: the opener's file is the master,
 * and the slave appears as /dev/ttysNNN, an ordinary terminal (kern/tty/tty.h)
 * that a shell and its jobs use as their controlling terminal. What the
 * master writes is the slave's input, typed at its keyboard; what the slave's
 * programs write, after output processing, is what the master reads.
 *
 * The sequence a terminal emulator follows is the POSIX one, with Darwin's
 * requests behind it (kern/syscall/termios_defs.h): open /dev/ptmx,
 * TIOCPTYGRANT, TIOCPTYUNLK, TIOCPTYGNAME for the slave's name, open it, and
 * hand that to the child as its standard descriptors and controlling
 * terminal. The slave refuses to open before TIOCPTYUNLK.
 *
 * When the master closes, the pair is over: the slave hangs up for good (its
 * session hears SIGHUP), its name goes, and it is freed when its last file
 * closes. When the slave's last file closes, the master reads what is left
 * and then end of file.
 */

#ifndef NXU_KERN_TTY_PTY_H
#define NXU_KERN_TTY_PTY_H

#include <stdbool.h>
#include <stdint.h>

/* How many pairs may exist at once. Each takes two devfs nodes (the master's and the slave's). */
#define PTY_MAX 32U

/* The group a slave belongs to (Darwin's "tty"). */
#define PTY_TTY_GID 4U

/*
 * pty_init
 *
 * Publish /dev/ptmx. Called once by tty_init.
 */
bool pty_init(void);

/* How many pairs exist (diagnostics and tests: a pair that is never freed shows here). */
uint32_t pty_count(void);

#endif
