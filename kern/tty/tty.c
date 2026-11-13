/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/tty.c
 *
 * Terminal objects: lifetime, the queues, the line discipline's place
 * between them and the files, and the file operations that devfs routes to a
 * terminal. See kern/tty/tty.h for the model and the driver interface, and
 * kern/tty/n_tty.h for the discipline.
 *
 * Reference counting: tty_create hands back the driver's reference.
 * tty_register takes one for the name, which tty_unregister gives back after
 * the name is gone from devfs. A file's reference is taken by the open
 * operation before anything else, so a terminal cannot be freed under an
 * open that sleeps, and dropped by its close operation after everything
 * else. tty_release frees the terminal at zero; nothing can find it then: the
 * name is gone (it held a reference), no file is open (each held one), and
 * the driver has given up its own.
 *
 * Opening a name and unregistering it are both reached only from the VFS,
 * which is serialized (doc/architecture/tty-subsystem.md section 9), so the
 * registration's reference is still there when devfs calls the open
 * operation. devfs refuses an open through a vnode whose name is gone.
 *
 * Locking: one spinlock per terminal (t_lock), irqsave; see tty.h. Every
 * sleep follows the
 * pattern of kern/ipc/socket.c: test the condition under the lock, take the
 * wait queue's sequence number while still holding it, drop it, and sleep only
 * if no wakeup has moved the sequence since. Every waker changes the condition
 * under the lock and then wakes. Wait-queue wakeups are made with t_lock held
 * (the queue's own lock nests inside it); readiness posts and driver
 * operations are made after it is dropped.
 *
 * Driver operations: called with no lock held. Each call site reads
 * t_detached under the lock just before, so none starts after tty_detach has
 * returned; one already started may still be running.
 *
 * The line discipline runs entirely under t_lock. Its hooks only touch the
 * output queue and t_lock-protected fields, and record what must happen once
 * the lock is dropped (start the driver, post readiness) in t_deferred;
 * every path that calls into the discipline takes that record with
 * tty_take_deferred before unlocking and acts on it with tty_run_deferred
 * after.
 *
 * Reads apply VMIN and VTIME (non-canonical mode) here, since only the core
 * can sleep: a deadline on the readers' wait queue (waitq_block_deadline)
 * measures VTIME, from the start of the read when VMIN is 0, from the last
 * byte received otherwise.
 */

#include <kern/tty/tty_internal.h>

#include <kern/console/console.h>
#include <kern/ioready.h>
#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <kern/memory/heap.h>
#include <kern/sched_prism/waitq.h>
#include <kern/syscall/syscall_defs.h>
#include <kern/process/signal.h>
#include <kern/syscall/termios_defs.h>
#include <vfs/devfs.h>
#include <vfs/file.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static nxu_spinlock_t g_tty_count_lock = NXU_SPINLOCK_INIT;
static uint32_t g_tty_count;
static bool g_tty_initialized;

static vfs_status_t tty_devfs_open(void *context, uint32_t flags, devfs_clone_t *clone);
static void tty_devfs_close(void *context);
static vfs_status_t tty_devfs_read(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size);
static vfs_status_t tty_devfs_write(void *context, const void *buffer, uint64_t size, uint32_t ioflags,
									uint64_t *written_size);
static vfs_status_t tty_devfs_ioctl(void *context, uint32_t command, void *argument);
static vfs_status_t tty_devfs_poll(void *context, uint32_t *ready, struct ioready **source);

const devfs_operations_t g_tty_devfs_operations = {.open = tty_devfs_open,
												   .close = tty_devfs_close,
												   .read = tty_devfs_read,
												   .write = tty_devfs_write,
												   .ioctl = tty_devfs_ioctl,
												   .poll = tty_devfs_poll};

/* ---- queues ------------------------------------------------------------- */

static bool
tty_queue_alloc(tty_queue_t *queue, uint32_t size)
{
	queue->data = kmalloc(size);
	queue->size = size;
	queue->head = 0U;
	queue->count = 0U;
	return queue->data != 0;
}

static void
tty_queue_free(tty_queue_t *queue)
{
	if (queue->data != 0) (void)kfree(queue->data);

	queue->data = 0;
}

static uint32_t
tty_queue_room(const tty_queue_t *queue)
{
	return queue->size - queue->count;
}

/* Append up to `count` bytes; returns how many fit. */
static uint32_t
tty_queue_put(tty_queue_t *queue, const uint8_t *bytes, uint32_t count)
{
	uint32_t total = count < tty_queue_room(queue) ? count : tty_queue_room(queue);
	uint32_t done = 0U;

	while (done < total) {
		uint32_t tail = (queue->head + queue->count) % queue->size;
		uint32_t contiguous = queue->size - tail;
		uint32_t chunk = total - done < contiguous ? total - done : contiguous;

		memcpy(queue->data + tail, bytes + done, chunk);
		queue->count += chunk;
		done += chunk;
	}

	return total;
}

/* Remove up to `capacity` bytes, oldest first; returns how many. */
static uint32_t
tty_queue_take(tty_queue_t *queue, uint8_t *buffer, uint32_t capacity)
{
	uint32_t total = capacity < queue->count ? capacity : queue->count;
	uint32_t done = 0U;

	while (done < total) {
		uint32_t contiguous = queue->size - queue->head;
		uint32_t chunk = total - done < contiguous ? total - done : contiguous;

		memcpy(buffer + done, queue->data + queue->head, chunk);
		queue->head = (queue->head + chunk) % queue->size;
		queue->count -= chunk;
		done += chunk;
	}

	return total;
}

static void
tty_queue_flush(tty_queue_t *queue)
{
	queue->head = 0U;
	queue->count = 0U;
}

/* ---- the line discipline's hooks (called with t_lock held) ------------- */

static uint32_t
tty_hook_output_room(void *context)
{
	tty_t tty = context;

	return tty_queue_room(&tty->t_output);
}

static void
tty_hook_output(void *context, const uint8_t *bytes, uint32_t count)
{
	tty_t tty = context;

	(void)tty_queue_put(&tty->t_output, bytes, count);
	tty->t_deferred |= TTY_DEFER_START;
}

static void
tty_hook_output_flush(void *context)
{
	tty_t tty = context;

	tty_queue_flush(&tty->t_output);
	waitq_wake_all(&tty->t_writers);
	tty->t_deferred |= TTY_DEFER_POST_WRITE;
}

static void
tty_hook_signal(void *context, n_tty_signal_t signal)
{
	tty_t tty = context;
	uint32_t number = signal == N_TTY_SIGNAL_INTERRUPT ? NXU_SIGINT
					: signal == N_TTY_SIGNAL_QUIT	   ? NXU_SIGQUIT
													   : NXU_SIGTSTP;

	tty->t_signals |= 1U << number;
	tty->t_deliver |= 1U << number;
}

static void
tty_hook_flow(void *context, bool stopped)
{
	tty_t tty = context;

	tty->t_stopped = stopped;

	/* Whatever waited while output was stopped can go now. */
	if (!stopped) tty->t_deferred |= TTY_DEFER_START;
}

static const n_tty_hooks_t g_tty_ldisc_hooks = {.output_room = tty_hook_output_room,
												.output = tty_hook_output,
												.output_flush = tty_hook_output_flush,
												.signal = tty_hook_signal,
												.flow = tty_hook_flow};

/*
 * Take what the hooks recorded; t_lock held. A detached driver is not
 * started. Signals go to the foreground group as it is now; with none, they
 * go nowhere (they are still counted in t_signals).
 */
static void
tty_take_deferred(tty_t tty, tty_deferred_t *deferred)
{
	deferred->work = tty->t_deferred;
	deferred->signals = tty->t_pgrp != 0U ? tty->t_deliver : 0U;
	deferred->pgrp = tty->t_pgrp;

	tty->t_deferred = 0U;
	tty->t_deliver = 0U;

	if (tty->t_detached || tty->t_driver->start == 0) deferred->work &= ~TTY_DEFER_START;
	if (tty->t_detached || tty->t_driver->unthrottle == 0) deferred->work &= ~TTY_DEFER_UNTHROTTLE;
}

static bool
tty_deferred_pending(const tty_deferred_t *deferred)
{
	return deferred->work != 0U || deferred->signals != 0U;
}

/* Do it, with no lock held: driver calls, readiness, then the signals (rule 6.3: never under a terminal lock). */
static void
tty_run_deferred(tty_t tty, const tty_deferred_t *deferred)
{
	uint32_t work = deferred->work;

	if ((work & TTY_DEFER_START) != 0U) tty->t_driver->start(tty, tty->t_driver_context);
	if ((work & TTY_DEFER_UNTHROTTLE) != 0U) tty->t_driver->unthrottle(tty, tty->t_driver_context);
	if ((work & TTY_DEFER_POST_READ) != 0U) ioready_post(&tty->t_ready, IOREADY_READ);
	if ((work & TTY_DEFER_POST_WRITE) != 0U) ioready_post(&tty->t_ready, IOREADY_WRITE);

	for (uint32_t signal = 1U; signal < NXU_NSIG; signal++) {
		if ((deferred->signals & (1U << signal)) != 0U) (void)signal_post_group(deferred->pgrp, signal);
	}
}

/* The discipline may have made input readable, or changed what a reader waits for: wake them. */
static void
tty_wake_readers_locked(tty_t tty)
{
	waitq_wake_all(&tty->t_readers);
	if (n_tty_readable(&tty->t_ldisc)) tty->t_deferred |= TTY_DEFER_POST_READ;
}

/* ---- termios ------------------------------------------------------------ */

void
tty_termios_defaults(struct nxu_termios *termios)
{
	memset(termios, 0, sizeof(*termios));

	termios->c_iflag = NXU_TTYDEF_IFLAG;
	termios->c_oflag = NXU_TTYDEF_OFLAG;
	termios->c_cflag = NXU_TTYDEF_CFLAG;
	termios->c_lflag = NXU_TTYDEF_LFLAG;

	termios->c_cc[NXU_VEOF] = NXU_CEOF;
	termios->c_cc[NXU_VEOL] = NXU_CEOL;
	termios->c_cc[NXU_VEOL2] = NXU_CEOL2;
	termios->c_cc[NXU_VERASE] = NXU_CERASE;
	termios->c_cc[NXU_VWERASE] = NXU_CWERASE;
	termios->c_cc[NXU_VKILL] = NXU_CKILL;
	termios->c_cc[NXU_VREPRINT] = NXU_CREPRINT;
	termios->c_cc[7] = NXU_POSIX_VDISABLE;
	termios->c_cc[NXU_VINTR] = NXU_CINTR;
	termios->c_cc[NXU_VQUIT] = NXU_CQUIT;
	termios->c_cc[NXU_VSUSP] = NXU_CSUSP;
	termios->c_cc[NXU_VDSUSP] = NXU_CDSUSP;
	termios->c_cc[NXU_VSTART] = NXU_CSTART;
	termios->c_cc[NXU_VSTOP] = NXU_CSTOP;
	termios->c_cc[NXU_VLNEXT] = NXU_CLNEXT;
	termios->c_cc[NXU_VDISCARD] = NXU_CDISCARD;
	termios->c_cc[NXU_VMIN] = NXU_CMIN;
	termios->c_cc[NXU_VTIME] = NXU_CTIME;
	termios->c_cc[NXU_VSTATUS] = NXU_CSTATUS;
	termios->c_cc[19] = NXU_POSIX_VDISABLE;

	termios->c_ispeed = NXU_TTYDEF_SPEED;
	termios->c_ospeed = NXU_TTYDEF_SPEED;
}

void
tty_get_termios(tty_t tty, struct nxu_termios *termios)
{
	uint64_t state = tty_lock(tty);
	*termios = tty->t_ldisc.termios;
	tty_unlock(tty, state);
}

/* Discard all input; a driver that was holding input back may send it now. t_lock held. */
static void
tty_flush_input_locked(tty_t tty)
{
	n_tty_flush_input(&tty->t_ldisc);

	if (tty->t_throttled) {
		tty->t_throttled = false;
		tty->t_deferred |= TTY_DEFER_UNTHROTTLE;
	}
}

/* Discard all output the driver has not taken; writers and drainers look again. t_lock held. */
static void
tty_flush_output_locked(tty_t tty)
{
	tty_queue_flush(&tty->t_output);
	waitq_wake_all(&tty->t_writers);
	tty->t_deferred |= TTY_DEFER_POST_WRITE;
}

vfs_status_t
tty_set_termios(tty_t tty, const struct nxu_termios *termios)
{
	return tty_change_termios(tty, termios, false);
}

/*
 * tty_change_termios
 *
 * The driver is asked first, with no lock held, and a refusal changes
 * nothing. Two changes racing each other are each validated and applied
 * whole; the later one wins.
 */
vfs_status_t
tty_change_termios(tty_t tty, const struct nxu_termios *termios, bool flush_input)
{
	uint64_t state = tty_lock(tty);
	bool ask = tty->t_driver->param != 0 && !tty->t_detached;
	tty_unlock(tty, state);

	if (ask) {
		vfs_status_t status = tty->t_driver->param(tty, tty->t_driver_context, termios);

		if (status != VFS_STATUS_OK) return status;
	}

	state = tty_lock(tty);

	if (flush_input) tty_flush_input_locked(tty);

	n_tty_set_termios(&tty->t_ldisc, termios);
	tty_wake_readers_locked(tty);

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);

	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);
	return VFS_STATUS_OK;
}

