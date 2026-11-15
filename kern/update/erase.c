/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/update/erase.c
 *
 * The boot side of "Erase All Content and Settings". patchd queues an erase
 * by writing NSW_ERASE_QUEUED on the recovery volume and restarting. On the
 * next boot this hook sees the marker and, instead of bootd, starts the
 * recovery volume's own patchd as PID 1 in erase mode, so the program doing
 * the wipe never runs from the volume being wiped. patchd removes every file
 * on /Volumes/mainOS, reinstalls the factory payload, verifies every byte it
 * wrote, logs each step, and restarts. An erase that failed after the wipe
 * began leaves NSW_ERASE_FAILED, which sends every later boot to recovery
 * until it is retried or cleared there.
 */
#include <kern/update/erase.h>
#include <kern/update/update.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/recovery_volume.h>
#include <kern/console/console.h>
#include <kern/loader/elf_format.h>
#include <libk/string.h>
#include <vfs/vfs.h>

static bool g_erase_queued;
static bool g_erase_running;

static bool
erase_read(const char *path, char *bytes, uint32_t capacity, uint32_t *size)
{
	/* Read a small regular file whole, when it fits in the buffer. */
	vnode_t vnode;

	*size = 0;
	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	bool success = vnode->v_type == VNODE_TYPE_REGULAR && vnode->v_size <= capacity;
	uint32_t length = success ? (uint32_t)vnode->v_size : 0;
	uint32_t complete = 0;

	while (success && complete < length) {
		uint64_t read = 0;
		success = vnode_read(vnode, complete, bytes + complete, length - complete, &read) == VFS_STATUS_OK &&
				  read > 0 && read <= length - complete;
		if (success) complete += (uint32_t)read;
	}

	vnode_rele(vnode);
	if (success) *size = length;
	return success;
}

static bool
erase_exists(const char *path)
{
	vnode_t vnode;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	vnode_rele(vnode);
	return true;
}

static bool
erase_program_valid(const char *path)
{
	/* The eraser must be a loadable executable before the boot commits to it. */
	uint8_t raw[LOADER_ELF_HEADER_SIZE];
	vnode_t vnode;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	uint64_t size = 0;
	bool success = vnode->v_type == VNODE_TYPE_REGULAR &&
				   vnode_read(vnode, 0, raw, sizeof(raw), &size) == VFS_STATUS_OK && size == sizeof(raw);
	vnode_rele(vnode);

	if (!success) return false;

	loader_disk_header_t disk;
	loader_header_t header;

	memcpy(&disk, raw, sizeof(disk));
#if LOADER_ELF_WIDEN
	loader_header_widen(&disk, &header);
#else
	header = disk;
#endif
	return loader_header_valid(&header);
}

static void
erase_cancel(const char *reason)
{
	/* The system volume is still intact: drop the queue and boot it normally. */
	kprintf("erase cancelled before any file was changed: %s\n", reason);

	vfs_status_t status = vfs_unlink(NSW_ERASE_QUEUED);

	if (status != VFS_STATUS_OK && status != VFS_STATUS_NOT_FOUND) {
		kprintf("could not remove %s: %s\n", NSW_ERASE_QUEUED, vfs_status_name(status));
	}

	(void)vfs_sync_all();
}

