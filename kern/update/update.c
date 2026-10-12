/* Copyright (c) 2026 NXU Project. All rights reserved. */
#include <kern/update/update.h>
#include <drivers/block/file_image.h>
#include <frameworks/SoftwareUpdate.framework/include/nsw.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/recovery_volume.h>
#include <kern/console/console.h>
#include <kern/loader/elf_format.h>
#include <kern/machine/system.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <libk/sha256.h>
#include <libk/string.h>
#include <vfs/vfs.h>

#define UPDATE_ENTRY_MAX 1024U
#define UPDATE_RELATIVE_MAX 240U
#define UPDATE_PLAN_MAGIC 0x4e58554aU
#define UPDATE_FILE 1U
#define UPDATE_DIRECTORY 2U
#define UPDATE_SOURCE "/UpdateSource"
#define UPDATE_RECOVERY "/UpdateRecovery"

typedef struct {
	uint32_t kind;
	uint32_t old_present;
	uint32_t old_size;
	uint32_t new_size;
	uint8_t old_hash[32];
	uint8_t new_hash[32];
	char relative[UPDATE_RELATIVE_MAX];
} update_entry_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	uint8_t entries_hash[32];
	uint8_t manifest_hash[32];
	char build[64];
} update_plan_header_t;

_Static_assert(sizeof(update_entry_t) == 320, "update journal must be architecture independent");
_Static_assert(sizeof(update_plan_header_t) == 140, "update journal header layout changed");

static update_entry_t g_entries[UPDATE_ENTRY_MAX];
static uint32_t g_count;
static uint8_t g_io[16384];
static char g_manifest_data[8192];
static char g_transaction[256];
static nsw_request_t g_request;
static nsw_manifest_t g_manifest;
static block_file_image_t g_source_image;
static block_file_image_t g_recovery_image;
static bool g_trial;

static bool
update_path(char output[256], const char *base, const char *name)
{
	uint32_t prefix = strlen(base);
	uint32_t suffix = strlen(name);
	if (prefix + suffix + 2 > 256) {
		return false;
	}
	memcpy(output, base, prefix);
	output[prefix++] = '/';
	memcpy(output + prefix, name, suffix + 1);
	return true;
}

static bool
update_relative(const char *path)
{
	uint32_t length = 0;
	while (length < UPDATE_RELATIVE_MAX && path[length] != 0) {
		length++;
	}
	if (length == 0 || length == UPDATE_RELATIVE_MAX || path[0] == '/') {
		return false;
	}
	const char *roots[] = {"System", "Applications", "bin", "init"};
	bool allowed = false;
	for (uint32_t root = 0; root < 4; root++) {
		uint32_t size = strlen(roots[root]);
		allowed |= length >= size && memcmp(path, roots[root], size) == 0 && (path[size] == 0 || path[size] == '/');
	}
	if (!allowed) {
		return false;
	}
	uint32_t start = 0;
	for (uint32_t index = 0; index <= length; index++) {
		if (path[index] != '/' && path[index] != 0) {
			if ((uint8_t)path[index] < 32 || path[index] == 127) {
				return false;
			}
			continue;
		}
		uint32_t size = index - start;
		if (size == 0 || (size == 1 && path[start] == '.') ||
			(size == 2 && path[start] == '.' && path[start + 1] == '.')) {
			return false;
		}
		start = index + 1;
	}
	return true;
}

static bool
update_read_file(const char *path, void *bytes, uint32_t capacity, uint32_t *size)
{
	vnode_t vnode;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) {
		return false;
	}
	bool success = vnode->v_type == VNODE_TYPE_REGULAR && vnode->v_size <= capacity;
	*size = success ? vnode->v_size : 0;
	uint32_t complete = 0;
	while (success && complete < *size) {
		uint64_t read = 0;
		success = vnode_read(vnode, complete, (char *)bytes + complete, *size - complete, &read) == VFS_STATUS_OK &&
				  read > 0 && read <= *size - complete;
		if (success) {
			complete += read;
		}
	}
	vnode_rele(vnode);
	return success;
}

static bool
update_text(const char *path, const char *expected)
{
	char bytes[32];
	uint32_t size;
	return update_read_file(path, bytes, sizeof(bytes), &size) && size == strlen(expected) &&
		   memcmp(bytes, expected, size) == 0;
}