void
tty_flush(tty_t tty, uint32_t which)
{
	uint64_t state = tty_lock(tty);

	if ((which & NXU_FREAD) != 0U) tty_flush_input_locked(tty);
	if ((which & NXU_FWRITE) != 0U) tty_flush_output_locked(tty);

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);

	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);
}

/*
 * tty_drain
 *
 * "Taken by the driver" is all the terminal can know; a driver with a
 * hardware FIFO of its own may still be sending the last bytes. Stopped
 * output (VSTOP) keeps a drain waiting, as it should: nothing has gone out.
 * Writers' wakeups are also the drainers': both wait for the output queue
 * to change.
 */
vfs_status_t
tty_drain(tty_t tty)
{
	uint64_t state = tty_lock(tty);

	while (tty->t_output.count != 0U && !tty->t_hungup) {
		uint32_t seq = waitq_seq(&tty->t_writers);
		tty_unlock(tty, state);

		if (waitq_block_deadline(&tty->t_writers, seq, true, WAITQ_NO_DEADLINE) == WAITQ_INTERRUPTED)
			return VFS_STATUS_INTERRUPTED;

		state = tty_lock(tty);
	}

	tty_unlock(tty, state);
	return VFS_STATUS_OK;
}

/* The same state VSTOP and VSTART change, so either restarts what the other stopped. */
void
tty_stop_output(tty_t tty, bool stop)
{
	uint64_t state = tty_lock(tty);

	tty->t_stopped = stop;
	tty->t_ldisc.stopped = stop;
	if (!stop) tty->t_deferred |= TTY_DEFER_START;

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);

	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);
}

