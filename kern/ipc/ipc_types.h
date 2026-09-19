/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_types.h
 *
 * Common types and limits for the NXPC kernel message transport.
 */

#ifndef NXU_KERN_IPC_TYPES_H
#define NXU_KERN_IPC_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NXPC begins with bounded inline messages. Out-of-line memory and port
 * descriptors belong to a later ABI revision and must not silently bypass
 * this limit.
 */
#define IPC_KMSG_MAX_INLINE_SIZE 4096U
#define IPC_PORT_QUEUE_LIMIT 64U

#define IPC_OBJECT_ID_NULL 0ULL
#define NXPC_LOG_PREFIX "[com.butterscotch.xpc]: "

typedef uint64_t ipc_object_id_t;

typedef enum {
	IPC_SUCCESS = 0,
	IPC_INVALID_ARGUMENT,
	IPC_NO_MEMORY,
	IPC_MESSAGE_TOO_LARGE,
	IPC_PORT_INACTIVE,
	IPC_QUEUE_EMPTY,
	IPC_QUEUE_FULL,
	IPC_BUFFER_TOO_SMALL,
	IPC_OVERFLOW,
	IPC_SPACE_FULL,
	IPC_NAME_INVALID
} ipc_return_t;

/*
 * A kernel message may carry at most one transferable name, moved into the
 * receiving task's ipc_space on delivery. IPC_KMSG_XFER_MEMORY is added
 * alongside vm_shm_region_t (see vm/vm_shm.h); only IPC_KMSG_XFER_PORT is
 * used until then.
 */
typedef enum {
	IPC_KMSG_XFER_NONE = 0,
	IPC_KMSG_XFER_PORT,
	IPC_KMSG_XFER_MEMORY
} ipc_kmsg_xfer_type_t;

struct ipc_kmsg;
struct ipc_port;

typedef struct ipc_kmsg *ipc_kmsg_t;
typedef struct ipc_port *ipc_port_t;

#define IPC_KMSG_NULL ((ipc_kmsg_t)0)
#define IPC_PORT_NULL ((ipc_port_t)0)

#endif