static bool
update_write(const char *path, const void *bytes, uint32_t size)
{
	filedesc_t files = &proc_kernel()->p_fd;
	uint32_t descriptor;
	if (vfs_open(files, path, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		return false;
	}
	uint32_t complete = 0;
	bool success = true;
	while (complete < size) {
		uint64_t written = 0;
		uint32_t count = size - complete;
		if (count > 16384) {
			count = 16384;
		}
		if (vfs_write(files, descriptor, (const char *)bytes + complete, count, &written) != VFS_STATUS_OK ||
			written == 0 || written > count) {
			success = false;
			break;
		}
		complete += written;
	}
	return vfs_close(files, descriptor) == VFS_STATUS_OK && success;
}

static bool
update_state(const char *state)
{
	char path[256];
	return update_path(path, g_transaction, "State") && update_write(path, state, strlen(state)) &&
		   vfs_sync_all() == VFS_STATUS_OK;
}

static bool
update_hash(const char *path, uint8_t digest[32], uint32_t *size)
{
	vnode_t vnode;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) {
		return false;
	}
	if (vnode->v_type != VNODE_TYPE_REGULAR || vnode->v_size > UINT32_MAX) {
		vnode_rele(vnode);
		return false;
	}
	*size = vnode->v_size;
	sha256_context_t context;
	sha256_init(&context);
	bool success = true;
	for (uint64_t offset = 0; offset < *size;) {
		uint64_t read = 0;
		uint32_t count = *size - offset;
		if (count > sizeof(g_io)) {
			count = sizeof(g_io);
		}
		if (vnode_read(vnode, offset, g_io, count, &read) != VFS_STATUS_OK || read == 0 || read > count) {
			success = false;
			break;
		}
		sha256_update(&context, g_io, read);
		offset += read;
	}
	sha256_final(&context, digest);
	vnode_rele(vnode);
	return success;
}

static bool
update_matches(const char *path, const uint8_t expected[32], uint32_t size)
{
	uint8_t digest[32];
	uint32_t actual_size;
	return update_hash(path, digest, &actual_size) && actual_size == size && memcmp(digest, expected, 32) == 0;
}

static bool
update_copy(const char *source, const char *target, const uint8_t expected[32], uint32_t size, bool replace)
{
	if (!update_matches(source, expected, size)) {
		return false;
	}
	if (replace) {
		vfs_status_t status = vfs_unlink(target);
		if (status != VFS_STATUS_OK && status != VFS_STATUS_NOT_FOUND) {
			return false;
		}
	}
	vnode_t input;
	if (vfs_lookup(source, &input) != VFS_STATUS_OK) {
		return false;
	}
	uint32_t descriptor;
	filedesc_t files = &proc_kernel()->p_fd;
	if (vfs_open(files, target, VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, &descriptor) != VFS_STATUS_OK) {
		vnode_rele(input);
		return false;
	}
	bool success = true;
	for (uint64_t offset = 0; offset < size;) {
		uint64_t read = 0;
		uint32_t count = size - offset;
		if (count > sizeof(g_io)) {
			count = sizeof(g_io);
		}
		if (vnode_read(input, offset, g_io, count, &read) != VFS_STATUS_OK || read == 0 || read > count) {
			success = false;
			break;
		}
		uint64_t complete = 0;
		while (complete < read) {
			uint64_t written = 0;
			if (vfs_write(files, descriptor, g_io + complete, read - complete, &written) != VFS_STATUS_OK ||
				written == 0 || written > read - complete) {
				success = false;
				break;
			}
			complete += written;
		}
		if (!success) {
			break;
		}
		offset += read;
	}
	if (vfs_close(files, descriptor) != VFS_STATUS_OK) {
		success = false;
	}
	vnode_rele(input);
	return success && update_matches(target, expected, size);
}

static bool
update_mkdir(const char *path)
{
	vfs_status_t status = vfs_mkdir(path);
	return status == VFS_STATUS_OK || status == VFS_STATUS_EXISTS;
}

static bool
update_backup_path(char path[256], uint32_t index)
{
	char name[16] = "Backup/00000000";
	const char *digits = "0123456789abcdef";
	for (uint32_t digit = 0; digit < 8; digit++) {
		name[7 + digit] = digits[(index >> ((7 - digit) * 4)) & 15];
	}
	return update_path(path, g_transaction, name);
}

