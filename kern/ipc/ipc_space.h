/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_space.h
 *
 * Per-process NXPC name table.
 */

#ifndef NXU_KERN_IPC_SPACE_H
#define NXU_KERN_IPC_SPACE_H

#include <kern/ipc/ipc_types.h>

#include <stdbool.h>
#include <stdint.h>

#define IPC_SPACE_MAX 32U
#define IPC_SPACE_NAME_INVALID 0U

typedef enum {
	IPC_SPACE_ENTRY_FREE = 0,
	IPC_SPACE_ENTRY_PORT
} ipc_space_entry_type_t;

typedef struct {
	ipc_space_entry_type_t type;
	ipc_port_t port;
} ipc_space_entry_t;

typedef struct ipc_space *ipc_space_t;

/*
 * struct ipc_space
 *
 * Per-process NXPC name table, mirroring vfs/file.h's struct filedesc: names
 * are 1-based indexes into is_entries (name 0/IPC_SPACE_NAME_INVALID is
 * never issued, so a zeroed-out name field always reads as "nothing here").
 * A space owns one lifetime reference to every port it holds a name for,
 * released when the name is removed or the owning process exits.
 */
struct ipc_space {
	ipc_space_entry_t is_entries[IPC_SPACE_MAX];
	uint32_t is_open_count;
	uint32_t is_freename;
	volatile uint32_t is_lock;
};

void ipc_space_init(ipc_space_t space);
void ipc_space_close_all(ipc_space_t space);

/*
 * fork: child (freshly initialised, empty) receives a name for every port
 * parent holds, at the same name, each with its own reference. Ports are not
 * split into send and receive rights here, so parent and child both hold the
 * full capability -- like two processes sharing an inherited pipe. On
 * failure child is left empty.
 */
bool ipc_space_fork(ipc_space_t child, ipc_space_t parent);

/*
 * port must already be a reference the caller owns and is handing off to
 * the space (mirroring file_alloc's initial reference moving straight into
 * filedesc_install) -- ipc_space_insert_port does not take its own.
 */
ipc_return_t ipc_space_insert_port(ipc_space_t space, ipc_port_t port, uint32_t *name);

/* Returns a new reference the caller must eventually release. */
ipc_return_t ipc_space_lookup_port(ipc_space_t space, uint32_t name, ipc_port_t *result);

/* Removes the name and hands the space's own reference to the caller. */
ipc_return_t ipc_space_remove(ipc_space_t space, uint32_t name, ipc_port_t *result);

#endif