void
boot_erase_init(void)
{
	kprintf("initializing the erase-all-content-and-settings boot hook\n");
	g_erase_queued = false;
	g_erase_running = false;

	if (!boot_recovery_volume_available()) {
		kprintf("no recovery volume, so no erase can be queued\n");
		kprintf("initialized the erase-all-content-and-settings boot hook successfully\n");
		return;
	}

	char marker[64];
	uint32_t size = 0;
	bool queued = erase_exists(NSW_ERASE_QUEUED);

	if (queued) {
		if (!erase_read(NSW_ERASE_QUEUED, marker, sizeof(marker), &size) || size != strlen(NSW_ERASE_MARKER) ||
			memcmp(marker, NSW_ERASE_MARKER, size) != 0) {
			erase_cancel("the queue marker is malformed");
		} else if (boot_update_is_active()) {
			/* An update trial or recovery source owns this boot; the erase waits for a plain boot. */
			kprintf("a software update is being resolved, so the queued erase waits for the next boot\n");
		} else if (!erase_program_valid(NSW_ERASE_PROGRAM)) {
			erase_cancel("the recovery volume has no valid patchd to run the erase");
		} else if (!erase_exists(NSW_FACTORY_MANIFEST) || !erase_exists(NSW_FACTORY_MANIFEST_HASH)) {
			erase_cancel("the recovery volume has no factory payload to reinstall");
		} else {
			g_erase_queued = true;
			kprintf("erase all content and settings is queued (%s)\n", NSW_ERASE_QUEUED);
			if (boot_mode_is_triage_os()) {
				/* Recovery was asked for (Shift+R, recovery=1): it wins, and the erase waits. */
				kprintf("recovery was requested for this boot, so the queued erase waits for the next normal boot\n");
			} else {
				kprintf("%s will run from the recovery volume as PID 1 instead of bootd\n", NSW_ERASE_PROGRAM);
				kprintf("every removed and reinstalled file is logged here and in %s\n", NSW_ERASE_LOG);
			}
		}
	} else if (erase_exists(NSW_ERASE_FAILED)) {
		char reason[128];

		if (erase_read(NSW_ERASE_FAILED, reason, sizeof(reason) - 1, &size)) {
			reason[size] = 0;
			if (size > 0 && reason[size - 1] == '\n') reason[size - 1] = 0;
		} else {
			memcpy(reason, "unreadable", 11);
		}

		kprintf("a previous erase did not finish (%s); the system volume may be incomplete\n", reason);
		kprintf("starting recovery; the full log is %s\n", NSW_ERASE_LOG);
		kprintf("to retry, run 'erase confirm' in the recovery shell; to boot mainOS anyway, remove %s\n", NSW_ERASE_FAILED);
		boot_mode_force_recovery();
	}

	kprintf("initialized the erase-all-content-and-settings boot hook successfully (%s)\n",
			g_erase_queued ? "erase queued" : "nothing to erase");
}

bool
boot_erase_pending(void)
{
	/* Shift+R or recovery=1 still wins: the user asked for recovery, and the erase stays queued. */
	return g_erase_queued && !boot_mode_is_triage_os();
}

loader_status_t
boot_erase_spawn(proc_t *result)
{
	static loader_exec_args_t arguments;
	static const char name[] = "patchd";
	uint32_t name_size = sizeof(name);
	uint32_t argument_size = sizeof(NSW_ERASE_ARGUMENT);

	memset(&arguments, 0, sizeof(arguments));
	arguments.argc = 2;
	arguments.length = name_size + argument_size;
	memcpy(arguments.strings, name, name_size);
	memcpy(arguments.strings + name_size, NSW_ERASE_ARGUMENT, argument_size);

	loader_spawn_request_t request;

	memset(&request, 0, sizeof(request));
	request.name = name;
	request.caps = LOADER_CAPS_DEFAULT;
	request.args = &arguments;
	request.group.mode = PROC_GROUP_INHERIT;

	/* Set before the process can run: its first system calls already remove protected files. */
	g_erase_running = true;
	kprintf("loading %s %s as PID 1\n", NSW_ERASE_PROGRAM, NSW_ERASE_ARGUMENT);

	loader_status_t status = loader_spawn_request(proc_kernel(), NSW_ERASE_PROGRAM, &request, result);

	if (status != LOADER_STATUS_OK || *result == 0 || (*result)->p_ident.pid != 1U) {
		g_erase_running = false;
		kprintf("the eraser did not start as PID 1: %s\n", loader_status_name(status));
		if (status == LOADER_STATUS_OK) status = LOADER_STATUS_PROCESS_ERROR;
		/* Nothing was removed yet: give up the erase so the caller can start bootd as usual. */
		erase_cancel("patchd could not be started from the recovery volume");
		g_erase_queued = false;
	}

	return status;
}

bool
boot_erase_running(void)
{
	return g_erase_running;
}