static bool
update_add(const char *relative)
{
	if (g_count == UPDATE_ENTRY_MAX || !update_relative(relative)) {
		return false;
	}
	update_entry_t *entry = &g_entries[g_count];
	memset(entry, 0, sizeof(*entry));
	memcpy(entry->relative, relative, strlen(relative) + 1);
	char source[256], target[256];
	if (!update_path(source, UPDATE_SOURCE, relative) || !update_path(target, "/disk", relative)) {
		return false;
	}
	vnode_t vnode;
	if (vfs_lookup(source, &vnode) != VFS_STATUS_OK) {
		return false;
	}
	entry->kind = vnode->v_type == VNODE_TYPE_DIRECTORY ? UPDATE_DIRECTORY
				  : vnode->v_type == VNODE_TYPE_REGULAR ? UPDATE_FILE
														: 0;
	vnode_rele(vnode);
	if (entry->kind == 0) {
		return false;
	}
	vfs_status_t status = vfs_lookup(target, &vnode);
	if (status == VFS_STATUS_OK) {
		bool same_type = entry->kind == UPDATE_DIRECTORY ? vnode->v_type == VNODE_TYPE_DIRECTORY
														 : vnode->v_type == VNODE_TYPE_REGULAR;
		vnode_rele(vnode);
		if (!same_type) {
			return false;
		}
		entry->old_present = 1;
	} else if (status != VFS_STATUS_NOT_FOUND) {
		return false;
	}
	if (entry->kind == UPDATE_FILE &&
		(!update_hash(source, entry->new_hash, &entry->new_size) ||
		 (entry->old_present && !update_hash(target, entry->old_hash, &entry->old_size)))) {
		return false;
	}
	g_count++;
	return true;
}

static bool
update_elf(const char *path)
{
	uint8_t raw[LOADER_ELF_HEADER_SIZE];
	vnode_t vnode;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) {
		return false;
	}
	uint64_t size = 0;
	bool success = vnode->v_type == VNODE_TYPE_REGULAR &&
				   vnode_read(vnode, 0, raw, sizeof(raw), &size) == VFS_STATUS_OK && size == sizeof(raw);
	vnode_rele(vnode);
	loader_header_t header;
	if (success) {
		loader_disk_header_t disk;
		memcpy(&disk, raw, sizeof(disk));
#if LOADER_ELF_WIDEN
		loader_header_widen(&disk, &header);
#else
		header = disk;
#endif
		success = loader_header_valid(&header);
	}
	return success;
}

static bool
update_plan(void)
{
	g_count = 0;
	const char *roots[] = {"System", "Applications", "bin", "init"};
	for (uint32_t index = 0; index < 4; index++) {
		char path[256];
		vnode_t vnode;
		if (!update_path(path, UPDATE_SOURCE, roots[index])) {
			return false;
		}
		vfs_status_t status = vfs_lookup(path, &vnode);
		if (status == VFS_STATUS_NOT_FOUND && index != 0) {
			continue;
		}
		if (status != VFS_STATUS_OK) {
			return false;
		}
		bool directory = vnode->v_type == VNODE_TYPE_DIRECTORY;
		vnode_rele(vnode);
		if (!directory || !update_add(roots[index])) {
			return false;
		}
	}
	for (uint32_t index = 0; index < g_count; index++) {
		if (g_entries[index].kind != UPDATE_DIRECTORY) {
			continue;
		}
		char path[256];
		if (!update_path(path, UPDATE_SOURCE, g_entries[index].relative)) {
			return false;
		}
		vnode_t directory;
		if (vfs_lookup(path, &directory) != VFS_STATUS_OK) {
			return false;
		}
		uint64_t offset = 0;
		vfs_dirent_t child;
		vfs_status_t status;
		while ((status = vnode_readdir(directory, &offset, &child)) == VFS_STATUS_OK) {
			if (strcmp(child.name, ".") == 0 || strcmp(child.name, "..") == 0) {
				continue;
			}
			char relative[256];
			bool safe_name = true;
			for (uint32_t character = 0; child.name[character] != 0; character++) {
				safe_name &= child.name[character] != '/';
			}
			if (!safe_name || !update_path(relative, g_entries[index].relative, child.name) || !update_add(relative)) {
				vnode_rele(directory);
				return false;
			}
		}
		vnode_rele(directory);
		if (status != VFS_STATUS_END_OF_DIRECTORY) {
			return false;
		}
	}
	const char *critical[] = {"bootd", "logd", "patchd"};
	for (uint32_t index = 0; index < 3; index++) {
		char relative[128] = "System/Library/CoreServices/";
		uint32_t prefix = strlen(relative);
		memcpy(relative + prefix, critical[index], strlen(critical[index]) + 1);
		char path[256];
		if (!update_path(path, UPDATE_SOURCE, relative) || !update_elf(path)) {
			return false;
		}
		update_entry_t *primary = 0, *recovery = 0;
		for (uint32_t entry = 0; entry < g_count; entry++) {
			if (strcmp(g_entries[entry].relative, relative) == 0) {
				primary = &g_entries[entry];
			}
		}
		memcpy(relative + strlen(relative), ".recovery", 10);
		for (uint32_t entry = 0; entry < g_count; entry++) {
			if (strcmp(g_entries[entry].relative, relative) == 0) {
				recovery = &g_entries[entry];
			}
		}
		if (primary == 0 || recovery == 0 || recovery->kind != UPDATE_FILE || primary->new_size != recovery->new_size ||
			memcmp(primary->new_hash, recovery->new_hash, 32) != 0) {
			return false;
		}
	}
	return true;
}

