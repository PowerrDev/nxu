/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_space.c
 *
 * Per-process NXPC name table: small-integer names to ipc_port_t objects,
 * mirroring vfs/file.c's filedesc_* lifecycle and locking exactly (a
 * per-space atomic-exchange spinlock, not ipc_port.c's single-CPU IRQ-mask
 * lock -- ipc_space is a per-process resource table like filedesc, not a
 * kernel-global object like ipc_port).
 */

#include <kern/ipc/ipc_space.h>

#include <kern/ipc/ipc_port.h>

#include <mach/machine/cpu.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void
ipc_space_lock(ipc_space_t space)
{
	while (__atomic_exchange_n(&space->is_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void
ipc_space_unlock(ipc_space_t space)
{
	__atomic_store_n(&space->is_lock, 0U, __ATOMIC_RELEASE);
}

void
ipc_space_init(ipc_space_t space)
{
	if (space == 0) return;

	memset(space, 0, sizeof(*space));
	space->is_freename = 1U;
}

ipc_return_t
ipc_space_insert_port(ipc_space_t space, ipc_port_t port, uint32_t *name)
{
	if (name != 0) *name = IPC_SPACE_NAME_INVALID;
	if (space == 0 || port == IPC_PORT_NULL || name == 0 || !ipc_port_active(port)) return IPC_INVALID_ARGUMENT;

	ipc_space_lock(space);

	uint32_t start = space->is_freename;
	if (start < 1U || start > IPC_SPACE_MAX) start = 1U;

	for (uint32_t slot = start; slot <= IPC_SPACE_MAX; slot++) {
		if (space->is_entries[slot - 1U].type != IPC_SPACE_ENTRY_FREE) continue;

		space->is_entries[slot - 1U].type = IPC_SPACE_ENTRY_PORT;
		space->is_entries[slot - 1U].port = port;
		space->is_open_count++;

		uint32_t next = slot + 1U;
		while (next <= IPC_SPACE_MAX && space->is_entries[next - 1U].type != IPC_SPACE_ENTRY_FREE) next++;
		space->is_freename = next;

		*name = slot;
		ipc_space_unlock(space);
		return IPC_SUCCESS;
	}

	ipc_space_unlock(space);
	return IPC_SPACE_FULL;
}

ipc_return_t
ipc_space_lookup_port(ipc_space_t space, uint32_t name, ipc_port_t *result)
{
	if (result != 0) *result = IPC_PORT_NULL;
	if (space == 0 || result == 0 || name < 1U || name > IPC_SPACE_MAX) return IPC_NAME_INVALID;

	ipc_space_lock(space);

	ipc_space_entry_t *entry = &space->is_entries[name - 1U];
	if (entry->type != IPC_SPACE_ENTRY_PORT) {
		ipc_space_unlock(space);
		return IPC_NAME_INVALID;
	}

	if (!ipc_port_reference(entry->port)) {
		ipc_space_unlock(space);
		return IPC_PORT_INACTIVE;
	}

	*result = entry->port;
	ipc_space_unlock(space);
	return IPC_SUCCESS;
}

ipc_return_t
ipc_space_remove(ipc_space_t space, uint32_t name, ipc_port_t *result)
{
	if (result != 0) *result = IPC_PORT_NULL;
	if (space == 0 || result == 0 || name < 1U || name > IPC_SPACE_MAX) return IPC_NAME_INVALID;

	ipc_space_lock(space);

	ipc_space_entry_t *entry = &space->is_entries[name - 1U];
	if (entry->type != IPC_SPACE_ENTRY_PORT) {
		ipc_space_unlock(space);
		return IPC_NAME_INVALID;
	}

	ipc_port_t port = entry->port;
	entry->type = IPC_SPACE_ENTRY_FREE;
	entry->port = IPC_PORT_NULL;
	if (space->is_open_count != 0U) space->is_open_count--;
	if (name < space->is_freename) space->is_freename = name;

	ipc_space_unlock(space);

	*result = port;
	return IPC_SUCCESS;
}

void
ipc_space_close_all(ipc_space_t space)
{
	if (space == 0) return;

	for (uint32_t name = 1U; name <= IPC_SPACE_MAX; name++) {
		ipc_port_t port;
		if (ipc_space_remove(space, name, &port) == IPC_SUCCESS) ipc_port_release(port);
	}
}
