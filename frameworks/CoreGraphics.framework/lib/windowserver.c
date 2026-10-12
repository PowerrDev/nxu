/* WindowServer client adapter. Every compositor operation crosses NXPC. */
#include <WindowServer/WSPrivate.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>
#include <nxu/runtime.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

static uint32_t g_service;
static bool g_connection_lost;

/* This adapter is single-threaded. A reply completes the server's pixel copy. */
static uint64_t g_transfer_id;
static uint64_t g_transfer_capacity;
static uint32_t *g_transfer_pixels;

static void
release_transfer(void)
{
	if (g_transfer_id) (void)nxu_shm_withdraw(g_transfer_id);
	if (g_transfer_pixels) (void)nxu_shm_unmap((uint64_t)(uintptr_t)g_transfer_pixels);

	g_transfer_id = g_transfer_capacity = 0;
	g_transfer_pixels = 0;
}

static bool
call(const wsmsg_request_t *request, uint64_t size, wsmsg_reply_t *reply)
{
	if (!g_service && !bootstrap_client_lookup(WS_BOOTSTRAP_LABEL, &g_service)) return false;

	int64_t port = nxu_ipc_port_allocate();

	if (port <= 0) return false;

	bool sent = nxu_ipc_send(g_service, request, size, (uint32_t)port) >= 0;
	bool received = false;
	if (sent) {
		int64_t length = nxu_ipc_receive_timed((uint32_t)port, reply, sizeof(*reply), 0, 0, 5000000);
		received = length == sizeof(*reply);
	}

	(void)nxu_ipc_port_deallocate((uint32_t)port);

	if (!sent || !received) {
		(void)nxu_ipc_port_deallocate(g_service);
		g_service = 0;
		g_connection_lost = true;
	}

	return received && reply->status == 0;
}

static uint32_t
action(uint32_t operation, uint32_t window, const uint32_t arguments[8])
{
	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;

	request.action.opcode = WSMSG_ACTION;
	request.action.action = operation;
	request.action.window_id = window;
	memcpy(request.action.arguments, arguments, sizeof(request.action.arguments));

	if (!call(&request, sizeof(request.action), &reply)) return 0;

	return operation == WS_ACTION_CREATE || operation == WS_ACTION_BACKGROUND ? reply.window_id : 1;
}

WSWindowID
CG_Create_Window(int32_t x, int32_t y, uint32_t width, uint32_t height, bool opaque, uint32_t radius)
{
	return action(WS_ACTION_CREATE, 0, (uint32_t[8]){x, y, width, height, opaque, radius});
}

WSWindowID
CG_Create_Background_Window(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	return action(WS_ACTION_BACKGROUND, 0, (uint32_t[8]){x, y, width, height});
}

bool
CG_Destroy_Window(WSWindowID window)
{
	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.destroy_window.opcode = WSMSG_DESTROY_WINDOW;
	request.destroy_window.window_id = window;
	return call(&request, sizeof(request.destroy_window), &reply);
}

bool
CG_Move_Window(WSWindowID window, int32_t x, int32_t y)
{
	return action(WS_ACTION_MOVE, window, (uint32_t[8]){x, y});
}

bool
CG_Set_Size_Limits(WSWindowID window, uint32_t min_width, uint32_t min_height, uint32_t max_width, uint32_t max_height)
{
	return action(WS_ACTION_LIMITS, window, (uint32_t[8]){min_width, min_height, max_width, max_height});
}

bool
CG_Resize_Window(WSWindowID window, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.resize_window = (wsmsg_resize_window_t){WSMSG_RESIZE_WINDOW, window, x, y, width, height};
	return call(&request, sizeof(request.resize_window), &reply);
}

bool
CG_Focus_Window(WSWindowID window)
{
	return action(WS_ACTION_FOCUS, window, (uint32_t[8]){0});
}

bool
CG_Set_Window_Shadow(WSWindowID window, uint32_t style)
{
	return action(WS_ACTION_SHADOW, window, (uint32_t[8]){style});
}

bool
CG_Set_Window_Key(WSWindowID window, bool key)
{
	return action(WS_ACTION_KEY, window, (uint32_t[8]){key});
}

bool
CG_Set_Window_Opaque_Interior(WSWindowID window, bool opaque)
{
	return action(WS_ACTION_OPAQUE_INTERIOR, window, (uint32_t[8]){opaque});
}

void
CG_Set_Shadow_Scale(uint32_t scale)
{
	(void)action(WS_ACTION_SHADOW_SCALE, 0, (uint32_t[8]){scale});
}

bool
CG_Pointer_Move(int32_t x, int32_t y)
{
	return action(WS_ACTION_POINTER, 0, (uint32_t[8]){x, y});
}

bool
CG_Set_Cursor_Kind(uint32_t kind)
{
	return action(WS_ACTION_CURSOR_KIND, 0, (uint32_t[8]){kind});
}

static int64_t
share(const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
	if (!pixels || !width || !height || width > 3840 || height > 2160 || stride < width) return -1;

	uint64_t bytes = (uint64_t)width * height * sizeof(uint32_t);

	if (bytes > g_transfer_capacity) {
		release_transfer();
		int64_t id = nxu_shm_create(bytes);

		if (id <= 0) return -1;

		int64_t address = nxu_shm_map((uint64_t)id);

		if (address <= 0) {
			(void)nxu_shm_withdraw((uint64_t)id);
			return -1;
		}

		g_transfer_id = (uint64_t)id;
		g_transfer_capacity = bytes;
		g_transfer_pixels = (void *)(uintptr_t)address;
	}

	for (uint32_t y = 0; y < height; y++)
		memcpy(g_transfer_pixels + (uint64_t)y * width, pixels + (uint64_t)y * stride, width * sizeof(uint32_t));

	return (int64_t)g_transfer_id;
}

bool
CG_Render_Window(WSWindowID window, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride)
{
	int64_t id = share(pixels, width, height, stride);

	if (id <= 0) return false;

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.render_window_shm = (wsmsg_render_window_shm_t){WSMSG_RENDER_WINDOW_SHM, window, width, height, width, id};
	bool rendered = call(&request, sizeof(request.render_window_shm), &reply);
	/* A timed-out request may still be queued: never overwrite its buffer. */
	if (!rendered) release_transfer();
	return rendered;
}

bool
CG_Set_Cursor(uint32_t kind, const uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride,
			  int32_t hotspot_x, int32_t hotspot_y)
{
	int64_t id = share(pixels, width, height, stride);

	if (id <= 0) return false;

	bool updated = action(WS_ACTION_CURSOR, 0, (uint32_t[8]){kind, width, height, width, hotspot_x, hotspot_y, id});
	if (!updated) release_transfer();
	return updated;
}

bool
CG_Present(void)
{
	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.present.opcode = WSMSG_PRESENT;
	return call(&request, sizeof(request.present), &reply);
}

bool
CoreGraphicsDisplaySize(uint32_t *width, uint32_t *height)
{
	if (!width || !height) return false;

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.action.opcode = WSMSG_ACTION;
	request.action.action = WS_ACTION_DISPLAY_SIZE;

	if (!call(&request, sizeof(request.action), &reply)) return false;

	g_connection_lost = false;
	*width = reply.window_id >> 16;
	*height = reply.window_id & 0xffff;
	return *width != 0 && *height != 0;
}

bool
CoreGraphicsConnectionAlive(void)
{
	return !g_connection_lost;
}