void
tty_get_winsize(tty_t tty, struct nxu_winsize *size)
{
	uint64_t state = tty_lock(tty);
	*size = tty->t_winsize;
	tty_unlock(tty, state);
}

void
tty_set_winsize(tty_t tty, const struct nxu_winsize *size)
{
	uint64_t state = tty_lock(tty);

	bool changed = tty->t_winsize.ws_row != size->ws_row || tty->t_winsize.ws_col != size->ws_col ||
				   tty->t_winsize.ws_xpixel != size->ws_xpixel || tty->t_winsize.ws_ypixel != size->ws_ypixel;

	tty->t_winsize = *size;

	if (changed) {
		tty->t_signals |= 1U << NXU_SIGWINCH;
		tty->t_deliver |= 1U << NXU_SIGWINCH;
	}

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);
	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);
}

uint32_t
tty_take_signals(tty_t tty)
{
	uint64_t state = tty_lock(tty);
	uint32_t signals = tty->t_signals;

	tty->t_signals = 0U;
	tty_unlock(tty, state);

	return signals;
}

/* ---- lifetime ----------------------------------------------------------- */

void
tty_init(void)
{
	kprintf("initializing tty\n");

	nxu_spin_lock(&g_tty_count_lock);
	g_tty_count = 0U;
	g_tty_initialized = true;
	nxu_spin_unlock(&g_tty_count_lock);

	if (!tty_jobctl_init()) {
		kprintf("cannot publish /dev/tty\n");
		return;
	}

	kprintf("initialized tty successfully\n");
}

