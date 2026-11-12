/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/devfs_selftest.c
 *
 * Boot self-test for devfs, run once /dev is mounted.
 *
 * It registers a cloning device and drives it through the same VFS entry
 * points a system call uses (vfs_open, vfs_read, vfs_write, vfs_close on a
 * private descriptor table), so it covers the whole path from a path name to
 * a driver operation:
 *
 *   - a registered name can be looked up, and a duplicate is refused;
 *   - a node reports its owner, group and mode, and a change shows;
 *   - each open of a cloning device reaches a device of its own, which is
 *     closed exactly once;
 *   - "do not wait" reaches the device both from the open flags and from a
 *     single call;
 *   - an unregistered name is gone at once while the files open on the device
 *     keep working;
 *   - detached slots are reclaimed, so nodes can come and go without limit.
 *
 * The test device is a one-message mailbox: a write stores the bytes, a read
 * takes them, and a read of an empty mailbox would have to wait.
 */

#include <vfs/devfs.h>

#include <kern/console/console.h>
#include <kern/syscall/syscall_defs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SELFTEST_NAME "devfs-selftest"
#define SELFTEST_PATH "/dev/" SELFTEST_NAME
#define SELFTEST_MAILBOX_SIZE 16U

typedef struct {
	char bytes[SELFTEST_MAILBOX_SIZE];
	uint32_t length;
	uint32_t closes;
} selftest_mailbox_t;

static selftest_mailbox_t g_mailboxes[2];
static uint32_t g_mailbox_opens;
static uint32_t g_cloner_closes;

/* Static: a descriptor table is too large for the boot stack. */
static struct filedesc g_selftest_fd;

static vfs_status_t
selftest_mailbox_read(void *context, void *buffer, uint64_t size, uint32_t ioflags, uint64_t *read_size)
{
	selftest_mailbox_t *mailbox = context;

	if (mailbox->length == 0U) {
		/* A real device would sleep here; the test never asks it to. */
		return (ioflags & VNODE_IO_NONBLOCK) != 0U ? VFS_STATUS_WOULD_BLOCK : VFS_STATUS_IO_ERROR;
	}

	uint64_t count = size < mailbox->length ? size : mailbox->length;

	memcpy(buffer, mailbox->bytes, (size_t)count);
	mailbox->length = 0U;
	*read_size = count;
	return VFS_STATUS_OK;
}

static vfs_status_t
selftest_mailbox_write(void *context, const void *buffer, uint64_t size, uint32_t ioflags, uint64_t *written_size)
{
	(void)ioflags;

	selftest_mailbox_t *mailbox = context;
	uint64_t count = size < SELFTEST_MAILBOX_SIZE ? size : SELFTEST_MAILBOX_SIZE;

	memcpy(mailbox->bytes, buffer, (size_t)count);
	mailbox->length = (uint32_t)count;
	*written_size = count;
	return VFS_STATUS_OK;
}

static void
selftest_mailbox_close(void *context)
{
	((selftest_mailbox_t *)context)->closes++;
}

static const devfs_operations_t g_selftest_mailbox_ops = {
	.close = selftest_mailbox_close, .read = selftest_mailbox_read, .write = selftest_mailbox_write};

/* The named device: every open is redirected to the next mailbox. */
static vfs_status_t
selftest_cloner_open(void *context, uint32_t flags, devfs_clone_t *clone)
{
	(void)context;
	(void)flags;

	if (g_mailbox_opens >= 2U) return VFS_STATUS_BUSY;

	clone->operations = &g_selftest_mailbox_ops;
	clone->context = &g_mailboxes[g_mailbox_opens++];
	return VFS_STATUS_OK;
}

static void
selftest_cloner_close(void *context)
{
	(void)context;
	g_cloner_closes++;
}

static const devfs_operations_t g_selftest_cloner_ops = {.open = selftest_cloner_open, .close = selftest_cloner_close};

static bool
selftest_fail(const char *what)
{
	kprintf("devfs_self_test: FAILED: %s\n", what);
	return false;
}

/* True when the node at SELFTEST_PATH reports exactly these attributes. */
static bool
selftest_attributes_are(uint32_t uid, uint32_t gid, uint32_t mode)
{
	vnode_t vnode;
	vnode_attr_t attr;

	if (vfs_lookup(SELFTEST_PATH, &vnode) != VFS_STATUS_OK) return false;

	bool match = vnode_getattr(vnode, &attr) == VFS_STATUS_OK && attr.va_type == VNODE_TYPE_CHARACTER &&
				 attr.va_mode == (NXU_S_IFCHR | mode) && attr.va_uid == uid && attr.va_gid == gid;

	vnode_rele(vnode);
	return match;
}

