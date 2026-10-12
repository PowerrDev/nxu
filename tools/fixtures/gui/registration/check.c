#include <frameworks/CoreFoundation.framework/sbin/bootd/registry.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>
#include <assert.h>

static nxu_ipc_peer_t live = {5, 17, 41, NXU_CAP_DISPLAY, 0};
static nxu_ipc_peer_t sender;
static nxu_bootstrap_request_t pending;
static uint32_t transferred;
static uint32_t released;
static bool queued;
static bool alive = true;

int64_t
nxu_ipc_port_allocate(void)
{
	return 1;
}

int64_t
nxu_ipc_register_bootstrap(uint32_t port)
{
	return port == 1 ? 0 : -1;
}

int64_t
nxu_ipc_port_deallocate(uint32_t port)
{
	released = port;
	return 0;
}

int64_t
nxu_process_identity(uint32_t pid, nxu_ipc_peer_t *identity)
{
	if (!alive || pid != live.pid) return -NXU_SYS_E_NOT_FOUND;
	*identity = live;
	return 0;
}

int64_t
nxu_ipc_send(uint32_t port, const void *buffer, uint64_t length, uint32_t transfer)
{
	(void)port;
	(void)buffer;
	(void)length;
	(void)transfer;
	return 0;
}

int64_t
nxu_ipc_receive_peer(uint32_t port, void *buffer, uint64_t length, uint32_t *transfer, uint32_t *type,
					 nxu_ipc_peer_t *peer)
{
	(void)port;
	assert(length == sizeof(pending));
	if (!queued) return -NXU_SYS_E_AGAIN;
	queued = false;
	memcpy(buffer, &pending, sizeof(pending));
	*transfer = transferred;
	*type = NXU_IPC_XFER_PORT;
	*peer = sender;
	return sizeof(pending);
}

static void
registration(bootd_registry_t *registry, nxu_ipc_peer_t peer, uint32_t port)
{
	pending = (nxu_bootstrap_request_t){.opcode = NXU_BOOTSTRAP_OP_REGISTER};
	memcpy(pending.label, WS_BOOTSTRAP_LABEL, sizeof(WS_BOOTSTRAP_LABEL));
	sender = peer;
	transferred = port;
	queued = true;
	released = 0;
	bootd_registry_poll(registry);
}

int
main(void)
{
	bootd_registry_t registry;
	assert(bootd_registry_init(&registry));
	registration(&registry, live, 10);
	assert(!registry.entry_count && released == 10);
	bootd_registry_bind_compositor(&registry, &live);
	registration(&registry, live, 11);
	assert(registry.entry_count == 1 && registry.entries[0].port_name == 11);

	/* Same PID, different lifetime: neither uniqueid nor idversion may be ignored. */
	nxu_ipc_peer_t stale = live;
	stale.uniqueid--;
	registration(&registry, stale, 12);
	assert(registry.entries[0].port_name == 11 && released == 12);
	stale = live;
	stale.idversion--;
	registration(&registry, stale, 13);
	assert(registry.entries[0].port_name == 11 && released == 13);
	stale = live;
	stale.pid++;
	registration(&registry, stale, 14);
	assert(registry.entries[0].port_name == 11 && released == 14);

	/* Messages queued before exit must not republish the dead endpoint. */
	alive = false;
	registration(&registry, live, 15);
	assert(registry.entries[0].port_name == 11 && released == 15);
	bootd_registry_bind_compositor(&registry, &(nxu_ipc_peer_t){0});
	assert(!registry.entry_count && released == 11);

	alive = true;
	live.uniqueid++;
	live.idversion++;
	bootd_registry_bind_compositor(&registry, &live);
	registration(&registry, stale, 16);
	assert(!registry.entry_count && released == 16);
	registration(&registry, live, 17);
	assert(registry.entry_count == 1 && registry.entries[0].port_name == 17);
	return 0;
}