bool
tty_create(const tty_driver_operations_t *operations, void *context, tty_t *result)
{
	if (result != 0) *result = TTY_NULL;

	if (operations == 0 || result == 0 || !g_tty_initialized) return false;

	tty_t tty = kmalloc(sizeof(struct tty));
	if (tty == 0) return false;

	*tty = (struct tty){0};
	tty->t_driver = operations;
	tty->t_driver_context = context;
	tty->t_references = 1U;
	ioready_init(&tty->t_ready);
	waitq_init(&tty->t_readers);
	waitq_init(&tty->t_writers);
	waitq_init(&tty->t_state);

	tty->t_input_buffer = kmalloc(TTY_INPUT_QUEUE_SIZE);
	tty->t_input_delimiters = kmalloc(TTY_INPUT_QUEUE_SIZE / 8U);

	if (tty->t_input_buffer == 0 || tty->t_input_delimiters == 0 ||
		!tty_queue_alloc(&tty->t_output, TTY_OUTPUT_QUEUE_SIZE)) {
		if (tty->t_input_buffer != 0) (void)kfree(tty->t_input_buffer);
		if (tty->t_input_delimiters != 0) (void)kfree(tty->t_input_delimiters);
		tty_queue_free(&tty->t_output);
		(void)kfree(tty);
		return false;
	}

	struct nxu_termios termios;

	tty_termios_defaults(&termios);
	n_tty_init(&tty->t_ldisc, &g_tty_ldisc_hooks, tty, tty->t_input_buffer, tty->t_input_delimiters,
			   TTY_INPUT_QUEUE_SIZE, &termios);

	nxu_spin_lock(&g_tty_count_lock);
	g_tty_count++;
	nxu_spin_unlock(&g_tty_count_lock);

	*result = tty;
	return true;
}

