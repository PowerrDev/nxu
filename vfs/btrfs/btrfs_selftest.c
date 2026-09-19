/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_selftest.c
 *
 * See btrfs_selftest.h. The walk uses only the public VFS interface
 * (vfs_mount, vnode_lookup, vnode_readdir, vnode_getattr, vnode_read,
 * vnode_readlink, vfs_unmount), so it exercises exactly what the rest of the
 * kernel would.
 */

#include <vfs/btrfs/btrfs_selftest.h>

#include <vfs/btrfs/btrfs.h>
#include <vfs/btrfs/btrfs_format.h>

#include <drivers/block/block_device.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <vfs/file.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>
#include <kern/process/proc.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	const char *path;
	char type;
	uint32_t mode;
	uint64_t size;
	uint64_t inode;
	uint64_t mtime;
	uint32_t crc;
	const char *target;
	int compressed;
} btrfs_expect_t;

typedef struct {
	const char *name;
	const btrfs_expect_t *entries;
	int count;
	int special;         /* 1: "/big" holds 2500 files that are checked by rule, not listed */
} btrfs_expect_fixture_t;

#include <vfs/btrfs/btrfs_selftest_data.h>

#define SELFTEST_PATH_MAX 1024U
#define SELFTEST_DEPTH_MAX 24U
#define SELFTEST_CHUNK 4097U
#define SELFTEST_LINK_MAX 4104U

typedef struct {
	const btrfs_expect_fixture_t *fixture;
	uint8_t *seen;
	uint8_t *buffer;
	char *linkbuf;
	char mount_path[16];
	block_device_t device;
	int failures;
	uint32_t entries;
	uint32_t files;
	uint64_t bytes;
	uint32_t refusals;
	uint32_t resumes;
	char first_file[SELFTEST_PATH_MAX];
} selftest_t;

static int g_failures_total;

/* libk has no strcpy/strcat. */
static void st_strcpy(char *destination, const char *source)
{
	size_t length = strlen(source);
	memcpy(destination, source, length + 1U);
}

static void st_strcat(char *destination, const char *source)
{
	st_strcpy(destination + strlen(destination), source);
}

static void st_fail(selftest_t *t, const char *path, const char *what, uint64_t got, uint64_t want)
{
	t->failures++;
	kprintf("btrfs_selftest: FAIL %s: %s (got %llu, want %llu)\n", path, what, (unsigned long long)got, (unsigned long long)want);
}

static char st_type_char(vnode_type_t type)
{
	switch (type) {
	case VNODE_TYPE_REGULAR: return 'f';
	case VNODE_TYPE_DIRECTORY: return 'd';
	case VNODE_TYPE_SYMLINK: return 'l';
	case VNODE_TYPE_CHARACTER: return 'c';
	case VNODE_TYPE_BLOCK: return 'b';
	case VNODE_TYPE_FIFO: return 'p';
	case VNODE_TYPE_SOCKET: return 's';
	default: return '?';
	}
}

static int st_find(const selftest_t *t, const char *path)
{
	for (int index = 0; index < t->fixture->count; index++) {
		if (strcmp(t->fixture->entries[index].path, path) == 0) return index;
	}

	return -1;
}

/* Sequential read in odd-sized steps; returns the crc32c of the whole file. */
static bool st_read_crc(selftest_t *t, vnode_t vnode, uint64_t size, uint32_t step, uint32_t *crc_out, const char *path)
{
	uint32_t crc = ~0U;
	uint64_t position = 0ULL;

	while (position < size) {
		uint64_t done = 0ULL;
		uint64_t want = size - position < step ? size - position : step;
		vfs_status_t status = vnode_read(vnode, position, t->buffer, step, &done);

		if (status != VFS_STATUS_OK || done != want) {
			st_fail(t, path, "read", status == VFS_STATUS_OK ? done : (uint64_t)status, want);
			return false;
		}

		crc = btrfs_crc32c(crc, t->buffer, (size_t)done);
		position += done;
	}

	*crc_out = ~crc;
	return true;
}