bool
devfs_self_test(void)
{
	uint32_t baseline = devfs_device_count();
	vnode_t vnode;

	memset(g_mailboxes, 0, sizeof(g_mailboxes));
	g_mailbox_opens = 0U;
	g_cloner_closes = 0U;
	filedesc_init(&g_selftest_fd);

	/* Names. */
	if (!devfs_register_device(SELFTEST_NAME, &g_selftest_cloner_ops, 0)) return selftest_fail("register");
	if (devfs_register_device(SELFTEST_NAME, &g_selftest_cloner_ops, 0)) return selftest_fail("duplicate name accepted");
	if (devfs_device_count() != baseline + 1U) return selftest_fail("device count after register");

	/* Attributes. */
	if (!selftest_attributes_are(0U, 0U, DEVFS_MODE_DEFAULT)) return selftest_fail("default attributes");
	if (!devfs_set_attributes(SELFTEST_NAME, 501U, 20U, 0620U)) return selftest_fail("set attributes");
	if (!selftest_attributes_are(501U, 20U, 0620U)) return selftest_fail("changed attributes");

	/* Cloning open: two opens, two devices, neither of them the named one. */
	uint32_t first;
	uint32_t second;

	if (vfs_open(&g_selftest_fd, SELFTEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE, &first) != VFS_STATUS_OK)
		return selftest_fail("first open");
	if (vfs_open(&g_selftest_fd, SELFTEST_PATH, VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_NONBLOCK, &second) !=
		VFS_STATUS_OK)
		return selftest_fail("second open");
	if (g_mailbox_opens != 2U) return selftest_fail("each open did not reach the cloning device");

	file_t first_file;
	file_t second_file;

	if (filedesc_get(&g_selftest_fd, first, &first_file) != VFS_STATUS_OK ||
		filedesc_get(&g_selftest_fd, second, &second_file) != VFS_STATUS_OK)
		return selftest_fail("descriptor lookup");
	if (vfs_lookup(SELFTEST_PATH, &vnode) != VFS_STATUS_OK) return selftest_fail("lookup while open");

	bool distinct =
		first_file->f_vnode != second_file->f_vnode && first_file->f_vnode != vnode && second_file->f_vnode != vnode;

	if (!distinct) return selftest_fail("clones share a vnode");
	if (!file_is_stream(first_file)) return selftest_fail("a device is not a stream");

	/* "Do not wait", once from the call and once from the open flags. */
	char buffer[SELFTEST_MAILBOX_SIZE];
	uint64_t count = 0ULL;

	if (file_read(first_file, buffer, sizeof(buffer), VNODE_IO_NONBLOCK, &count) != VFS_STATUS_WOULD_BLOCK)
		return selftest_fail("per-call nonblocking read");
	if (vfs_read(&g_selftest_fd, second, buffer, sizeof(buffer), &count) != VFS_STATUS_WOULD_BLOCK)
		return selftest_fail("nonblocking open flag did not reach the device");

	file_rele(first_file);
	file_rele(second_file);

	/* Data goes to the device that was opened, not to its sibling. */
	if (vfs_write(&g_selftest_fd, first, "ping", 4ULL, &count) != VFS_STATUS_OK || count != 4ULL)
		return selftest_fail("write");
	if (g_mailboxes[1].length != 0U) return selftest_fail("write reached the other clone");

	/* An unregistered name is gone; the open devices and the held vnode are not. */
	if (!devfs_unregister_device(SELFTEST_NAME)) return selftest_fail("unregister");
	if (devfs_unregister_device(SELFTEST_NAME)) return selftest_fail("unregistered twice");

	vnode_t gone;

	if (vfs_lookup(SELFTEST_PATH, &gone) != VFS_STATUS_NOT_FOUND) return selftest_fail("name survived unregister");
	if (vfs_read(&g_selftest_fd, first, buffer, sizeof(buffer), &count) != VFS_STATUS_OK || count != 4ULL ||
		memcmp(buffer, "ping", 4U) != 0)
		return selftest_fail("read after unregister");

	vnode_attr_t attr;

	if (vnode_getattr(vnode, &attr) != VFS_STATUS_OK) return selftest_fail("held vnode after unregister");

	vnode_rele(vnode);

	/* Each clone is closed once; the cloning device, never opened as itself, is not. */
	if (vfs_close(&g_selftest_fd, first) != VFS_STATUS_OK || vfs_close(&g_selftest_fd, second) != VFS_STATUS_OK)
		return selftest_fail("close");
	if (g_mailboxes[0].closes != 1U || g_mailboxes[1].closes != 1U || g_cloner_closes != 0U)
		return selftest_fail("close count");

	/* More register/unregister rounds than there are slots: detached slots must be reclaimed. */
	for (uint32_t round = 0U; round < DEVFS_DEVICE_MAX * 2U; round++) {
		if (!devfs_register_device(SELFTEST_NAME, &g_selftest_mailbox_ops, &g_mailboxes[0]))
			return selftest_fail("slot not reclaimed");
		if (vfs_lookup(SELFTEST_PATH, &vnode) != VFS_STATUS_OK) return selftest_fail("lookup in reclaim round");

		vnode_rele(vnode);

		if (!devfs_unregister_device(SELFTEST_NAME)) return selftest_fail("unregister in reclaim round");
	}

	if (devfs_device_count() != baseline) return selftest_fail("device count after the test");

	kputln("devfs_self_test: passed");
	return true;
}