bool
tty_reference(tty_t tty)
{
	if (tty == TTY_NULL) return false;

	uint64_t state = tty_lock(tty);
	bool valid = tty->t_references != 0U && tty->t_references != UINT32_MAX;
	if (valid) tty->t_references++;
	tty_unlock(tty, state);

	return valid;
}

/*
 * tty_release
 *
 * At zero nobody else can reach the terminal (see the file header), so
 * teardown needs no lock: whatever is still attached to the notifier is told
 * the terminal hung up, the driver frees its context, and the memory goes.
 */
void
tty_release(tty_t tty)
{
	if (tty == TTY_NULL) return;

	uint64_t state = tty_lock(tty);
	bool destroy = false;

	if (tty->t_references != 0U) {
		tty->t_references--;
		destroy = tty->t_references == 0U;
	}

	tty_unlock(tty, state);

	if (!destroy) return;

	ioready_destroy(&tty->t_ready);

	if (tty->t_driver->release != 0) tty->t_driver->release(tty->t_driver_context);

	(void)kfree(tty->t_input_buffer);
	(void)kfree(tty->t_input_delimiters);
	tty_queue_free(&tty->t_output);
	(void)kfree(tty);

	nxu_spin_lock(&g_tty_count_lock);
	g_tty_count--;
	nxu_spin_unlock(&g_tty_count_lock);
}

uint32_t
tty_count(void)
{
	nxu_spin_lock(&g_tty_count_lock);
	uint32_t count = g_tty_count;
	nxu_spin_unlock(&g_tty_count_lock);

	return count;
}

/* ---- names -------------------------------------------------------------- */

bool
tty_register(tty_t tty, const char *name, uint32_t uid, uint32_t gid, uint32_t mode)
{
	if (tty == TTY_NULL || name == 0) return false;

	size_t length = strlen(name);
	if (length == 0U || length > TTY_NAME_MAX) return false;

	/* The name's reference, taken before the name can be found. */
	if (!tty_reference(tty)) return false;

	uint64_t state = tty_lock(tty);
	bool claimed = !tty->t_registered && !tty->t_detached;

	if (claimed) {
		tty->t_registered = true;
		memcpy(tty->t_name, name, length + 1U);
	}

	tty_unlock(tty, state);

	if (claimed && devfs_register_device_owned(name, &g_tty_devfs_operations, tty, uid, gid, mode)) return true;

	if (claimed) {
		state = tty_lock(tty);
		tty->t_registered = false;
		tty->t_name[0] = '\0';
		tty_unlock(tty, state);
	}

	tty_release(tty);
	return false;
}

void
tty_unregister(tty_t tty)
{
	if (tty == TTY_NULL) return;

	char name[TTY_NAME_MAX + 1U];
	uint64_t state = tty_lock(tty);
	bool registered = tty->t_registered;

	if (registered) {
		memcpy(name, tty->t_name, sizeof(name));
		tty->t_registered = false;
		tty->t_name[0] = '\0';
	}

	tty_unlock(tty, state);

	if (!registered) return;

	(void)devfs_unregister_device(name);
	tty_release(tty);
}

/* ---- the driver's side -------------------------------------------------- */

uint32_t
tty_input(tty_t tty, const void *bytes, uint32_t count)
{
	if (tty == TTY_NULL || bytes == 0 || count == 0U) return 0U;

	uint64_t state = tty_lock(tty);

	/* Nobody to read it, nobody ever will, or the receiver is off: taken and dropped, so the driver does not hold it back. */
	if (tty->t_opens == 0U || tty->t_hungup || (tty->t_ldisc.termios.c_cflag & NXU_CREAD) == 0U) {
		tty_unlock(tty, state);
		return count;
	}

	uint32_t received = tty->t_ldisc.received;
	uint32_t taken = n_tty_receive(&tty->t_ldisc, bytes, count);

	/* Refused, or full now: either way the driver hears when reading makes room. */
	if (taken < count || n_tty_receive_room(&tty->t_ldisc) == 0U) tty->t_throttled = true;

	/* A byte stored can satisfy VMIN or restart a VTIME timer, even before a line is complete. */
	if (tty->t_ldisc.received != received || n_tty_readable(&tty->t_ldisc)) tty_wake_readers_locked(tty);

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);

	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);

	return taken;
}

uint32_t
tty_input_room(tty_t tty)
{
	if (tty == TTY_NULL) return 0U;

	uint64_t state = tty_lock(tty);
	uint32_t room = n_tty_receive_room(&tty->t_ldisc);
	tty_unlock(tty, state);

	return room;
}

