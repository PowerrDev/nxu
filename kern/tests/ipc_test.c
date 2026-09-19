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
#include <kern/ipc/ipc_space.h>
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
	result = ipc_kmsg_alloc(g_ipc_test_payload, sizeof(g_ipc_test_payload), IPC_PORT_NULL, &kmsg);
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
	result = ipc_kmsg_alloc(&sentinel, IPC_KMSG_MAX_INLINE_SIZE + 1U, IPC_PORT_NULL, &received);
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

/*
 * Routine:     ipc_space_self_test
 * Purpose:
 *              Verify the per-process name table in isolation: install/
 *              lookup/remove refcounting, a message carrying a transferred
 *              port moving that port from one space into a different one on
 *              receive (the exact shape ipc_send/ipc_receive will expose to
 *              userspace), and the boundary conditions on invalid names.
 */
bool
ipc_space_self_test(void)
{
	if (!ipc_initialized()) return false;

	struct ipc_space space_a;
	struct ipc_space space_b;
	ipc_space_init(&space_a);
	ipc_space_init(&space_b);

	ipc_port_t service_port = IPC_PORT_NULL;
	ipc_port_t reply_port = IPC_PORT_NULL;
	bool passed = false;

	if (ipc_port_alloc(&service_port) != IPC_SUCCESS) goto cleanup;
	if (ipc_port_alloc(&reply_port) != IPC_SUCCESS) goto cleanup;

	uint32_t service_name;
	if (ipc_space_insert_port(&space_a, service_port, &service_name) != IPC_SUCCESS) goto cleanup;
	service_port = IPC_PORT_NULL;

	uint32_t reply_name;
	if (ipc_space_insert_port(&space_b, reply_port, &reply_name) != IPC_SUCCESS) goto cleanup;
	reply_port = IPC_PORT_NULL;

	/* B looks up its own reply port and A's service port, exactly like a
	 * real sender preparing an ipc_send with a transfer. */
	ipc_port_t reply_ref = IPC_PORT_NULL;
	ipc_port_t service_ref = IPC_PORT_NULL;
	if (ipc_space_lookup_port(&space_b, reply_name, &reply_ref) != IPC_SUCCESS) goto cleanup;
	if (ipc_space_lookup_port(&space_a, service_name, &service_ref) != IPC_SUCCESS) {
		ipc_port_release(reply_ref);
		goto cleanup;
	}

	static const char payload[] = "hello service";
	ipc_kmsg_t kmsg = IPC_KMSG_NULL;
	if (ipc_kmsg_alloc(payload, sizeof(payload), reply_ref, &kmsg) != IPC_SUCCESS) {
		ipc_port_release(reply_ref);
		ipc_port_release(service_ref);
		goto cleanup;
	}
	/* reply_ref's reference now belongs to kmsg. */

	ipc_return_t enqueue_result = ipc_port_enqueue(service_ref, kmsg);
	ipc_port_release(service_ref);
	if (enqueue_result != IPC_SUCCESS) {
		ipc_kmsg_free(kmsg);
		goto cleanup;
	}

	/* A receives on the service port it actually owns and moves the
	 * transferred reply port into its own space -- the receive half of
	 * what ipc_receive will do. */
	ipc_port_t owned_service_port = IPC_PORT_NULL;
	if (ipc_space_lookup_port(&space_a, service_name, &owned_service_port) != IPC_SUCCESS) goto cleanup;

	ipc_kmsg_t received = IPC_KMSG_NULL;
	ipc_return_t dequeue_result = ipc_port_dequeue(owned_service_port, &received);
	ipc_port_release(owned_service_port);
	if (dequeue_result != IPC_SUCCESS || received == IPC_KMSG_NULL) goto cleanup;

	if (received->ikm_xfer_type != IPC_KMSG_XFER_PORT || received->ikm_xfer_port == IPC_PORT_NULL) {
		ipc_kmsg_free(received);
		goto cleanup;
	}

	uint32_t reply_name_in_a;
	if (ipc_space_insert_port(&space_a, received->ikm_xfer_port, &reply_name_in_a) != IPC_SUCCESS) {
		ipc_kmsg_free(received);
		goto cleanup;
	}
	received->ikm_xfer_type = IPC_KMSG_XFER_NONE;
	received->ikm_xfer_port = IPC_PORT_NULL;
	ipc_kmsg_free(received);

	/* A can now reach B's reply port under its own name. */
	ipc_port_t confirm = IPC_PORT_NULL;
	if (ipc_space_lookup_port(&space_a, reply_name_in_a, &confirm) != IPC_SUCCESS) goto cleanup;
	ipc_port_release(confirm);

	/* Boundary conditions: an out-of-range or never-issued name is rejected. */
	ipc_port_t bogus = IPC_PORT_NULL;
	if (ipc_space_lookup_port(&space_a, IPC_SPACE_MAX + 1U, &bogus) != IPC_NAME_INVALID) goto cleanup;
	if (ipc_space_lookup_port(&space_a, IPC_SPACE_NAME_INVALID, &bogus) != IPC_NAME_INVALID) goto cleanup;

	passed = true;

cleanup:
	/*
	 * Everything this test ever created is, by this point, either still
	 * sitting in one of the two loose locals below (an early failure) or
	 * has been installed into space_a/space_b -- closing both spaces plus
	 * releasing any not-yet-installed local covers every path.
	 */
	ipc_space_close_all(&space_a);
	ipc_space_close_all(&space_b);
	if (service_port != IPC_PORT_NULL) ipc_port_release(service_port);
	if (reply_port != IPC_PORT_NULL) ipc_port_release(reply_port);

	if (!passed) return false;
	if (ipc_port_outstanding_count() != 0ULL) return false;
	if (ipc_kmsg_outstanding_count() != 0ULL) return false;

	kprintf(NXPC_LOG_PREFIX "ipc_space self-test passed (port transferred across spaces)\n");
	return true;
}
