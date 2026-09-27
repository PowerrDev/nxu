/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/CoreFoundation.framework/sbin/bootd/registry.c
 *
 * bootd's bootstrap registry: one well-known port, polled non-blockingly
 * from the same loop that drives job supervision. See
 * frameworks/include/nxu/bootstrap_protocol.h for the wire format and
 * frameworks/CoreFoundation.framework/lib/service/bootstrap_client.c for
 * the client side every other process uses.
 */

#include <frameworks/CoreFoundation.framework/sbin/bootd/registry.h>

#include <nxu/string.h>
#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/* At most this many registry messages are answered per poll. */
#define BOOTD_REGISTRY_BATCH 64U

static bool bootd_registry_poll_one(bootd_registry_t *registry);

static void
bootd_registry_fill_label(char destination[NXU_BOOTSTRAP_LABEL_MAX], const char *label)
{
	uint64_t length = nxu_strlen(label);
	if (length >= NXU_BOOTSTRAP_LABEL_MAX) length = NXU_BOOTSTRAP_LABEL_MAX - 1U;

	for (uint64_t index = 0ULL; index < length; index++) destination[index] = label[index];
	for (uint64_t index = length; index < NXU_BOOTSTRAP_LABEL_MAX; index++) destination[index] = '\0';
}

static void
bootd_registry_handle_register(bootd_registry_t *registry, const char *label, uint32_t xfer_name, uint32_t xfer_type)
{
	if (xfer_type != NXU_IPC_XFER_PORT || xfer_name == 0U) return;

	for (uint32_t index = 0U; index < registry->entry_count; index++) {
		if (nxu_streq(registry->entries[index].label, label)) {
			(void)nxu_ipc_port_deallocate(registry->entries[index].port_name);
			registry->entries[index].port_name = xfer_name;
			return;
		}
	}

	if (registry->entry_count >= BOOTD_REGISTRY_MAX_ENTRIES) {
		(void)nxu_ipc_port_deallocate(xfer_name);
		return;
	}

	bootd_registry_entry_t *entry = &registry->entries[registry->entry_count];
	bootd_registry_fill_label(entry->label, label);
	entry->port_name = xfer_name;
	registry->entry_count++;
}

static void
bootd_registry_handle_lookup(bootd_registry_t *registry, const char *label, uint32_t reply_local_name, uint32_t xfer_type)
{
	if (xfer_type != NXU_IPC_XFER_PORT || reply_local_name == 0U) return;

	uint32_t found_port_name = 0U;
	for (uint32_t index = 0U; index < registry->entry_count; index++) {
		if (nxu_streq(registry->entries[index].label, label)) {
			found_port_name = registry->entries[index].port_name;
			break;
		}
	}

	nxu_bootstrap_reply_t reply;
	reply.status = found_port_name != 0U ? (int32_t)NXU_BOOTSTRAP_STATUS_OK : (int32_t)NXU_BOOTSTRAP_STATUS_NOT_FOUND;

	/*
	 * found_port_name (0 when unmatched) doubles as the "no transfer"
	 * sentinel nxu_ipc_send expects -- see kern/syscall/syscall_defs.h's
	 * IPC_SPACE_NAME_INVALID convention.
	 */
	(void)nxu_ipc_send(reply_local_name, &reply, sizeof(reply), found_port_name);
	(void)nxu_ipc_port_deallocate(reply_local_name);
}

bool
bootd_registry_init(bootd_registry_t *registry)
{
	if (registry == 0) return false;
	*registry = (bootd_registry_t) { 0 };

	int64_t port_name = nxu_ipc_port_allocate();
	if (port_name <= 0) return false;

	if (nxu_ipc_register_bootstrap((uint32_t)port_name) != 0) {
		(void)nxu_ipc_port_deallocate((uint32_t)port_name);
		return false;
	}

	registry->port_name = (uint32_t)port_name;
	return true;
}

void
bootd_registry_poll(bootd_registry_t *registry)
{
	if (registry == 0 || registry->port_name == 0U) return;

	/*
	 * Everything queued, not one message: bootd sleeps between polls now, and
	 * one answer per poll let lookups of a service that had not registered yet
	 * (each answered NOT_FOUND and asked again) pile up ahead of the very
	 * registration they were waiting for. Bounded, so a flood cannot keep bootd
	 * from its jobs.
	 */
	for (uint32_t handled = 0U; handled < BOOTD_REGISTRY_BATCH; handled++) {
		if (!bootd_registry_poll_one(registry)) return;
	}
}

static bool
bootd_registry_poll_one(bootd_registry_t *registry)
{
	nxu_bootstrap_request_t request;
	uint32_t xfer_name = 0U;
	uint32_t xfer_type = 0U;

	int64_t received = nxu_ipc_receive(registry->port_name, &request, sizeof(request), &xfer_name, &xfer_type);
	if (received < 0) return false;
	if ((uint64_t)received != sizeof(request)) return true;

	request.label[NXU_BOOTSTRAP_LABEL_MAX - 1U] = '\0';

	switch ((nxu_bootstrap_op_t)request.opcode) {
	case NXU_BOOTSTRAP_OP_REGISTER:
		bootd_registry_handle_register(registry, request.label, xfer_name, xfer_type);
		break;
	case NXU_BOOTSTRAP_OP_LOOKUP:
		bootd_registry_handle_lookup(registry, request.label, xfer_name, xfer_type);
		break;
	default:
		break;
	}

	return true;
}