uint32_t
tty_output_take(tty_t tty, void *buffer, uint32_t capacity)
{
	if (tty == TTY_NULL || buffer == 0 || capacity == 0U) return 0U;

	uint64_t state = tty_lock(tty);
	uint32_t taken = tty->t_stopped ? 0U : tty_queue_take(&tty->t_output, buffer, capacity);

	if (taken != 0U) waitq_wake_all(&tty->t_writers);

	tty_unlock(tty, state);

	if (taken != 0U) ioready_post(&tty->t_ready, IOREADY_WRITE);

	return taken;
}

uint32_t
tty_output_pending(tty_t tty)
{
	if (tty == TTY_NULL) return 0U;

	uint64_t state = tty_lock(tty);
	uint32_t pending = tty->t_output.count;
	tty_unlock(tty, state);

	return pending;
}

/* Hang up, and for good if `detach`: every sleeper wakes to find out. */
static void
tty_hangup_internal(tty_t tty, bool detach)
{
	if (tty == TTY_NULL) return;

	uint64_t state = tty_lock(tty);

	tty->t_hungup = true;
	if (detach) tty->t_detached = true;

	waitq_wake_all(&tty->t_readers);
	waitq_wake_all(&tty->t_writers);
	waitq_wake_all(&tty->t_state);

	tty_unlock(tty, state);

	ioready_post(&tty->t_ready, IOREADY_HANGUP | IOREADY_READ | IOREADY_WRITE);

	/* The session loses its terminal; its leader and foreground group hear of it. */
	tty_hangup_session(tty);
}

void
tty_hangup(tty_t tty)
{
	tty_hangup_internal(tty, false);
}

void
tty_detach(tty_t tty)
{
	tty_hangup_internal(tty, true);
}

/* ---- the files' side ---------------------------------------------------- */

/*
 * tty_devfs_open
 *
 * The file's reference first, so nothing below can lose the terminal. Then
 * wait out a driver open or close in progress; then either join the files
 * already open, or be the first and open the driver, with t_transition
 * keeping everyone else out until it answers. A terminal whose first open
 * finds it hung up by a previous session starts afresh.
 */
vfs_status_t
tty_file_open(tty_t tty, uint32_t flags)
{
	(void)flags;

	if (!tty_reference(tty)) return VFS_STATUS_NO_DEVICE;

	uint64_t state = tty_lock(tty);

	while (tty->t_transition && !tty->t_detached) {
		uint32_t seq = waitq_seq(&tty->t_state);
		tty_unlock(tty, state);

		if (waitq_block_deadline(&tty->t_state, seq, true, WAITQ_NO_DEADLINE) == WAITQ_INTERRUPTED) {
			tty_release(tty);
			return VFS_STATUS_INTERRUPTED;
		}

		state = tty_lock(tty);
	}

	if (tty->t_detached || (tty->t_hungup && tty->t_opens != 0U)) {
		tty_unlock(tty, state);
		tty_release(tty);
		return VFS_STATUS_NO_DEVICE;
	}

	if (tty->t_opens != 0U) {
		tty->t_opens++;
		tty_unlock(tty, state);
		return VFS_STATUS_OK;
	}

	tty->t_transition = true;
	tty->t_hungup = false;
	tty_unlock(tty, state);

	vfs_status_t status = tty->t_driver->open != 0 ? tty->t_driver->open(tty, tty->t_driver_context) : VFS_STATUS_OK;

	state = tty_lock(tty);

	/* Detached while the driver answered: the open fails, as one arriving a moment later would. */
	if (status == VFS_STATUS_OK && tty->t_detached) status = VFS_STATUS_NO_DEVICE;
	if (status == VFS_STATUS_OK) tty->t_opens = 1U;

	tty->t_transition = false;
	waitq_wake_all(&tty->t_state);
	tty_unlock(tty, state);

	if (status != VFS_STATUS_OK) tty_release(tty);

	return status;
}

/* The devfs open: the file's open, then the controlling terminal, if this opener is to acquire one. */
static vfs_status_t
tty_devfs_open(void *context, uint32_t flags, devfs_clone_t *clone)
{
	(void)clone;

	vfs_status_t status = tty_file_open(context, flags);

	if (status == VFS_STATUS_OK) tty_job_open(context, flags);

	return status;
}

tty_t
tty_from_file(file_t file)
{
	const devfs_operations_t *operations;
	void *context;

	if (file == 0 || file->f_type == FILE_TYPE_SOCKET || file->f_vnode == 0) return TTY_NULL;
	if (!devfs_device_of(file->f_vnode, &operations, &context) || operations != &g_tty_devfs_operations) return TTY_NULL;

	return tty_reference(context) ? context : TTY_NULL;
}

/*
 * tty_devfs_close
 *
 * Called once per file, after every other operation on it has returned. The
 * last close flushes both queues and closes the driver; an open that arrives
 * meanwhile waits for it (t_transition) and then opens the driver again.
 */