static bool
update_save_plan(void)
{
	char path[256];
	update_plan_header_t header = {.magic = UPDATE_PLAN_MAGIC, .version = 1, .count = g_count};
	sha256(g_entries, g_count * sizeof(g_entries[0]), header.entries_hash);
	memcpy(header.manifest_hash, g_request.manifest_hash, 32);
	memcpy(header.build, g_request.build, sizeof(header.build));
	if (!update_path(path, g_transaction, "Entries") ||
		!update_write(path, g_entries, g_count * sizeof(g_entries[0])) ||
		!update_path(path, g_transaction, "Journal") || !update_write(path, &header, sizeof(header))) {
		return false;
	}
	return vfs_sync_all() == VFS_STATUS_OK;
}

static bool
update_load_plan(void)
{
	char path[256];
	uint32_t size;
	update_plan_header_t header;
	if (!update_path(path, g_transaction, "Journal") || !update_read_file(path, &header, sizeof(header), &size) ||
		size != sizeof(header) || header.magic != UPDATE_PLAN_MAGIC || header.version != 1 || header.count == 0 ||
		header.count > UPDATE_ENTRY_MAX || memcmp(header.build, g_request.build, sizeof(header.build)) != 0 ||
		memcmp(header.manifest_hash, g_request.manifest_hash, 32) != 0 ||
		!update_path(path, g_transaction, "Entries") || !update_read_file(path, g_entries, sizeof(g_entries), &size) ||
		size != header.count * sizeof(g_entries[0])) {
		return false;
	}
	uint8_t digest[32];
	sha256(g_entries, size, digest);
	if (memcmp(digest, header.entries_hash, 32) != 0) {
		return false;
	}
	g_count = header.count;
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		if (!update_relative(entry->relative) || entry->old_present > 1 ||
			(entry->kind != UPDATE_FILE && entry->kind != UPDATE_DIRECTORY)) {
			return false;
		}
		for (uint32_t previous = 0; previous < index; previous++) {
			if (strcmp(entry->relative, g_entries[previous].relative) == 0) {
				return false;
			}
		}
	}
	return true;
}

static bool
update_space(void)
{
	vfs_space_info_t system, recovery;
	if (vfs_space_info("/disk", &system) != VFS_STATUS_OK ||
		vfs_space_info(NSW_RECOVERY_ROOT, &recovery) != VFS_STATUS_OK || system.read_only || recovery.read_only ||
		system.block_size == 0 || recovery.block_size == 0) {
		return false;
	}
	/* Allow metadata/extent overhead as well as the journal and file data. */
	uint64_t backup_bytes = sizeof(g_entries) + 1048576;
	uint64_t growth_bytes = 1048576;
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		backup_bytes += 2ULL * recovery.block_size;
		growth_bytes += 2ULL * system.block_size;
		if (entry->kind == UPDATE_FILE) {
			backup_bytes +=
				((uint64_t)entry->old_size + recovery.block_size - 1) / recovery.block_size * recovery.block_size;
			uint64_t old_blocks = ((uint64_t)entry->old_size + system.block_size - 1) / system.block_size;
			uint64_t new_blocks = ((uint64_t)entry->new_size + system.block_size - 1) / system.block_size;
			if (new_blocks > old_blocks) {
				growth_bytes += (new_blocks - old_blocks) * system.block_size;
			}
		}
	}
	if (backup_bytes > recovery.free_bytes || growth_bytes > system.free_bytes) {
		kputln("patchd: insufficient space for update files and rollback backups");
		return false;
	}
	return true;
}

