/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tty/pty.c
 *
 * Pseudo-terminal pairs. See kern/tty/pty.h for the model.
 *
 * A pair is one struct pty: the slave's terminal (a tty_t, whose driver is
 * this file), and the master's side, which is a nameless devfs device made
 * by the cloning open of /dev/ptmx. The master has a ring of its own. The
 * driver's start operation moves the slave's output into it as soon as it is
 * written (pty_pull), and the master reads from the ring. That keeps the
 * output the slave wrote just before its last close, which the terminal
 * itself discards at that close: a shell's last words reach the emulator.
 * When the ring is full, output stays in the terminal's queue and the slave's
 * writers wait there, so the master still paces them.
 *
 * Reference counting: the pair is the terminal's driver context, so the
 * terminal owns it. The master holds the driver's reference (tty_create's)
 * and gives it up when it closes, after hanging the slave up for good and
 * removing its name; each open slave file holds one of its own. When the
 * last goes, the terminal calls pty_release, which frees the pair and its
 * index. The master's devfs node points at the pair and is used only while
 * the master is open, which is while the driver's reference is held.
 *
 * Locking: each pair has one spinlock (p_lock), irqsave like the terminal's,
 * covering the ring and the flags. It is taken around short updates only,
 * never across a call into kern/tty/tty.c, a sleep, or a readiness post.
 * Moving output (pty_pull) is done by one caller at a time, with no lock held
 * across tty_output_take, so two movers cannot interleave their chunks: a
 * caller that finds a move in progress asks the mover to go round once more
 * (the pattern of the loopback test driver). Sleeps follow the
 * sequence-number pattern of kern/ipc/socket.c: the sequence is taken before
 * the condition is tested, and every waker changes the condition first.
 */

#include <kern/tty/pty.h>

#include <kern/console/console.h>
#include <kern/ioready.h>
#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/waitq.h>
#include <kern/syscall/termios_defs.h>
#include <kern/tty/tty_internal.h>
#include <vfs/devfs.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The master's ring: as large as the terminal's own output queue. */
#define PTY_RING_SIZE 4096U

/* Moved from the terminal's output queue to the ring this much at a time. */
#define PTY_PULL_CHUNK 256U

/* "/dev/ttys" and three digits. */
#define PTY_DEV_PREFIX "/dev/"
#define PTY_SLAVE_PREFIX "ttys"

/*
 * struct pty
 *
 * p_tty, p_index and p_name are fixed once the pair is made. The rest is
 * protected by p_lock, except the wait queues and the notifier, which have
 * their own locks.
 *
 * p_ring / p_head / p_count
 *     The slave's output, waiting for the master to read it.
 *
 * p_moving / p_again
 *     A pty_pull is in progress; another caller asked it to look again.
 *
 * p_unlocked
 *     TIOCPTYUNLK has been made: the slave may be opened.
 *
 * p_slave_open / p_slave_closed
 *     The slave has a file open; it had files and the last one has closed
 *     (the master reads end of file once the ring is empty). An open after
 *     that clears p_slave_closed again.
 */
struct pty {
	tty_t p_tty;
	uint32_t p_index;
	char p_name[TTY_NAME_MAX + 1U];

	nxu_spinlock_t p_lock;
	uint8_t p_ring[PTY_RING_SIZE];
	uint32_t p_head;
	uint32_t p_count;
	bool p_moving;
	bool p_again;
	bool p_unlocked;
	bool p_slave_open;
	bool p_slave_closed;

	waitq_t p_readers; /* the master, waiting for the slave's output */
	waitq_t p_writers; /* the master, waiting for room in the slave's input */

	/* Posted when output arrives, input room appears, or the slave closes. */
	ioready_t p_ready;
};

static nxu_spinlock_t g_pty_lock = NXU_SPINLOCK_INIT;
static bool g_pty_used[PTY_MAX];
static uint32_t g_pty_count;

static vfs_status_t pty_slave_open(tty_t tty, void *context);
static void pty_slave_close(tty_t tty, void *context);
static void pty_slave_start(tty_t tty, void *context);
static void pty_slave_unthrottle(tty_t tty, void *context);
static void pty_release(void *context);

