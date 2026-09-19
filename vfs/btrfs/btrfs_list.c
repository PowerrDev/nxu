/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_list.c
 *
 * See btrfs_list.h. Walks with vnode_readdir/vnode_lookup, the same calls the
 * filesystem's own self-test uses, so a long name (past the 63-byte path
 * component limit of vfs_lookup) still works: lookups go one component at a
 * time through vnode_lookup.
 */

#include <vfs/btrfs/btrfs_list.h>

#include <vfs/btrfs/btrfs.h>

#include <drivers/block/block_device.h>
#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <vfs/vfs.h>
#include <vfs/vnode.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LIST_PATH_MAX 1024U
#define LIST_DEPTH_MAX 32U
#define LIST_DEFAULT_MAX_ENTRIES 512U
#define LIST_LINK_MAX 1024U
#define LIST_CAT_MAX 8192U

/* POSIX permission bits of va_mode. */
#define LIST_S_IRUSR 0400U
#define LIST_S_IWUSR 0200U
#define LIST_S_IXUSR 0100U

typedef struct {
	uint32_t max_entries;
	uint32_t printed;
	uint32_t entries;
	uint32_t directories;
	uint32_t files;
	uint32_t symlinks;
	uint32_t others;
	uint32_t errors;
	uint64_t bytes;
	char *link;
} list_walk_t;

static char list_type_char(vnode_type_t type)
{
	switch (type) {
	case VNODE_TYPE_DIRECTORY: return 'd';
	case VNODE_TYPE_SYMLINK: return 'l';
	case VNODE_TYPE_CHARACTER: return 'c';
	case VNODE_TYPE_BLOCK: return 'b';
	case VNODE_TYPE_FIFO: return 'p';
	case VNODE_TYPE_SOCKET: return 's';
	default: return '-';
	}
}

/* Nine rwx characters from the permission bits. */
static void list_mode_string(uint32_t mode, char out[10])
{
	for (uint32_t group = 0U; group < 3U; group++) {
		uint32_t shift = (2U - group) * 3U;

		out[group * 3U + 0U] = ((mode >> shift) & 4U) != 0U ? 'r' : '-';
		out[group * 3U + 1U] = ((mode >> shift) & 2U) != 0U ? 'w' : '-';
		out[group * 3U + 2U] = ((mode >> shift) & 1U) != 0U ? 'x' : '-';
	}

	out[9] = '\0';
}

static void list_print_entry(list_walk_t *walk, vnode_t vnode, vnode_type_t type, const char *path)
{
	walk->entries++;

	if (type == VNODE_TYPE_DIRECTORY) walk->directories++;
	else if (type == VNODE_TYPE_REGULAR) walk->files++;
	else if (type == VNODE_TYPE_SYMLINK) walk->symlinks++;
	else walk->others++;

	vnode_attr_t attr;

	memset(&attr, 0, sizeof(attr));

	if (vnode_getattr(vnode, &attr) != VFS_STATUS_OK) {
		walk->errors++;
		kprintf("btrfs_list_run: getattr failed for %s\n", path);
		return;
	}

	if (type == VNODE_TYPE_REGULAR) walk->bytes += attr.va_size;

	if (walk->printed >= walk->max_entries) return;
	walk->printed++;

	char mode[10];

	list_mode_string(attr.va_mode & (LIST_S_IRUSR | LIST_S_IWUSR | LIST_S_IXUSR | 077U), mode);

	if (type == VNODE_TYPE_SYMLINK) {
		uint64_t length = 0ULL;
		vfs_status_t status = vnode_readlink(vnode, walk->link, LIST_LINK_MAX - 1U, &length);

		if (status != VFS_STATUS_OK) {
			walk->link[0] = '?';
			length = 1ULL;
		}

		walk->link[length < LIST_LINK_MAX ? length : LIST_LINK_MAX - 1U] = '\0';
		kprintf("%c%s %u %llu %s -> %s\n", list_type_char(type), mode, attr.va_nlink, (unsigned long long)attr.va_size, path, walk->link);
	} else {
		kprintf("%c%s %u %llu %s\n", list_type_char(type), mode, attr.va_nlink, (unsigned long long)attr.va_size, path);
	}
}

