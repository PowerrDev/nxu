/* Exercise the production ownership table against a synthetic reused PID. */
#define main windowserver_service_main
#include <frameworks/BootDaemons.framework/windowserver_service.c>
#undef main
#include <assert.h>

static nxu_ipc_peer_t live = {9, 12, 44, 0, 0};
static uint32_t destroyed;

int64_t
nxu_process_identity(uint32_t pid, nxu_ipc_peer_t *identity)
{
	if (pid != live.pid) return -NXU_SYS_E_NOT_FOUND;
	*identity = live;
	return 0;
}

bool
WS_Destroy_Window(WSWindowID window)
{
	destroyed = window;
	return true;
}

int
main(void)
{
	g_windows[0] = (windowserver_window_t){live, 101, 7};
	assert(windowserver_find(101, &live) == &g_windows[0]);
	assert(!windowserver_find(102, &live));

	nxu_ipc_peer_t recycled = live;
	recycled.uniqueid++;
	assert(!windowserver_find(101, &recycled));
	assert(!windowserver_peer_live(&recycled));
	recycled = live;
	recycled.idversion++;
	assert(!windowserver_find(101, &recycled));
	assert(!windowserver_peer_live(&recycled));

	/* A new lifetime with the same PID cannot keep the old client's window. */
	live.uniqueid++;
	live.idversion++;
	windowserver_reap_windows();
	assert(destroyed == 7 && !g_windows[0].handle);
	assert(!windowserver_find(101, &live));
	return 0;
}