static bool
update_backup(void)
{
	char path[256], source[256];
	if (!update_path(path, g_transaction, "Backup") || !update_mkdir(path)) {
		return false;
	}
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		if (entry->kind != UPDATE_FILE || !entry->old_present) {
			continue;
		}
		kprintf("patchd: backing up /disk/%s\n", entry->relative);
		if (!update_path(source, "/disk", entry->relative) || !update_backup_path(path, index) ||
			!update_copy(source, path, entry->old_hash, entry->old_size, false)) {
			return false;
		}
	}
	return vfs_sync_all() == VFS_STATUS_OK && update_save_plan();
}

static bool
update_apply(void)
{
	char pending[256];
	uint32_t pending_size;
	if (!update_read_file(NSW_INSTALL_PENDING, pending, sizeof(pending), &pending_size) ||
		!update_write(NSW_ACTIVE_UPDATE, pending, pending_size) || vfs_sync_all() != VFS_STATUS_OK ||
		!update_state("applying\n")) {
		return false;
	}
	uint32_t applied = 0;
	uint32_t fail_after = boot_arg_uint32("update.test.fail_after", UINT32_MAX);
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		char source[256], target[256];
		if (!update_path(source, UPDATE_SOURCE, entry->relative) || !update_path(target, "/disk", entry->relative)) {
			return false;
		}
		if (entry->kind == UPDATE_DIRECTORY) {
			kprintf("patchd: applying patches to %s/\n", target);
			if (!update_mkdir(target)) {
				return false;
			}
			continue;
		}
		if (update_matches(target, entry->new_hash, entry->new_size)) {
			continue;
		}
		if (applied == fail_after) {
			kputln("patchd: injected installation failure");
			return false;
		}
		kprintf("patchd: applying patches to %s\n", target);
		if (!update_copy(source, target, entry->new_hash, entry->new_size, true)) {
			return false;
		}
		applied++;
	}
	return vfs_sync_all() == VFS_STATUS_OK && update_state("trial\n");
}

static bool
update_rollback(void)
{
	kputln("patchd: restoring the previous system from the update journal");
	if (!update_load_plan()) {
		return false;
	}
	/* Validate every backup before touching any installed path. */
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		char path[256];
		if (entry->kind == UPDATE_FILE && entry->old_present &&
			(!update_backup_path(path, index) || !update_matches(path, entry->old_hash, entry->old_size))) {
			return false;
		}
	}
	if (!update_state("rolling-back\n")) {
		return false;
	}
	for (uint32_t index = 0; index < g_count; index++) {
		update_entry_t *entry = &g_entries[index];
		if (entry->kind != UPDATE_FILE) {
			continue;
		}
		char source[256], target[256];
		if (!update_path(target, "/disk", entry->relative)) {
			return false;
		}
		if (entry->old_present) {
			if (update_matches(target, entry->old_hash, entry->old_size)) {
				continue;
			}
			kprintf("patchd: rolling back %s\n", target);
			if (!update_backup_path(source, index) ||
				!update_copy(source, target, entry->old_hash, entry->old_size, true)) {
				return false;
			}
		} else {
			kprintf("patchd: removing new update file %s\n", target);
			vfs_status_t status = vfs_unlink(target);
			if (status != VFS_STATUS_OK && status != VFS_STATUS_NOT_FOUND) {
				return false;
			}
		}
	}
	for (uint32_t index = g_count; index > 0; index--) {
		update_entry_t *entry = &g_entries[index - 1];
		if (entry->kind == UPDATE_DIRECTORY && !entry->old_present) {
			char path[256];
			if (!update_path(path, "/disk", entry->relative)) {
				return false;
			}
			vfs_status_t status = vfs_rmdir(path);
			if (status != VFS_STATUS_OK && status != VFS_STATUS_NOT_FOUND) {
				return false;
			}
		}
	}
	return vfs_sync_all() == VFS_STATUS_OK && update_state("rolled-back\n");
}