static void list_walk(list_walk_t *walk, vnode_t directory, char *path, uint32_t depth)
{
	size_t base = strlen(path);
	uint64_t cursor = 0ULL;
	vfs_dirent_t *entry = kcalloc(1U, sizeof(*entry));

	if (entry == 0) {
		walk->errors++;
		kputln("btrfs_list_run: out of memory");
		return;
	}

	for (;;) {
		vfs_status_t status = vnode_readdir(directory, &cursor, entry);

		if (status == VFS_STATUS_END_OF_DIRECTORY) break;

		if (status != VFS_STATUS_OK) {
			walk->errors++;
			kprintf("btrfs_list_run: readdir of %s failed: %s\n", path, vfs_status_name(status));
			break;
		}

		if ((entry->name_length == 1U && entry->name[0] == '.') || (entry->name_length == 2U && entry->name[0] == '.' && entry->name[1] == '.')) continue;

		if (base + 1U + entry->name_length + 1U >= LIST_PATH_MAX) {
			walk->errors++;
			kprintf("btrfs_list_run: path too long under %s, entry skipped\n", path);
			continue;
		}

		if (base != 1U) path[base] = '/';
		memcpy(path + (base == 1U ? 1U : base + 1U), entry->name, (size_t)entry->name_length + 1U);

		vnode_t child;

		status = vnode_lookup(directory, entry->name, &child);

		if (status != VFS_STATUS_OK) {
			walk->errors++;
			kprintf("btrfs_list_run: lookup of %s failed: %s\n", path, vfs_status_name(status));
		} else {
			list_print_entry(walk, child, entry->type, path);

			if (child->v_type == VNODE_TYPE_DIRECTORY) {
				if (depth + 1U > LIST_DEPTH_MAX) {
					walk->errors++;
					kprintf("btrfs_list_run: %s is nested deeper than %u levels, not descended\n", path, LIST_DEPTH_MAX);
				} else {
					list_walk(walk, child, path, depth + 1U);
				}
			}

			vnode_rele(child);
		}

		path[base] = '\0';
	}

	kfree(entry);
}

/* Resolve a path relative to the mount root one component at a time (no length limit per name). */
static vfs_status_t list_resolve(vnode_t root, const char *path, vnode_t *result)
{
	vnode_t current = root;

	vnode_reference(current);

	const char *cursor = path;

	while (*cursor != '\0') {
		while (*cursor == '/') cursor++;
		if (*cursor == '\0') break;

		char name[VFS_DIRENT_NAME_MAX + 1U];
		size_t length = 0U;

		while (cursor[length] != '\0' && cursor[length] != '/') {
			if (length >= VFS_DIRENT_NAME_MAX) {
				vnode_rele(current);
				return VFS_STATUS_PATH_TOO_LONG;
			}

			name[length] = cursor[length];
			length++;
		}

		name[length] = '\0';
		cursor += length;

		vnode_t next;
		vfs_status_t status = vnode_lookup(current, name, &next);

		vnode_rele(current);

		if (status != VFS_STATUS_OK) return status;

		current = next;
	}

	*result = current;
	return VFS_STATUS_OK;
}

static bool list_cat(vnode_t root, const char *path)
{
	vnode_t file;
	vfs_status_t status = list_resolve(root, path, &file);

	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_list_run: %s: %s\n", path, vfs_status_name(status));
		return false;
	}

	vnode_attr_t attr;

	memset(&attr, 0, sizeof(attr));

	if (vnode_getattr(file, &attr) != VFS_STATUS_OK || file->v_type != VNODE_TYPE_REGULAR) {
		kprintf("btrfs_list_run: %s is not a regular file\n", path);
		vnode_rele(file);
		return false;
	}

	uint64_t want = attr.va_size < LIST_CAT_MAX ? attr.va_size : LIST_CAT_MAX;
	uint8_t *buffer = kmalloc((size_t)want + 1U);

	if (buffer == 0) {
		kputln("btrfs_list_run: out of memory");
		vnode_rele(file);
		return false;
	}

	uint64_t got = 0ULL;

	status = vnode_read(file, 0ULL, buffer, want, &got);
	vnode_rele(file);

	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_list_run: read of %s failed: %s\n", path, vfs_status_name(status));
		kfree(buffer);
		return false;
	}

	kprintf("btrfs_list_run: %s: %llu bytes%s\n", path, (unsigned long long)attr.va_size, attr.va_size > want ? " (first part shown)" : "");
	kputln("---- begin file ----");

	for (uint64_t index = 0ULL; index < got; index++) {
		uint8_t byte = buffer[index];

		if (byte == '\n' || (byte >= 0x20U && byte < 0x7FU)) kputc((char)byte);
		else kputc('.');
	}

	if (got != 0ULL && buffer[got - 1ULL] != '\n') kputc('\n');
	kputln("---- end file ----");

	kfree(buffer);
	return true;
}