static const tty_driver_operations_t g_pty_driver = {.open = pty_slave_open,
													 .close = pty_slave_close,
													 .start = pty_slave_start,
													 .unthrottle = pty_slave_unthrottle,
													 .release = pty_release};

static vfs_status_t pty_ptmx_open(void *context, uint32_t flags, devfs_clone_t *clone);
static void pty_master_close(void *context);
static vfs_status_t pty_master_read(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size);
static vfs_status_t pty_master_write(void *context, const void *buffer, uint64_t size, uint32_t ioflags,
									 uint64_t *written_size);
static vfs_status_t pty_master_ioctl(void *context, uint32_t command, void *argument);
static vfs_status_t pty_master_poll(void *context, uint32_t *ready, struct ioready **source);

static const devfs_operations_t g_ptmx_operations = {.open = pty_ptmx_open};

static const devfs_operations_t g_pty_master_operations = {.close = pty_master_close,
														   .read = pty_master_read,
														   .write = pty_master_write,
														   .ioctl = pty_master_ioctl,
														   .poll = pty_master_poll};

static uint64_t
pty_lock(struct pty *pty)
{
	return nxu_spin_lock_irqsave(&pty->p_lock);
}

static void
pty_unlock(struct pty *pty, uint64_t state)
{
	nxu_spin_unlock_irqrestore(&pty->p_lock, state);
}

/* ---- indices ------------------------------------------------------------ */

static bool
pty_index_allocate(uint32_t *index)
{
	bool found = false;

	nxu_spin_lock(&g_pty_lock);

	for (uint32_t candidate = 0U; candidate < PTY_MAX && !found; candidate++) {
		if (g_pty_used[candidate]) continue;

		g_pty_used[candidate] = true;
		g_pty_count++;
		*index = candidate;
		found = true;
	}

	nxu_spin_unlock(&g_pty_lock);
	return found;
}

static void
pty_index_free(uint32_t index)
{
	nxu_spin_lock(&g_pty_lock);

	if (index < PTY_MAX && g_pty_used[index]) {
		g_pty_used[index] = false;
		g_pty_count--;
	}

	nxu_spin_unlock(&g_pty_lock);
}

uint32_t
pty_count(void)
{
	nxu_spin_lock(&g_pty_lock);
	uint32_t count = g_pty_count;
	nxu_spin_unlock(&g_pty_lock);

	return count;
}

/* "ttys" and the index in three digits, as Darwin names them. */
static void
pty_format_name(uint32_t index, char name[TTY_NAME_MAX + 1U])
{
	size_t prefix = sizeof(PTY_SLAVE_PREFIX) - 1U;

	memcpy(name, PTY_SLAVE_PREFIX, prefix);
	name[prefix] = (char)('0' + (index / 100U) % 10U);
	name[prefix + 1U] = (char)('0' + (index / 10U) % 10U);
	name[prefix + 2U] = (char)('0' + index % 10U);
	name[prefix + 3U] = '\0';
}

/* ---- the ring ----------------------------------------------------------- */

/* Append what fits; p_lock held. */
static void
pty_ring_put(struct pty *pty, const uint8_t *bytes, uint32_t count)
{
	for (uint32_t done = 0U; done < count;) {
		uint32_t tail = (pty->p_head + pty->p_count) % PTY_RING_SIZE;
		uint32_t contiguous = PTY_RING_SIZE - tail;
		uint32_t chunk = count - done < contiguous ? count - done : contiguous;

		memcpy(pty->p_ring + tail, bytes + done, chunk);
		pty->p_count += chunk;
		done += chunk;
	}
}

/* Remove up to `capacity` bytes, oldest first; p_lock held. */
static uint32_t
pty_ring_take(struct pty *pty, uint8_t *buffer, uint32_t capacity)
{
	uint32_t total = capacity < pty->p_count ? capacity : pty->p_count;

	for (uint32_t done = 0U; done < total;) {
		uint32_t contiguous = PTY_RING_SIZE - pty->p_head;
		uint32_t chunk = total - done < contiguous ? total - done : contiguous;

		memcpy(buffer + done, pty->p_ring + pty->p_head, chunk);
		pty->p_head = (pty->p_head + chunk) % PTY_RING_SIZE;
		pty->p_count -= chunk;
		done += chunk;
	}

	return total;
}