static void
tty_devfs_close(void *context)
{
	tty_t tty = context;
	uint64_t state = tty_lock(tty);

	if (tty->t_opens > 1U) {
		tty->t_opens--;
		tty_unlock(tty, state);
		tty_release(tty);
		return;
	}

	tty->t_opens = 0U;
	tty->t_transition = true;
	tty->t_throttled = false;
	tty->t_stopped = false;
	tty->t_ldisc.stopped = false;
	tty->t_signals = 0U;
	tty->t_deferred = 0U;
	n_tty_flush_input(&tty->t_ldisc);
	tty_queue_flush(&tty->t_output);

	bool call = tty->t_driver->close != 0 && !tty->t_detached;

	tty_unlock(tty, state);

	if (call) tty->t_driver->close(tty, tty->t_driver_context);

	state = tty_lock(tty);
	tty->t_transition = false;
	if (!tty->t_detached) tty->t_hungup = false;
	waitq_wake_all(&tty->t_state);
	tty_unlock(tty, state);

	tty_release(tty);
}

/*
 * tty_read_ready
 *
 * Whether a read of `capacity` can finish now, by the rules of the mode;
 * t_lock held. `timed_out` is whether this read's VTIME has run out.
 *
 *   canonical            a whole line (or an end of file)
 *   VMIN 0, VTIME 0      always, with whatever is there, possibly nothing
 *   VMIN 0, VTIME > 0    a byte, or VTIME since the read began
 *   VMIN > 0, VTIME 0    VMIN bytes (or `capacity`, if smaller)
 *   VMIN > 0, VTIME > 0  as above, or VTIME since the last byte arrived
 */
static bool
tty_read_ready(const tty_t tty, uint32_t capacity, bool timed_out)
{
	const n_tty_t *ldisc = &tty->t_ldisc;

	if ((ldisc->termios.c_lflag & NXU_ICANON) != 0U) return n_tty_line_ready(ldisc);

	uint32_t min = ldisc->termios.c_cc[NXU_VMIN];
	uint32_t time = ldisc->termios.c_cc[NXU_VTIME];
	uint32_t available = n_tty_available(ldisc);

	if (min == 0U) return available != 0U || time == 0U || timed_out;

	uint32_t want = min < capacity ? min : capacity;

	return available >= want || (time != 0U && timed_out && available != 0U);
}

/*
 * tty_devfs_read
 *
 * Wait until the mode says the read can finish (tty_read_ready), then take
 * from the line discipline: one line at most in canonical mode, up to
 * `size` otherwise. A hung-up terminal returns what is left and then end of
 * file. "Do not wait" returns what there is, or WOULD_BLOCK when a read
 * would have to wait for the first byte. The wait is interruptible.
 */
static vfs_status_t
tty_devfs_read(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size)
{
	tty_t tty = context;

	*read_size = 0ULL;

	if (size == 0ULL) return VFS_STATUS_OK;

	vfs_status_t allowed = tty_job_check(tty, TTY_JOB_READ);

	if (allowed != VFS_STATUS_OK) return allowed;

	uint32_t capacity = size > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)size;
	uint64_t deadline = WAITQ_NO_DEADLINE;
	uint32_t received_mark = 0U;
	bool timed_out = false;
	uint64_t state = tty_lock(tty);

	while (!tty_read_ready(tty, capacity, timed_out)) {
		const n_tty_t *ldisc = &tty->t_ldisc;
		bool canonical = (ldisc->termios.c_lflag & NXU_ICANON) != 0U;
		uint32_t available = canonical ? 0U : n_tty_available(ldisc);

		if (tty->t_hungup) break;

		if ((ioflags & VNODE_IO_NONBLOCK) != 0U) {
			if (available != 0U) break;

			tty_unlock(tty, state);
			return VFS_STATUS_WOULD_BLOCK;
		}

		/* VTIME, in tenths of a second: from now when VMIN is 0, from the last byte otherwise. */
		uint32_t time = ldisc->termios.c_cc[NXU_VTIME];

		if (canonical || time == 0U) {
			deadline = WAITQ_NO_DEADLINE;
		} else if (ldisc->termios.c_cc[NXU_VMIN] == 0U) {
			if (deadline == WAITQ_NO_DEADLINE) deadline = timer_get_microseconds() + (uint64_t)time * 100000ULL;
		} else if (available == 0U) {
			deadline = WAITQ_NO_DEADLINE;
		} else if (deadline == WAITQ_NO_DEADLINE || ldisc->received != received_mark) {
			received_mark = ldisc->received;
			deadline = timer_get_microseconds() + (uint64_t)time * 100000ULL;
		}

		uint32_t seq = waitq_seq(&tty->t_readers);
		tty_unlock(tty, state);

		waitq_result_t result = waitq_block_deadline(&tty->t_readers, seq, true, deadline);

		if (result == WAITQ_INTERRUPTED) return VFS_STATUS_INTERRUPTED;

		state = tty_lock(tty);

		/* An inter-byte timer that a new byte restarted has not run out. */
		if (result == WAITQ_TIMED_OUT &&
			(tty->t_ldisc.termios.c_cc[NXU_VMIN] == 0U || tty->t_ldisc.received == received_mark))
			timed_out = true;
	}

	uint32_t taken = n_tty_read(&tty->t_ldisc, buffer, capacity);
	bool unthrottle = tty->t_throttled && n_tty_receive_room(&tty->t_ldisc) != 0U && !tty->t_detached &&
					  tty->t_driver->unthrottle != 0;

	if (unthrottle) tty->t_throttled = false;

	tty_unlock(tty, state);

	/* The driver held input back for want of room; there is some now. */
	if (unthrottle) tty->t_driver->unthrottle(tty, tty->t_driver_context);

	*read_size = taken;
	return VFS_STATUS_OK;
}

