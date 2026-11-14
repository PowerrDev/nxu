/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/tty.h
 *
 * Terminals: the kernel object behind every terminal device, and the
 * interface a terminal driver implements.
 *
 * A terminal (struct tty) sits between two parties that never meet:
 *
 *   - Files. Programs open the terminal's node in /dev and read, write and
 *     ioctl it like any character device. The terminal registers itself with
 *     devfs, so those calls arrive here through the ordinary VFS path.
 *
 *   - A driver. Something moves the bytes to and from the outside world: a
 *     UART, the master side of a pseudo-terminal, a test loopback. The driver
 *     creates the terminal, hands it input as it arrives (tty_input), and
 *     takes output when told there is some (tty_output_take).
 *
 * Between them the terminal keeps two queues. Input waits in the input
 * queue until a program reads it; output waits in the output queue until the
 * driver takes it. Both are bounded. A full output queue makes writers wait;
 * a full input queue refuses more input and the driver holds it back until
 * it is told there is room again (the unthrottle operation).
 *
 * Between the queues and the files sits the line discipline
 * (kern/tty/n_tty.h), configured by the terminal's termios: line editing and
 * echo on the way in, output processing on the way out. Its input buffer is
 * the input queue. A terminal starts with the usual defaults
 * (tty_termios_defaults): canonical input, echo, signals, CR-to-NL in, NL to
 * CR-NL out. A driver whose line must stay byte-exact sets raw modes.
 *
 * Driver model, in the manner of the BSD t_oproc and Linux tty_operations:
 *
 *   - Output is pulled. A write appends to the output queue and then calls
 *     the driver's start operation, which takes what it can with
 *     tty_output_take, now or later. Taking makes room, which wakes writers.
 *
 *   - Input is pushed. tty_input runs it through the line discipline and
 *     says how much was taken. It is called from thread context only, never
 *     from an interrupt handler: echo can call the driver's start operation
 *     before it returns, and a signal character's signal is posted from it.
 *     A driver whose input arrives by interrupt hands it to a thread first.
 *     A driver must not hold a lock of its own that start takes while it
 *     calls tty_input.
 *
 *   - Driver operations are always called with no terminal lock held, from
 *     thread context. They may call any tty_* routine of this file.
 *
 * Lifetime:
 *
 *   - tty_create returns the terminal with one reference, the driver's. The
 *     driver gives it up with tty_release once it will make no more calls on
 *     the terminal (normally right after tty_detach).
 *
 *   - tty_register publishes a name in /dev and holds a reference for it;
 *     tty_unregister removes the name and drops it. Every open file holds one
 *     more.
 *
 *   - When the last reference goes, the driver's release operation is called
 *     with its context, and the terminal is freed. That is the driver's cue to
 *     free whatever the context points to.
 *
 * State, as files see it:
 *
 *   - Open. The first open calls the driver's open operation and the last
 *     close its close operation; the two never overlap, and an open that
 *     arrives while the driver is being opened or closed waits for that to
 *     finish. The last close discards whatever is still queued in either
 *     direction.
 *
 *   - Files. Every open makes a file of its own (struct tty_file), so a
 *     revoke (tty_revoke) can cut the files of one session off from the
 *     terminal while it goes on with the next.
 *
 *   - Hung up (tty_hangup: the line dropped, the other end of a
 *     pseudo-terminal closed). Reads return what is left in the input queue
 *     and then end of file; writes fail with an I/O error; a new open fails
 *     with NO_DEVICE; readiness reports a hangup. The session the terminal
 *     controlled loses it, and its leader and foreground group are sent
 *     SIGHUP and SIGCONT. The terminal stays hung up until its last file
 *     closes, and the next open starts afresh.
 *
 *   - Detached (tty_detach: the driver is going away for good). Hung up, and
 *     every later open fails with NO_DEVICE. The driver's operations other
 *     than release are not started again after tty_detach returns.
 *
 * Locking: each terminal has one spinlock, taken with interrupts masked (the
 * hardware drivers to come may touch the output queue from an interrupt). It
 * covers the queues and
 * the state above, is never held across a driver operation, a sleep, or a
 * readiness post, and is never held together with any other terminal's lock.
 * Readers and writers sleep on their own wait queues with it dropped (the
 * sequence-number pattern of kern/ipc/socket.c).
 */

#ifndef NXU_KERN_TTY_TTY_H
#define NXU_KERN_TTY_TTY_H

#include <kern/process/proc.h>
#include <kern/syscall/termios_defs.h>
#include <vfs/file.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>

struct tty;
typedef struct tty *tty_t;