static void
update_clear_pending(void)
{
	(void)vfs_unlink(NSW_INSTALL_READY);
	(void)vfs_unlink(NSW_INSTALL_PENDING);
	(void)vfs_unlink(NSW_ACTIVE_UPDATE);
	(void)vfs_sync_all();
}

static bool
update_validate_images(bool verify_system)
{
	char directory[256], path[256];
	uint32_t size;
	uint8_t digest[32], expected[32];
	if (!update_path(directory, NSW_PREPARED_ROOT, g_request.build) || !update_path(path, directory, "Ready") ||
		!update_text(path, "verified\n") || !update_path(path, directory, "BuildManifest.plist") ||
		!update_read_file(path, g_manifest_data, sizeof(g_manifest_data), &size)) {
		return false;
	}
	sha256(g_manifest_data, size, digest);
	if (memcmp(digest, g_request.manifest_hash, 32) != 0 || !nsw_manifest_parse(g_manifest_data, size, &g_manifest) ||
		strcmp(g_manifest.build, g_request.build) != 0) {
		return false;
	}
	const char *images[] = {g_manifest.system, g_manifest.recovery};
	const char *hashes[] = {g_manifest.system_hash, g_manifest.recovery_hash};
	for (uint32_t index = verify_system ? 0 : 1; index < 2; index++) {
		if (!nsw_hash_decode(hashes[index], expected) || !update_path(path, directory, images[index]) ||
			!update_hash(path, digest, &size) || memcmp(digest, expected, 32) != 0) {
			return false;
		}
	}
	return true;
}

static bool
update_mount_image(block_file_image_t *image, const char *filename, const char *mountpoint, const char *name)
{
	char directory[256], path[256];
	if (!update_path(directory, NSW_PREPARED_ROOT, g_request.build) || !update_path(path, directory, filename) ||
		!update_mkdir(mountpoint) || !block_file_image_open(image, path, name)) {
		return false;
	}
	vfs_status_t status = vfs_mount("ext4", &image->device, mountpoint);
	if (status != VFS_STATUS_OK) {
		kprintf("patchd: cannot mount %s: %s\n", mountpoint, vfs_status_name(status));
		block_file_image_close(image);
		return false;
	}
	return true;
}

static void
update_close_source(void)
{
	if (g_source_image.vnode != 0 && vfs_unmount(UPDATE_SOURCE) == VFS_STATUS_OK) {
		block_file_image_close(&g_source_image);
	}
}

static bool
update_open_recovery(void)
{
	if (g_recovery_image.vnode != 0) {
		return true;
	}
	if (!update_mount_image(&g_recovery_image, g_manifest.recovery, UPDATE_RECOVERY, "nsw-recovery")) {
		return false;
	}
	if (!update_elf(UPDATE_RECOVERY "/System/Recovery/triageOS")) {
		if (vfs_unmount(UPDATE_RECOVERY) == VFS_STATUS_OK) {
			block_file_image_close(&g_recovery_image);
		}
		return false;
	}
	return true;
}

static void
update_close_recovery(void)
{
	if (g_recovery_image.vnode != 0 && vfs_unmount(UPDATE_RECOVERY) == VFS_STATUS_OK) {
		block_file_image_close(&g_recovery_image);
	}
}

static bool
update_recovery(void)
{
	update_close_source();
	if (!update_validate_images(false) || !update_open_recovery()) {
		if (boot_recovery_volume_available()) {
			boot_mode_force_recovery();
			kputln("patchd: starting the standalone recovery partition");
			return true;
		}
		kputln("patchd: rollback and recovery boot unavailable; refusing a partial system boot");
		return false;
	}
	boot_mode_force_recovery();
	kputln("patchd: starting the verified recovery environment");
	return true;
}

bool
boot_update_is_active(void)
{
	return g_trial || g_recovery_image.vnode != 0;
}

const char *
boot_update_recovery_program(void)
{
	return g_recovery_image.vnode != 0 ? UPDATE_RECOVERY "/System/Recovery/triageOS" : boot_recovery_volume_program();
}

