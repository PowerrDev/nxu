/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/ipc_test.c
 *
 * Kernel-only validation for NXPC message allocation and port FIFO semantics.
 */

#include <kern/tests/ipc_test.h>

#include <kern/console/console.h>
#include <kern/ipc/ipc_init.h>
#include <kern/ipc/ipc_kmsg.h>
#include <kern/ipc/ipc_port.h>
#include <kern/ipc/ipc_types.h>

#include <stddef.h>
#include <stdint.h>

static const char g_ipc_test_payload[] = "NXPC kernel transport";

/*
 * Routine:     ipc_self_test
 * Purpose:
 *              Verify one complete kernel message round trip and the boundary
 *              conditions that would otherwise corrupt a future userspace ABI.
 */
bool
ipc_self_test(void)
{
	if (!ipc_initialized()) return false;

	ipc_port_t port = IPC_PORT_NULL;
	ipc_return_t result = ipc_port_alloc(&port);
	if (result != IPC_SUCCESS || port == IPC_PORT_NULL) return false;

	ipc_object_id_t object_id = ipc_port_object_id(port);
	ipc_kmsg_t kmsg = IPC_KMSG_NULL;
	result = ipc_kmsg_alloc(g_ipc_test_payload, sizeof(g_ipc_test_payload), &kmsg);
	if (result != IPC_SUCCESS) goto fail_port;

	result = ipc_port_enqueue(port, kmsg);
	if (result != IPC_SUCCESS) goto fail_kmsg;
	kmsg = IPC_KMSG_NULL;

	ipc_kmsg_t received = IPC_KMSG_NULL;
	result = ipc_port_dequeue(port, &received);
	if (result != IPC_SUCCESS || received == IPC_KMSG_NULL) goto fail_port;
	if (received->ikm_size != sizeof(g_ipc_test_payload)) goto fail_received;

	for (size_t index = 0U; index < sizeof(g_ipc_test_payload); index++) {
		if (received->ikm_data[index] != (uint8_t)g_ipc_test_payload[index]) goto fail_received;
	}

	ipc_kmsg_free(received);
	received = IPC_KMSG_NULL;

	result = ipc_port_dequeue(port, &received);
	if (result != IPC_QUEUE_EMPTY || received != IPC_KMSG_NULL) goto fail_port;

	uint8_t sentinel = 0U;
	result = ipc_kmsg_alloc(&sentinel, IPC_KMSG_MAX_INLINE_SIZE + 1U, &received);
	if (result != IPC_MESSAGE_TOO_LARGE || received != IPC_KMSG_NULL) goto fail_port;

	if (!ipc_port_reference(port)) goto fail_port;
	ipc_port_release(port);
	if (!ipc_port_active(port)) goto fail_port;

	ipc_port_release(port);
	port = IPC_PORT_NULL;

	if (ipc_port_outstanding_count() != 0ULL) return false;
	if (ipc_kmsg_outstanding_count() != 0ULL) return false;
	if (ipc_kmsg_outstanding_bytes() != 0ULL) return false;

	kprintf(NXPC_LOG_PREFIX "self-test passed on port object %llu (%llu-byte payload)\n", object_id, (uint64_t)sizeof(g_ipc_test_payload));
	return true;

fail_received:
	ipc_kmsg_free(received);

fail_port:
	ipc_port_release(port);
	return false;

fail_kmsg:
	ipc_kmsg_free(kmsg);
	ipc_port_release(port);
	return false;
}