#define TTY_NULL ((tty_t)0)

/* Capacity of each queue, in bytes. */
#define TTY_INPUT_QUEUE_SIZE 4096U
#define TTY_OUTPUT_QUEUE_SIZE 4096U

/* Longest name a terminal can be registered under in /dev. */
#define TTY_NAME_MAX 31U

/*
 * tty_driver_operations_t
 *
 * Every operation is optional and receives the context given to tty_create.
 *
 * open
 *     The terminal's first file is being opened. A failure fails that open,
 *     and the terminal stays closed.
 *
 * close
 *     The terminal's last file has been closed.
 *
 * start
 *     The output queue has bytes in it. Take them with tty_output_take, now
 *     or whenever the hardware is ready; it is called again after every
 *     write, echo and restart, so a driver that cannot take everything may
 *     simply return. It may be called from inside the driver's own call to
 *     tty_input.
 *
 * unthrottle
 *     The input queue refused bytes, or was filled, and has since been read
 *     from: there is room again (tty_input_room says how much). A driver
 *     may therefore either offer everything and keep what is refused, or
 *     offer only what tty_input_room allows.
 *
 * param
 *     A new configuration is about to take effect (BSD's t_param): set the
 *     line up for it (speed, character size, parity) or refuse it with
 *     INVALID, and the terminal keeps the old one. Called before the change,
 *     with no lock held; a driver with no line to configure leaves it out.
 *
 * ioctl
 *     A request the terminal does not handle itself. Return NOT_TTY for one
 *     the driver does not know either.
 *
 * release
 *     The terminal's last reference is gone; it is being freed and will
 *     never call the driver again. Free the context.
 */
typedef struct tty_driver_operations {
	vfs_status_t (*open)(tty_t tty, void *context);
	void (*close)(tty_t tty, void *context);
	void (*start)(tty_t tty, void *context);
	void (*unthrottle)(tty_t tty, void *context);
	vfs_status_t (*param)(tty_t tty, void *context, const struct nxu_termios *termios);
	vfs_status_t (*ioctl)(tty_t tty, void *context, uint32_t command, void *argument);
	void (*release)(void *context);
} tty_driver_operations_t;

/*
 * tty_init
 *
 * Set up the terminal subsystem. Called once at boot, before any driver
 * creates a terminal.
 */
void tty_init(void);

/*
 * tty_create
 *
 * A new terminal driven by `operations` with `context`, holding one
 * reference for the caller, closed, with empty queues. False when memory
 * runs out.
 */
bool tty_create(const tty_driver_operations_t *operations, void *context, tty_t *result);

/* Take and give up a reference. tty_reference is false for a terminal already being freed. */
bool tty_reference(tty_t tty);
void tty_release(tty_t tty);

/*
 * tty_register / tty_unregister
 *
 * Publish the terminal as /dev/`name` (at most TTY_NAME_MAX bytes, one path
 * component) with the given owner, group and permission bits, and remove it
 * again. A terminal has at most one name. Unregistering does not close the
 * files already open on it.
 */
bool tty_register(tty_t tty, const char *name, uint32_t uid, uint32_t gid, uint32_t mode);
void tty_unregister(tty_t tty);

/*
 * tty_input
 *
 * Bytes arrived from the outside world. Runs them through the line
 * discipline and returns how many it took; if that is fewer than `count`
 * (raw input, buffer full), the driver keeps the rest and offers them again
 * after its unthrottle operation is called. Canonical input never refuses:
 * what does not fit in a full line is dropped. Input to a terminal that is
 * hung up or has no file open is discarded (and counted as taken). Thread
 * context only; see "Driver model".
 */
uint32_t tty_input(tty_t tty, const void *bytes, uint32_t count);

/*
 * tty_input_error
 *
 * One byte that arrived with a line error: a break (the byte the hardware
 * read with it, normally 0), or a byte with a parity or framing error.
 * termios decides what it becomes (kern/tty/n_tty.h, "Line errors"). True
 * when it was taken; false when it must be offered again after unthrottle,
 * as tty_input's refused bytes are. An overrun is not one of these: the byte
 * it comes with is good and goes through tty_input. Thread context only.
 */
typedef enum { TTY_INPUT_BREAK, TTY_INPUT_PARITY, TTY_INPUT_FRAMING } tty_input_error_t;

bool tty_input_error(tty_t tty, uint8_t byte, tty_input_error_t error);

/* How many bytes tty_input is sure to take right now (UINT32_MAX in canonical mode). */
uint32_t tty_input_room(tty_t tty);