static void st_check_file(selftest_t *t, vnode_t vnode, const btrfs_expect_t *expect, const char *path)
{
	uint32_t crc;
	uint64_t done = 0ULL;
	uint64_t size = expect->size;

	if (expect->compressed) {
		/* Compressed extents must be refused cleanly, never served as garbage. */
		vfs_status_t status = vnode_read(vnode, 0ULL, t->buffer, 64U, &done);

		if (status != VFS_STATUS_NOT_SUPPORTED || done != 0ULL) st_fail(t, path, "a compressed file was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_NOT_SUPPORTED);
		t->refusals++;
		return;
	}

	if (!st_read_crc(t, vnode, size, SELFTEST_CHUNK, &crc, path)) return;
	if (crc != expect->crc) st_fail(t, path, "crc32c of the contents", crc, expect->crc);

	/* A second pass with a different step lands the reads at different offsets. */
	if (size > 8192ULL) {
		if (!st_read_crc(t, vnode, size, 65536U + 13U, &crc, path)) return;
		if (crc != expect->crc) st_fail(t, path, "crc32c with a second read step", crc, expect->crc);
	}

	/* Reads at and past the end return nothing; a read across the end is short. */
	vfs_status_t status = vnode_read(vnode, size, t->buffer, 16U, &done);
	if (status != VFS_STATUS_OK || done != 0ULL) st_fail(t, path, "read at EOF", done, 0ULL);

	status = vnode_read(vnode, size + 5000ULL, t->buffer, 16U, &done);
	if (status != VFS_STATUS_OK || done != 0ULL) st_fail(t, path, "read past EOF", done, 0ULL);

	if (size > 3ULL) {
		status = vnode_read(vnode, size - 3ULL, t->buffer, 100U, &done);
		if (status != VFS_STATUS_OK || done != 3ULL) st_fail(t, path, "read across EOF", done, 3ULL);
	}

	t->files++;
	t->bytes += size;
}

static void st_check_node(selftest_t *t, vnode_t vnode, const char *path, vnode_type_t dirent_type)
{
	vnode_attr_t attr;
	int index = st_find(t, path);

	t->entries++;

	if (index < 0) {
		st_fail(t, path, "entry is not in Linux's listing", 0ULL, 0ULL);
		return;
	}

	const btrfs_expect_t *expect = &t->fixture->entries[index];
	t->seen[index]++;

	if (vnode_getattr(vnode, &attr) != VFS_STATUS_OK) {
		st_fail(t, path, "getattr", 0ULL, 0ULL);
		return;
	}

	if (st_type_char(attr.va_type) != expect->type) st_fail(t, path, "type", (uint64_t)st_type_char(attr.va_type), (uint64_t)expect->type);
	if (st_type_char(dirent_type) != expect->type) st_fail(t, path, "readdir type", (uint64_t)st_type_char(dirent_type), (uint64_t)expect->type);
	if (vnode->v_type != attr.va_type) st_fail(t, path, "vnode type differs from getattr", 0ULL, 0ULL);
	if (attr.va_mode != expect->mode) st_fail(t, path, "mode", attr.va_mode, expect->mode);
	if (attr.va_size != expect->size) st_fail(t, path, "size", attr.va_size, expect->size);
	if (vnode->v_size != expect->size) st_fail(t, path, "vnode size", vnode->v_size, expect->size);
	if (expect->inode != 0ULL && attr.va_inode != expect->inode) st_fail(t, path, "inode", attr.va_inode, expect->inode);
	if (expect->mtime != 0ULL && attr.va_mtime_sec != expect->mtime) st_fail(t, path, "mtime", attr.va_mtime_sec, expect->mtime);

	if (attr.va_type == VNODE_TYPE_REGULAR) {
		st_check_file(t, vnode, expect, path);
		if (t->first_file[0] == '\0' && expect->size != 0ULL && !expect->compressed) {
			st_strcpy(t->first_file, path);
		}
	} else if (attr.va_type == VNODE_TYPE_SYMLINK) {
		/* Targets go up to 4095 bytes. */
		char *target = t->linkbuf;
		uint64_t length = 0ULL;
		vfs_status_t status = vnode_readlink(vnode, target, SELFTEST_LINK_MAX - 1U, &length);

		if (status != VFS_STATUS_OK) {
			st_fail(t, path, "readlink", (uint64_t)status, 0ULL);
		} else {
			target[length] = '\0';
			if (expect->target == 0 || strlen(expect->target) != length || memcmp(expect->target, target, (size_t)length) != 0) st_fail(t, path, "symlink target", length, expect->target == 0 ? 0ULL : strlen(expect->target));
		}
	} else if (attr.va_type == VNODE_TYPE_CHARACTER || attr.va_type == VNODE_TYPE_BLOCK) {
		if (attr.va_rdev == 0ULL) st_fail(t, path, "device number", 0ULL, 1ULL);
	}
}

static void st_walk(selftest_t *t, vnode_t directory, char *path, uint32_t depth)
{
	size_t base = strlen(path);
	uint64_t cursor = 0ULL;
	uint64_t saved = 0ULL;
	uint32_t index = 0U;
	uint32_t half = 0U;
	vfs_dirent_t *entry = kcalloc(1U, sizeof(*entry));

	if (entry == 0) {
		st_fail(t, path, "out of memory", 0ULL, 0ULL);
		return;
	}

	if (depth > SELFTEST_DEPTH_MAX) {
		st_fail(t, path, "directory nesting too deep", depth, SELFTEST_DEPTH_MAX);
		kfree(entry);
		return;
	}

	for (;;) {
		vfs_status_t status = vnode_readdir(directory, &cursor, entry);

		if (status == VFS_STATUS_END_OF_DIRECTORY) break;
		if (status != VFS_STATUS_OK) {
			st_fail(t, path, "readdir", (uint64_t)status, 0ULL);
			break;
		}

		index++;
		if (index == 4U) {
			half = index;
			saved = cursor;
		}

		if ((entry->name_length == 1U && entry->name[0] == '.') || (entry->name_length == 2U && entry->name[0] == '.' && entry->name[1] == '.')) continue;

		if (base + 1U + entry->name_length + 1U >= SELFTEST_PATH_MAX) {
			st_fail(t, path, "path too long", 0ULL, 0ULL);
			continue;
		}

		if (base != 1U) path[base] = '/';
		memcpy(path + (base == 1U ? 1U : base + 1U), entry->name, (size_t)entry->name_length + 1U);

		/* Lookup by name reaches the same inode readdir reported (long names too: the path API caps components at 63 bytes). */
		vnode_t child;
		status = vnode_lookup(directory, entry->name, &child);

		if (status != VFS_STATUS_OK) {
			st_fail(t, path, "lookup", (uint64_t)status, 0ULL);
		} else {
			st_check_node(t, child, path, entry->type);

			int found = st_find(t, path);
			bool skip_children = found >= 0 && t->fixture->special == 1 && strcmp(path, "/big") == 0;

			if (child->v_type == VNODE_TYPE_DIRECTORY && !skip_children && t->failures < 20) st_walk(t, child, path, depth + 1U);
			vnode_rele(child);
		}

		path[base] = '\0';
	}

	/* Resuming from a saved cursor continues right after the entry it was saved at. */
	if (half != 0U) {
		uint64_t resume = saved;
		uint32_t counted = 0U;

		while (vnode_readdir(directory, &resume, entry) == VFS_STATUS_OK) counted++;
		if (counted != index - half) st_fail(t, path, "readdir resume from a saved cursor", counted, index - half);
		t->resumes++;
	}

	/* A name that is not there is not found. */
	vnode_t none;
	vfs_status_t status = vnode_lookup(directory, "this-name-does-not-exist", &none);
	if (status != VFS_STATUS_NOT_FOUND) st_fail(t, path, "lookup of a missing name", (uint64_t)status, (uint64_t)VFS_STATUS_NOT_FOUND);

	kfree(entry);
}

/* "/big" holds 2500 files f0000..f2499 with the content "n<i>\n": checked by rule. */
static void st_check_big(selftest_t *t, vnode_t root)
{
	vnode_t big;
	vfs_status_t status = vnode_lookup(root, "big", &big);

	if (status != VFS_STATUS_OK) {
		st_fail(t, "/big", "lookup", (uint64_t)status, 0ULL);
		return;
	}

	vfs_dirent_t *entry = kcalloc(1U, sizeof(*entry));
	uint64_t cursor = 0ULL;
	uint32_t count = 0U;
	uint8_t *seen = kcalloc(2500U, 1U);

	if (entry == 0 || seen == 0) {
		st_fail(t, "/big", "out of memory", 0ULL, 0ULL);
		vnode_rele(big);
		kfree(entry);
		kfree(seen);
		return;
	}

	while (vnode_readdir(big, &cursor, entry) == VFS_STATUS_OK) {
		if (entry->name[0] == '.') continue;

		uint32_t number = 0U;
		bool valid = entry->name_length == 5U && entry->name[0] == 'f';

		for (uint32_t digit = 1U; valid && digit < 5U; digit++) {
			if (entry->name[digit] < '0' || entry->name[digit] > '9') valid = false;
			else number = number * 10U + (uint32_t)(entry->name[digit] - '0');
		}

		if (!valid || number >= 2500U || seen[number] != 0U) {
			st_fail(t, "/big", "unexpected or repeated entry", count, 0ULL);
			break;
		}

		seen[number] = 1U;
		count++;

		/* Content must be "n<number>\n". */
		char expected[16];
		uint32_t length = 0U;
		char digits[8];
		uint32_t nd = 0U;
		uint32_t value = number;

		do { digits[nd++] = (char)('0' + value % 10U); value /= 10U; } while (value != 0U);
		expected[length++] = 'n';
		while (nd != 0U) expected[length++] = digits[--nd];
		expected[length++] = '\n';

		vnode_t file;
		if (vnode_lookup(big, entry->name, &file) != VFS_STATUS_OK) {
			st_fail(t, "/big", "lookup of a listed name", number, 0ULL);
			break;
		}

		uint64_t done = 0ULL;
		vfs_status_t rs = vnode_read(file, 0ULL, t->buffer, 64U, &done);
		vnode_rele(file);

		if (rs != VFS_STATUS_OK || done != length || memcmp(t->buffer, expected, length) != 0) {
			st_fail(t, "/big", "file content", done, length);
			break;
		}
	}

	if (count != 2500U) st_fail(t, "/big", "entry count", count, 2500ULL);

	kfree(entry);
	kfree(seen);
	vnode_rele(big);
}

/* Every mutating operation is refused with READ_ONLY and nothing reaches the device. */
static void st_check_read_only(selftest_t *t)
{
	char path[SELFTEST_PATH_MAX];
	uint64_t writes_before = t->device->write_operations;
	filedesc_t filedesc = &proc_kernel()->p_fd;
	uint32_t descriptor;
	vnode_t vnode;
	uint64_t written = 0ULL;
	vfs_status_t status;

	st_strcpy(path, t->mount_path);
	st_strcat(path, "/nxu-new-directory");
	status = vfs_mkdir(path);
	if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "mkdir was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

	st_strcpy(path, t->mount_path);
	st_strcat(path, "/nxu-new-file");
	status = vfs_create(path, VNODE_TYPE_REGULAR, &vnode);
	if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "create was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

	status = vfs_open(filedesc, path, VFS_OPEN_WRITE | VFS_OPEN_CREATE, &descriptor);
	if (status != VFS_STATUS_READ_ONLY) {
		st_fail(t, path, "open with create was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);
		if (status == VFS_STATUS_OK) (void)vfs_close(filedesc, descriptor);
	}

	if (t->first_file[0] != '\0') {
		st_strcpy(path, t->mount_path);
		st_strcat(path, t->first_file);

		status = vfs_unlink(path);
		if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "unlink was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

		status = vfs_open(filedesc, path, VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE, &descriptor);
		if (status != VFS_STATUS_READ_ONLY) {
			st_fail(t, path, "open with truncate was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);
			if (status == VFS_STATUS_OK) (void)vfs_close(filedesc, descriptor);
		}

		status = vfs_open(filedesc, path, VFS_OPEN_WRITE, &descriptor);
		if (status == VFS_STATUS_OK) {
			status = vfs_write(filedesc, descriptor, "x", 1ULL, &written);
			if (status != VFS_STATUS_READ_ONLY || written != 0ULL) st_fail(t, path, "write was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);
			(void)vfs_close(filedesc, descriptor);
		}

		if (vfs_lookup(path, &vnode) == VFS_STATUS_OK) {
			status = vnode_write(vnode, 0ULL, "x", 1ULL, &written);
			if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "vnode_write was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

			status = vnode_truncate(vnode, 0ULL);
			if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "truncate was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

			vnode_t created;
			vnode_t parent = vnode->v_parent;
			if (parent != 0) {
				status = vnode_create(parent, "nxu-x", VNODE_TYPE_REGULAR, &created);
				if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "vnode_create was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);

				status = vnode_unlink(parent, "nxu-x");
				if (status != VFS_STATUS_READ_ONLY) st_fail(t, path, "vnode_unlink was not refused", (uint64_t)status, (uint64_t)VFS_STATUS_READ_ONLY);
			}

			vnode_rele(vnode);
		}
	}

	if (t->device->write_operations != writes_before) st_fail(t, t->mount_path, "the device was written to", t->device->write_operations, writes_before);

	/* The read-only volume also says so in its space report. */
	vfs_space_info_t info;
	if (vfs_space_info(t->mount_path, &info) != VFS_STATUS_OK || !info.read_only) st_fail(t, t->mount_path, "space_info does not report read-only", info.read_only, 1ULL);

	t->refusals++;
}

static const btrfs_expect_fixture_t *st_fixture(const char *name)
{
	for (uint32_t index = 0U; index < sizeof(g_expect_fixtures) / sizeof(g_expect_fixtures[0]); index++) {
		if (strcmp(g_expect_fixtures[index].name, name) == 0) return &g_expect_fixtures[index];
	}

	return 0;
}

/* Run one positive spec: NAME[@SUBVOLID][+verify]. */
static bool st_run_fixture(const char *token, uint32_t device_index)
{
	char name[48];
	size_t length = 0U;
	bool verify = false;
	uint64_t subvol = 0ULL;

	while (token[length] != '\0' && length + 1U < sizeof(name)) {
		if (token[length] == '+') break;
		name[length] = token[length];
		length++;
	}
	name[length] = '\0';
	if (strcmp(token + length, "+verify") == 0) verify = true;

	const char *at = name;
	while (*at != '\0' && *at != '@') at++;
	if (*at == '@') {
		for (const char *digit = at + 1; *digit >= '0' && *digit <= '9'; digit++) subvol = subvol * 10ULL + (uint64_t)(*digit - '0');
	}

	const btrfs_expect_fixture_t *fixture = st_fixture(name);
	block_device_t device = block_device_get(device_index);

	if (fixture == 0 || device == 0) {
		kprintf("btrfs_selftest: FAIL %s: no expected listing or no block device %u\n", token, device_index);
		return false;
	}

	selftest_t *t = kcalloc(1U, sizeof(*t));
	if (t == 0) return false;

	t->fixture = fixture;
	t->device = device;
	t->seen = kcalloc((size_t)fixture->count, 1U);
	t->buffer = kmalloc(65536U + 64U);
	t->linkbuf = kmalloc(SELFTEST_LINK_MAX);
	st_strcpy(t->mount_path, "/btrfs");
	t->mount_path[6] = (char)('0' + device_index);
	t->mount_path[7] = '\0';

	if (t->seen == 0 || t->buffer == 0 || t->linkbuf == 0) {
		kprintf("btrfs_selftest: FAIL %s: out of memory\n", token);
		return false;
	}

	kprintf("btrfs_selftest: %s: mounting %s (%llu sectors) at %s%s%s\n", token, device->name, (unsigned long long)device->sector_count, t->mount_path, verify ? ", data checksums on" : "", subvol != 0ULL ? ", explicit subvolume" : "");

	vfs_status_t status = vfs_mkdir(t->mount_path);
	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) {
		kprintf("btrfs_selftest: FAIL %s: mkdir %s: %s\n", token, t->mount_path, vfs_status_name(status));
		return false;
	}

	btrfs_mount_options_t options = { subvol, verify, false };
	btrfs_set_next_mount_options(&options);

	status = vfs_mount("btrfs", device, t->mount_path);
	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_selftest: FAIL %s: mount: %s (%s)\n", token, vfs_status_name(status), btrfs_last_mount_status_name());
		return false;
	}

	vnode_t root;
	status = vfs_lookup(t->mount_path, &root);
	if (status != VFS_STATUS_OK) {
		st_fail(t, t->mount_path, "lookup of the mount root", (uint64_t)status, 0ULL);
	} else {
		char path[SELFTEST_PATH_MAX];

		st_strcpy(path, "/");
		st_check_node(t, root, path, VNODE_TYPE_DIRECTORY);   /* the root itself is "/" in the listing */
		st_walk(t, root, path, 0U);
		if (fixture->special == 1) st_check_big(t, root);

		/* Entries must match in both directions: nothing in Linux's listing may be missing. */
		for (int index = 0; index < fixture->count; index++) {
			if (t->seen[index] == 0 && strcmp(fixture->entries[index].path, "/") != 0) st_fail(t, fixture->entries[index].path, "in Linux's listing but not seen", 0ULL, 1ULL);
			if (t->seen[index] > 1) st_fail(t, fixture->entries[index].path, "seen more than once", t->seen[index], 1ULL);
		}

		/* Path-based access through the VFS finds the same things. */
		if (t->first_file[0] != '\0') {
			st_strcpy(path, t->mount_path);
			st_strcat(path, t->first_file);
			vnode_t viapath;

			if (vfs_lookup(path, &viapath) != VFS_STATUS_OK) st_fail(t, path, "vfs_lookup by path", 0ULL, 0ULL);
			else vnode_rele(viapath);
		}

		vnode_rele(root);
	}

	st_check_read_only(t);

	btrfs_dump();

	status = vfs_unmount(t->mount_path);
	if (status != VFS_STATUS_OK) st_fail(t, t->mount_path, "unmount (a leaked vnode reference?)", (uint64_t)status, 0ULL);

	int failures = t->failures;
	kprintf("btrfs_selftest: %s %s: %u entries, %u files, %llu bytes, %u refusals checked, %u readdir resumes, %d failures\n", failures == 0 ? "PASS" : "FAIL", token, t->entries, t->files, (unsigned long long)t->bytes, t->refusals, t->resumes, failures);

	kfree(t->seen);
	kfree(t->buffer);
	kfree(t->linkbuf);
	kfree(t);
	return failures == 0;
}

typedef struct {
	const char *name;
	btrfs_status_t status;
} selftest_status_name_t;

static const selftest_status_name_t g_status_names[] = {
	{ "bad-magic", BTRFS_ERR_BAD_MAGIC },
	{ "csum", BTRFS_ERR_CSUM },
	{ "corrupt", BTRFS_ERR_CORRUPT },
	{ "truncated", BTRFS_ERR_TRUNCATED },
	{ "unsupported-feature", BTRFS_ERR_UNSUPPORTED_FEATURE },
	{ "unsupported-csum", BTRFS_ERR_UNSUPPORTED_CSUM },
	{ "unsupported-profile", BTRFS_ERR_UNSUPPORTED_PROFILE },
	{ "log-tree", BTRFS_ERR_LOG_TREE },
	{ "not-found", BTRFS_ERR_NOT_FOUND }
};

/* Run one negative spec: the mount must fail with exactly this status. */
static bool st_run_refusal(const char *name, uint32_t device_index)
{
	btrfs_status_t expected = BTRFS_OK;
	bool known = false;

	for (uint32_t index = 0U; index < sizeof(g_status_names) / sizeof(g_status_names[0]); index++) {
		if (strcmp(g_status_names[index].name, name) == 0) {
			expected = g_status_names[index].status;
			known = true;
		}
	}

	block_device_t device = block_device_get(device_index);
	if (!known || device == 0) {
		kprintf("btrfs_selftest: FAIL !%s: unknown status or no block device %u\n", name, device_index);
		return false;
	}

	char path[16] = "/btrfs";
	path[6] = (char)('0' + device_index);
	path[7] = '\0';

	kprintf("btrfs_selftest: !%s: mounting %s, expecting a clean refusal (%s)\n", name, device->name, btrfs_status_name(expected));

	vfs_status_t status = vfs_mkdir(path);
	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) return false;

	uint32_t mounts_before = btrfs_mount_count();
	btrfs_set_next_mount_options(0);
	status = vfs_mount("btrfs", device, path);

	bool ok = status != VFS_STATUS_OK && btrfs_last_mount_status() == expected && btrfs_mount_count() == mounts_before;

	if (status == VFS_STATUS_OK) (void)vfs_unmount(path);

	kprintf("btrfs_selftest: %s !%s: vfs status \"%s\", btrfs status \"%s\", mounts %u -> %u; kernel still running\n", ok ? "PASS" : "FAIL", name, vfs_status_name(status), btrfs_last_mount_status_name(), mounts_before, btrfs_mount_count());
	return ok;
}

bool btrfs_selftest_run(const char *spec)
{
	static bool registered;

	g_failures_total = 0;

	if (!registered) {
		if (!btrfs_register()) {
			kputs("btrfs_selftest: FAIL: btrfs_register\n");
			return false;
		}
		registered = true;
	}

	/* The core's crc32c against the standard check value, before anything is trusted to it. */
	if (btrfs_csum_crc32c("123456789", 9U) != 0xE3069283U) {
		kputs("btrfs_selftest: FAIL: crc32c check value\n");
		return false;
	}

	kprintf("btrfs_selftest: spec \"%s\", %u block devices\n", spec, block_device_count());

	uint32_t device_index = 1U;
	const char *cursor = spec;

	while (*cursor != '\0') {
		char token[64];
		size_t length = 0U;

		while (cursor[length] != '\0' && cursor[length] != ',' && length + 1U < sizeof(token)) {
			token[length] = cursor[length];
			length++;
		}
		token[length] = '\0';
		cursor += length;
		if (*cursor == ',') cursor++;
		if (length == 0U) continue;

		bool ok = token[0] == '!' ? st_run_refusal(token + 1, device_index) : st_run_fixture(token, device_index);
		if (!ok) g_failures_total++;
		device_index++;
	}

	if (g_failures_total == 0) kputs("btrfs_selftest: ALL PASSED\n");
	else kprintf("btrfs_selftest: FAILED (%d failures)\n", g_failures_total);

	return g_failures_total == 0;
}