/*
 * pty_pull
 *
 * Move the slave's output from the terminal's queue into the ring, as much
 * as fits, and tell the master. One mover at a time; see the file header.
 * The ring only fills here and only empties in pty_master_read, which calls
 * this again afterwards, so output left in the terminal's queue for want of
 * room is always moved once there is some.
 */
static void
pty_pull(struct pty *pty)
{
	uint8_t chunk[PTY_PULL_CHUNK];
	bool moved = false;
	uint64_t state = pty_lock(pty);

	if (pty->p_moving) {
		pty->p_again = true;
		pty_unlock(pty, state);
		return;
	}

	pty->p_moving = true;

	do {
		pty->p_again = false;

		for (;;) {
			uint32_t room = PTY_RING_SIZE - pty->p_count;

			if (room == 0U) break;

			pty_unlock(pty, state);

			uint32_t want = room < sizeof(chunk) ? room : (uint32_t)sizeof(chunk);
			uint32_t count = tty_output_take(pty->p_tty, chunk, want);

			state = pty_lock(pty);

			if (count == 0U) break;

			/* Only this mover adds to the ring, and readers only take from it: the room is still there. */
			pty_ring_put(pty, chunk, count);
			moved = true;
		}
	} while (pty->p_again);

	pty->p_moving = false;

	if (moved) waitq_wake_all(&pty->p_readers);

	pty_unlock(pty, state);

	if (moved) ioready_post(&pty->p_ready, IOREADY_READ);
}

/* ---- the slave's driver ------------------------------------------------- */

/* The slave's first open: refused until the master has unlocked it. */
static vfs_status_t
pty_slave_open(tty_t tty, void *context)
{
	(void)tty;

	struct pty *pty = context;
	uint64_t state = pty_lock(pty);
	bool unlocked = pty->p_unlocked;

	if (unlocked) {
		pty->p_slave_open = true;
		pty->p_slave_closed = false;
	}

	pty_unlock(pty, state);
	return unlocked ? VFS_STATUS_OK : VFS_STATUS_DENIED;
}

/* The slave's last close: the master reads what is left, then end of file. */
static void
pty_slave_close(tty_t tty, void *context)
{
	(void)tty;

	struct pty *pty = context;
	uint64_t state = pty_lock(pty);

	pty->p_slave_open = false;
	pty->p_slave_closed = true;
	waitq_wake_all(&pty->p_readers);
	waitq_wake_all(&pty->p_writers);

	pty_unlock(pty, state);

	ioready_post(&pty->p_ready, IOREADY_READ | IOREADY_HANGUP);
}

static void
pty_slave_start(tty_t tty, void *context)
{
	(void)tty;

	pty_pull(context);
}

/* The slave's input has room again: a master waiting to type can go on. */
static void
pty_slave_unthrottle(tty_t tty, void *context)
{
	(void)tty;

	struct pty *pty = context;
	uint64_t state = pty_lock(pty);

	waitq_wake_all(&pty->p_writers);
	pty_unlock(pty, state);

	ioready_post(&pty->p_ready, IOREADY_WRITE);
}

/* The terminal's last reference is gone: nothing can reach the pair any more. */
static void
pty_release(void *context)
{
	struct pty *pty = context;

	ioready_destroy(&pty->p_ready);
	pty_index_free(pty->p_index);
	(void)kfree(pty);
}

/* ---- /dev/ptmx and the master ------------------------------------------- */

/*
 * pty_ptmx_open
 *
 * Make a pair and redirect the open to its master. The slave is published
 * at once, so TIOCPTYGNAME has a name to give, but stays closed to opens
 * until TIOCPTYUNLK. It starts out owned by the opener's real user, as
 * TIOCPTYGRANT would leave it.
 */