/*
 * tty_output_take
 *
 * Move up to `capacity` bytes from the output queue into `buffer`, oldest
 * first; returns how many. Wakes writers waiting for room.
 */
uint32_t tty_output_take(tty_t tty, void *buffer, uint32_t capacity);

/* How many bytes are waiting in the output queue. */
uint32_t tty_output_pending(tty_t tty);

/*
 * tty_hangup / tty_detach
 *
 * The line has dropped, or the driver is going away for good; see "State"
 * above. Every sleeping reader and writer wakes, and readiness reports a
 * hangup. Both may be called more than once.
 */
void tty_hangup(tty_t tty);
void tty_detach(tty_t tty);

/*
 * tty_revoke / tty_revoke_vnode
 *
 * Darwin's revoke(2) for a terminal: every file open on it now is cut off
 * from it for good. Reads on those files return end of file and writes fail
 * with an I/O error (as on a dead vnode); their requests are NOT_TTY, so
 * they are no terminal any more; a sleeper on one wakes to find it so. The
 * terminal itself is as if they had all been closed (the driver is closed,
 * the queues are emptied) and its session is told as for a hangup: the
 * session loses it, and its leader is sent SIGHUP. The next open starts a
 * new generation of files, which the old ones are not part of, however
 * long they stay open. getty revokes the console after each session, so a
 * job the user left behind cannot read the next user's typing.
 *
 * tty_revoke_vnode is the system call's way in: false when `vnode` is not
 * a terminal's name; otherwise *status says how the revoke went.
 */
void tty_revoke(tty_t tty);
bool tty_revoke_vnode(vnode_t vnode, vfs_status_t *status);

/*
 * tty_termios_defaults / tty_get_termios / tty_set_termios
 *
 * The configuration a new terminal starts with (Darwin's ttydefaults, plus
 * IUTF8), and reading and changing a terminal's. A change is first offered
 * to the driver (its param operation), which may refuse it; it then applies
 * at once, to input already queued as well (kern/tty/n_tty.h,
 * n_tty_set_termios), and wakes readers, whose wait condition it may have
 * changed. With CREAD off, input is discarded as it arrives.
 */
void tty_termios_defaults(struct nxu_termios *termios);
void tty_get_termios(tty_t tty, struct nxu_termios *termios);
vfs_status_t tty_set_termios(tty_t tty, const struct nxu_termios *termios);

/*
 * tty_get_winsize / tty_set_winsize
 *
 * The window size, zero until someone sets it. Setting a different one
 * records SIGWINCH for the foreground process group (tty_take_signals).
 */
void tty_get_winsize(tty_t tty, struct nxu_winsize *size);
void tty_set_winsize(tty_t tty, const struct nxu_winsize *size);

/*
 * tty_take_signals
 *
 * The signals (1 << NXU_SIG*) the signal characters have asked for since
 * the last call, and forget them. Delivering them to the foreground process
 * group is job control's part; until then they wait here.
 */
uint32_t tty_take_signals(tty_t tty);

/*
 * Job control (kern/tty/tty_jobctl.c), the half the process subsystem and
 * the loader call. The model and the lock order are in kern/process/pgrp.h.
 *
 * tty_claim_locked
 *     Become the terminal of session sid with pgid in the foreground, if no
 *     session has it. Called by kern/process/pgrp.c with the process table
 *     lock held; takes the terminal's.
 *
 * tty_session_gone / tty_session_leader_exit
 *     Session sid no longer exists, or its leader has exited: the terminal
 *     forgets it, and after a leader's exit its foreground group is sent
 *     SIGHUP and SIGCONT. No lock held.
 *
 * tty_job_acquire_for / tty_job_set_foreground_for
 *     What the spawn attributes ask for: `tty` becomes the controlling
 *     terminal of the session proc leads; proc's controlling terminal gets
 *     pgid (a group of proc's session) in the foreground.
 *
 * tty_from_file
 *     The terminal an open file refers to, with a reference, or TTY_NULL.
 */
bool tty_claim_locked(tty_t tty, proc_id_t sid, proc_id_t pgid);
void tty_session_gone(tty_t tty, proc_id_t sid);
void tty_session_leader_exit(tty_t tty, proc_id_t sid);
bool tty_job_acquire_for(proc_t proc, tty_t tty);
vfs_status_t tty_job_set_foreground_for(proc_t proc, proc_id_t pgid);
tty_t tty_from_file(file_t file);

/* How many terminals exist (diagnostics and tests: a terminal that is never freed shows here). */
uint32_t tty_count(void);

#endif