bool btrfs_list_run(const btrfs_list_request_t *request)
{
	static bool registered;

	if (!registered) {
		if (!btrfs_register()) {
			kputln("btrfs_list_run: btrfs_register failed");
			return false;
		}

		registered = true;
	}

	block_device_t device = block_device_get(request->device_index);

	if (device == 0) {
		kprintf("btrfs_list_run: no block device %u (%u present)\n", request->device_index, block_device_count());
		return false;
	}

	char mount_path[8];

	memcpy(mount_path, "/btrfs", 6U);
	mount_path[6] = (char)('0' + (request->device_index % 10U));
	mount_path[7] = '\0';

	kprintf("btrfs_list_run: mounting %s (%llu sectors) at %s%s\n", device->name, (unsigned long long)device->sector_count, mount_path, request->verify ? ", data checksums on" : "");

	vfs_status_t status = vfs_mkdir(mount_path);

	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) {
		kprintf("btrfs_list_run: mkdir %s: %s\n", mount_path, vfs_status_name(status));
		return false;
	}

	btrfs_mount_options_t options = { request->subvolume, request->verify, false };

	btrfs_set_next_mount_options(&options);

	status = vfs_mount("btrfs", device, mount_path);

	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_list_run: mount failed: %s (%s)\n", vfs_status_name(status), btrfs_last_mount_status_name());
		return false;
	}

	bool ok = true;
	vnode_t root;

	status = vfs_lookup(mount_path, &root);

	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_list_run: lookup of %s: %s\n", mount_path, vfs_status_name(status));
		ok = false;
	} else {
		if (request->list) {
			list_walk_t walk;
			char *path = kcalloc(1U, LIST_PATH_MAX);

			memset(&walk, 0, sizeof(walk));
			walk.max_entries = request->max_entries != 0U ? request->max_entries : LIST_DEFAULT_MAX_ENTRIES;
			walk.link = kmalloc(LIST_LINK_MAX);

			if (path == 0 || walk.link == 0) {
				kputln("btrfs_list_run: out of memory");
				ok = false;
			} else {
				path[0] = '/';
				kputln("---- tree ----");
				list_walk(&walk, root, path, 0U);
				kputln("---- end tree ----");

				kprintf(
					"btrfs_list_run: %u entries: %u directories, %u files (%llu bytes), %u symlinks, %u other\n",
					walk.entries,
					walk.directories,
					walk.files,
					(unsigned long long)walk.bytes,
					walk.symlinks,
					walk.others
				);

				if (walk.entries > walk.printed) {
					kprintf("btrfs_list_run: only the first %u entries were printed (raise btrfs-max)\n", walk.printed);
				}

				if (walk.errors != 0U) {
					kprintf("btrfs_list_run: %u error(s) while walking\n", walk.errors);
					ok = false;
				}
			}

			if (path != 0) kfree(path);
			if (walk.link != 0) kfree(walk.link);
		}

		if (request->cat_path != 0 && !list_cat(root, request->cat_path)) ok = false;

		vnode_rele(root);
	}

	status = vfs_unmount(mount_path);

	if (status != VFS_STATUS_OK) {
		kprintf("btrfs_list_run: unmount failed (a leaked vnode reference?): %s\n", vfs_status_name(status));
		ok = false;
	} else {
		kputln("btrfs_list_run: unmounted");
	}

	return ok;
}