static vfs_status_t
pty_ptmx_open(void *context, uint32_t flags, devfs_clone_t *clone)
{
	(void)context;
	(void)flags;

	proc_t proc = current_proc();
	uint32_t owner = proc != 0 ? proc->p_ucred.cr_ruid : 0U;
	uint32_t index;

	if (!pty_index_allocate(&index)) return VFS_STATUS_NO_SPACE;

	struct pty *pty = kmalloc(sizeof(struct pty));

	if (pty == 0) {
		pty_index_free(index);
		return VFS_STATUS_NO_MEMORY;
	}

	/* Zeroed: an unlocked spinlock, an empty ring, a slave that is locked and was never opened. */
	memset(pty, 0, sizeof(*pty));
	pty->p_index = index;
	pty_format_name(index, pty->p_name);
	waitq_init(&pty->p_readers);
	waitq_init(&pty->p_writers);
	ioready_init(&pty->p_ready);

	if (!tty_create(&g_pty_driver, pty, &pty->p_tty)) {
		ioready_destroy(&pty->p_ready);
		(void)kfree(pty);
		pty_index_free(index);
		return VFS_STATUS_NO_MEMORY;
	}

	/* From here the terminal owns the pair: releasing it frees both. */
	if (!tty_register(pty->p_tty, pty->p_name, owner, PTY_TTY_GID, 0620U)) {
		tty_detach(pty->p_tty);
		tty_release(pty->p_tty);
		return VFS_STATUS_NO_SPACE;
	}

	clone->operations = &g_pty_master_operations;
	clone->context = pty;
	return VFS_STATUS_OK;
}

/*
 * pty_master_close
 *
 * The pair is over. The slave hangs up for good, so its session is told
 * and its files see end of file and I/O errors; the name goes; the driver's
 * reference goes, and with it the pair, once no slave file is left. Called
 * once, after every other master operation has returned.
 */
static void
pty_master_close(void *context)
{
	struct pty *pty = context;
	tty_t tty = pty->p_tty;

	tty_detach(tty);
	tty_unregister(tty);
	tty_release(tty);
}

/*
 * pty_master_read
 *
 * What the slave has written: whatever the ring holds, without waiting for
 * more. With nothing there, wait, unless the slave has closed (end of file)
 * or the read must not wait. The wait is interruptible.
 */
static vfs_status_t
pty_master_read(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size)
{
	struct pty *pty = context;
	uint32_t capacity = size > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)size;

	*read_size = 0ULL;

	if (capacity == 0U) return VFS_STATUS_OK;

	for (;;) {
		uint32_t seq = waitq_seq(&pty->p_readers);
		uint64_t state = pty_lock(pty);
		uint32_t taken = pty_ring_take(pty, (uint8_t *)buffer, capacity);
		bool closed = pty->p_slave_closed;

		pty_unlock(pty, state);

		if (taken != 0U) {
			/* Room in the ring: bring over what waited in the terminal's queue. */
			pty_pull(pty);
			*read_size = taken;
			return VFS_STATUS_OK;
		}

		if (closed) return VFS_STATUS_OK;
		if ((ioflags & VNODE_IO_NONBLOCK) != 0U) return VFS_STATUS_WOULD_BLOCK;
		if (!waitq_block_seq(&pty->p_readers, seq, true)) return VFS_STATUS_INTERRUPTED;
	}
}

/*
 * pty_master_write
 *
 * Typing: the bytes are the slave's input, through its line discipline.
 * Canonical input never refuses; raw input the slave has no room for is
 * held back here until it reads (the unthrottle operation wakes us). With
 * the slave closed, input is discarded, as the terminal does with input
 * nobody will read.
 */
static vfs_status_t
pty_master_write(void *context, const void *buffer, uint64_t size, uint32_t ioflags, uint64_t *written_size)
{
	struct pty *pty = context;
	const uint8_t *bytes = buffer;
	uint64_t done = 0ULL;

	*written_size = 0ULL;

	while (done < size) {
		uint32_t seq = waitq_seq(&pty->p_writers);
		uint64_t remaining = size - done;
		uint32_t chunk = remaining > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;

		done += tty_input(pty->p_tty, bytes + done, chunk);

		if (done == size) break;

		if ((ioflags & VNODE_IO_NONBLOCK) != 0U) {
			*written_size = done;
			return done != 0ULL ? VFS_STATUS_OK : VFS_STATUS_WOULD_BLOCK;
		}

		if (!waitq_block_seq(&pty->p_writers, seq, true)) {
			*written_size = done;
			return done != 0ULL ? VFS_STATUS_OK : VFS_STATUS_INTERRUPTED;
		}
	}

	*written_size = done;
	return VFS_STATUS_OK;
}