bool
boot_update_install(void)
{
	bool ready = update_text(NSW_INSTALL_READY, "queued\n");
	const char *request_path = NSW_INSTALL_PENDING;
	if (!ready) {
		vnode_t pending;
		vfs_status_t status = vfs_lookup(NSW_INSTALL_PENDING, &pending);
		if (status == VFS_STATUS_NOT_FOUND) {
			request_path = NSW_ACTIVE_UPDATE;
			status = vfs_lookup(request_path, &pending);
			if (status == VFS_STATUS_NOT_FOUND) {
				return true;
			}
		}
		if (status != VFS_STATUS_OK) {
			return false;
		}
		vnode_rele(pending);
	}
	char pending[256], state_path[256];
	uint32_t size;
	memset(&g_request, 0, sizeof(g_request));
	if (!update_read_file(request_path, pending, sizeof(pending), &size) ||
		!nsw_request_parse(pending, size, &g_request) ||
		!update_path(g_transaction, NSW_TRANSACTIONS_ROOT, g_request.transaction) ||
		!update_path(state_path, g_transaction, "State")) {
		kputln("patchd: malformed pending installation; refusing an unverifiable system boot");
		return false;
	}
	if (update_text(state_path, "committed\n") || update_text(state_path, "rolled-back\n")) {
		update_clear_pending();
		return true;
	}
	vnode_t journal;
	char path[256];
	if (!update_path(path, g_transaction, "Journal")) {
		return false;
	}
	vfs_status_t journal_status = vfs_lookup(path, &journal);
	if (journal_status != VFS_STATUS_OK && journal_status != VFS_STATUS_NOT_FOUND) {
		kputln("patchd: unable to inspect the update journal; requiring verified recovery");
		return update_recovery();
	}
	if (journal_status == VFS_STATUS_OK) {
		vnode_rele(journal);
		/* Any journalled, unconfirmed transaction is restored before userspace. */
		if (update_rollback()) {
			update_clear_pending();
			kputln("patchd: interrupted or unconfirmed update rolled back");
			return true;
		}
		return update_recovery();
	}
	if (!ready) {
		if (strcmp(request_path, NSW_ACTIVE_UPDATE) == 0) {
			kputln("patchd: active update journal is unavailable; requiring recovery");
			return update_recovery();
		}
		update_clear_pending();
		kputln("patchd: incomplete installation queue discarded; system files were not changed");
		return true;
	}
	if (boot_mode_is_triage_os()) {
		return true;
	}
	kprintf("patchd: installing prepared build %s before userspace starts\n", g_request.build);
	if (!update_validate_images(true) || !update_open_recovery() ||
		!update_mount_image(&g_source_image, g_manifest.system, UPDATE_SOURCE, "nsw-system") || !update_plan() ||
		!update_space()) {
		kputln("patchd: update preflight failed; system files were not changed");
		update_close_source();
		update_close_recovery();
		update_clear_pending();
		return true;
	}
	if (!update_backup()) {
		kputln("patchd: backup failed; system files were not changed");
		update_close_source();
		update_close_recovery();
		update_clear_pending();
		return true;
	}
	if (!update_apply()) {
		update_close_source();
		if (update_rollback()) {
			update_close_recovery();
			update_clear_pending();
			kputln("patchd: failed installation rolled back");
			return true;
		}
		return update_recovery();
	}
	update_close_source();
	update_close_recovery();
	g_trial = true;
	kputln("patchd: patches applied; waiting for the updated system to confirm its boot");
	return true;
}

static void
update_watch(void *argument)
{
	(void)argument;
	uint64_t deadline = timer_get_microseconds() + 90000000;
	char path[256];
	if (!update_path(path, g_transaction, "State")) {
		return;
	}
	while (timer_get_microseconds() < deadline) {
		if (update_text(path, "committed\n")) {
			kputln("patchd: updated boot health confirmed");
			return;
		}
		(void)sched_sleep_us(1000000);
	}
	kputln("patchd: updated boot was not confirmed; restarting to roll back");
	machine_system_reset();
}

void
boot_update_watch_start(void)
{
	if (!g_trial) {
		return;
	}
	thread_t thread;
	if (!kernel_thread_create(proc_task(proc_kernel()), update_watch, 0, &thread) || !sched_thread_start(thread)) {
		kputln("patchd: unable to start update boot watchdog; restarting to roll back");
		machine_system_reset();
	}
}