/*
 * tty_devfs_write
 *
 * Output processing into the output queue, as much as fits, starting the
 * driver after each step and waiting for room while there is more. A write
 * cut short after moving bytes reports the short count; one that moved none
 * reports why (would block, interrupted, hung up). Under FLUSHO everything is
 * taken and discarded.
 */
static vfs_status_t
tty_devfs_write(void *context, const void *buffer, uint64_t size, uint32_t ioflags, uint64_t *written_size)
{
	tty_t tty = context;
	const uint8_t *bytes = buffer;
	uint64_t done = 0ULL;
	vfs_status_t status = VFS_STATUS_OK;

	*written_size = 0ULL;

	vfs_status_t allowed = tty_job_check(tty, TTY_JOB_WRITE);

	if (allowed != VFS_STATUS_OK) return allowed;

	uint64_t state = tty_lock(tty);

	while (done < size) {
		if (tty->t_hungup) {
			status = VFS_STATUS_IO_ERROR;
			break;
		}

		uint64_t remaining = size - done;
		uint32_t chunk = remaining > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
		uint32_t taken = n_tty_write(&tty->t_ldisc, bytes + done, chunk);

		done += taken;

		tty_deferred_t deferred;

		tty_take_deferred(tty, &deferred);

		/* Bytes queued: start the driver, then try again; it may have made room already. */
		if (tty_deferred_pending(&deferred)) {
			tty_unlock(tty, state);
			tty_run_deferred(tty, &deferred);
			state = tty_lock(tty);
			continue;
		}

		if (done == size) break;

		/* No room for the next byte's output, and the driver has been told: wait for it. */
		if ((ioflags & VNODE_IO_NONBLOCK) != 0U) {
			status = VFS_STATUS_WOULD_BLOCK;
			break;
		}

		uint32_t seq = waitq_seq(&tty->t_writers);
		tty_unlock(tty, state);

		if (waitq_block_deadline(&tty->t_writers, seq, true, WAITQ_NO_DEADLINE) == WAITQ_INTERRUPTED) {
			*written_size = done;
			return done != 0ULL ? VFS_STATUS_OK : VFS_STATUS_INTERRUPTED;
		}

		state = tty_lock(tty);
	}

	tty_deferred_t deferred;

	tty_take_deferred(tty, &deferred);

	tty_unlock(tty, state);
	tty_run_deferred(tty, &deferred);

	*written_size = done;
	return done != 0ULL ? VFS_STATUS_OK : status;
}

static vfs_status_t
tty_devfs_ioctl(void *context, uint32_t command, void *argument)
{
	return tty_ioctl(context, command, argument);
}

/*
 * tty_devfs_poll
 *
 * Readable when the line discipline has something a read can take (a whole
 * line in canonical mode) or once hung up (a read then returns end of file);
 * writable with room in the output queue and the line up.
 */
static vfs_status_t
tty_devfs_poll(void *context, uint32_t *ready, struct ioready **source)
{
	tty_t tty = context;
	uint32_t mask = 0U;
	uint64_t state = tty_lock(tty);

	if (n_tty_readable(&tty->t_ldisc) || tty->t_hungup) mask |= IOREADY_READ;
	if (!tty->t_hungup && tty_queue_room(&tty->t_output) != 0U) mask |= IOREADY_WRITE;
	if (tty->t_hungup) mask |= IOREADY_HANGUP;

	tty_unlock(tty, state);

	*ready = mask;
	*source = &tty->t_ready;
	return VFS_STATUS_OK;
}