/*
 * pty_master_ioctl
 *
 * The Darwin pseudo-terminal requests, and the settings and window size of
 * the slave's terminal, which the emulator owns: it sets the window size
 * when its window changes, and that is what sends the slave's foreground
 * group SIGWINCH. The master is not a terminal itself, so job-control
 * requests are not taken here.
 */
static vfs_status_t
pty_master_ioctl(void *context, uint32_t command, void *argument)
{
	struct pty *pty = context;

	switch (command) {
	case NXU_TIOCPTYGNAME: {
		nxu_ptyname_t *name = argument;
		size_t prefix = sizeof(PTY_DEV_PREFIX) - 1U;
		size_t length = strlen(pty->p_name);

		memset(name, 0, sizeof(*name));
		memcpy(name->name, PTY_DEV_PREFIX, prefix);
		memcpy(name->name + prefix, pty->p_name, length + 1U);
		return VFS_STATUS_OK;
	}

	case NXU_TIOCPTYGRANT: {
		proc_t proc = current_proc();
		uint32_t owner = proc != 0 ? proc->p_ucred.cr_ruid : 0U;

		return devfs_set_attributes(pty->p_name, owner, PTY_TTY_GID, 0620U) ? VFS_STATUS_OK : VFS_STATUS_NO_DEVICE;
	}

	case NXU_TIOCPTYUNLK: {
		uint64_t state = pty_lock(pty);

		pty->p_unlocked = true;
		pty_unlock(pty, state);
		return VFS_STATUS_OK;
	}

	case NXU_FIONREAD: {
		uint64_t state = pty_lock(pty);
		uint32_t count = pty->p_count;

		pty_unlock(pty, state);
		*(uint32_t *)argument = count + tty_output_pending(pty->p_tty);
		return VFS_STATUS_OK;
	}

	case NXU_TIOCGETA:
		tty_get_termios(pty->p_tty, argument);
		return VFS_STATUS_OK;

	case NXU_TIOCSETA:
	case NXU_TIOCSETAW:
	case NXU_TIOCSETAF:
		/* The master writes no output of its own to wait for: all three apply at once, SETAF discarding input. */
		return tty_change_termios(pty->p_tty, argument, command == NXU_TIOCSETAF);

	case NXU_TIOCFLUSH: {
		uint32_t which = *(const uint32_t *)argument;

		if ((which & ~(NXU_FREAD | NXU_FWRITE)) != 0U) return VFS_STATUS_INVALID;
		if (which == 0U) which = NXU_FREAD | NXU_FWRITE;

		/* The slave's output, as the master sees it, is the terminal's queue and the ring. */
		if ((which & NXU_FWRITE) != 0U) {
			uint64_t state = pty_lock(pty);

			pty->p_head = 0U;
			pty->p_count = 0U;
			pty_unlock(pty, state);
		}

		tty_flush(pty->p_tty, which);
		return VFS_STATUS_OK;
	}

	case NXU_TIOCGWINSZ:
		tty_get_winsize(pty->p_tty, argument);
		return VFS_STATUS_OK;

	case NXU_TIOCSWINSZ:
		tty_set_winsize(pty->p_tty, argument);
		return VFS_STATUS_OK;

	default:
		return VFS_STATUS_NOT_TTY;
	}
}

/*
 * pty_master_poll
 *
 * Readable when the ring has output or the slave has closed (a read then
 * returns end of file); writable when the slave's input has room; hung up
 * once the slave has closed.
 */
static vfs_status_t
pty_master_poll(void *context, uint32_t *ready, struct ioready **source)
{
	struct pty *pty = context;
	uint32_t mask = 0U;
	uint64_t state = pty_lock(pty);

	if (pty->p_count != 0U || pty->p_slave_closed) mask |= IOREADY_READ;
	if (pty->p_slave_closed) mask |= IOREADY_HANGUP;

	pty_unlock(pty, state);

	if (tty_input_room(pty->p_tty) != 0U) mask |= IOREADY_WRITE;

	*ready = mask;
	*source = &pty->p_ready;
	return VFS_STATUS_OK;
}

bool
pty_init(void)
{
	kprintf("initializing pty\n");

	if (!devfs_register_device("ptmx", &g_ptmx_operations, 0)) {
		kprintf("cannot publish /dev/ptmx\n");
		return false;
	}

	kprintf("initialized pty successfully\n");
	return true;
}
