#include <WindowServer/WSPrivate.h>

#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/runtime.h>
#include <nxu/gui_host_protocol.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

#include <stdbool.h>
#include <stdint.h>

/* The standalone service loads WindowServer.framework through dyrld. The
 * framework's allocator, clock and panic adapters resolve from Runtime. */
#define WINDOWSERVER_MAX_WIDTH 3840U
#define WINDOWSERVER_MAX_HEIGHT 2160U

static int64_t claim_record(void *record, uint64_t size)
{
	int64_t status;

	/* Login and SAP temporarily exclude even an authorized compositor.
	 * Waiting preserves bootd's crash budget for actual service failures. */
	while ((status = nxu_gui_host(record, size)) == -NXU_SYS_E_DENIED)
		(void)nxu_sleep_us(50000);

	return status;
}

static bool windowserver_claim(void)
{
	union {
		nxu_gui_host_hello_t hello;
		nxu_gui_host_hello_reply_t reply;
	} record = {0};

	record.hello.header = (nxu_gui_host_header_t){sizeof(record.hello), 1, 0, NXU_GUI_HOST_HELLO, 0, 0, 1};
	record.hello.required_features = record.hello.supported_features = NXU_GUI_HOST_FEATURE_LEASE;
	record.hello.max_record_size = NXU_GUI_HOST_MAX_RECORD;
	if (claim_record(&record, sizeof(record)))
		return false;

	uint64_t epoch = record.reply.header.session_epoch;
	record.hello.header =
		(nxu_gui_host_header_t){sizeof(nxu_gui_host_header_t), 1, 0, NXU_GUI_HOST_DISPLAY_ATTACH, 0, epoch, 2};
	return claim_record(&record, sizeof(record)) == 0;
}

static uint32_t g_display_width;
static uint32_t g_display_height;
static uint32_t *g_shadow_framebuffer;

static void windowserver_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

/*
 * WSDisplay's present callback. x/y/width/height bound the damaged rect
 * WS_Present just composited into g_shadow_framebuffer; nxu_recovery_present
 * copies that rect from user memory into the real scanout, gated to this
 * process by the display-ownership claim taken in main() (see
 * kern/console/display_owner.h). pixels must address the FULL
 * (stride-shaped) buffer, not an offset sub-rectangle -- the syscall applies
 * x/y/stride itself, the same convention display->framebuffer uses kernel-
 * side (kern/syscall/syscall.c's syscall_recovery_present).
 */
static bool windowserver_present(void *context, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	(void)context;
	return nxu_recovery_present(g_shadow_framebuffer, g_display_width, x, y, width, height) == 0;
}

/* Mirrors drivers/input/mouse.h's MOUSE_BUTTON_* bit layout -- not
 * available to userland builds, so duplicated here rather than pulled in. */
#define WS_INPUT_BUTTON_LEFT 0x01U
#define WS_INPUT_BUTTON_RIGHT 0x02U
#define WS_INPUT_BUTTON_MIDDLE 0x04U

static uint32_t g_pointer_buttons;
static int32_t g_pointer_x;
static int32_t g_pointer_y;
static int32_t g_pointer_raw_x;
static int32_t g_pointer_raw_y;
static bool g_pointer_baseline;

static void windowserver_dispatch_button(uint32_t previous, uint32_t current, uint32_t mask, uint32_t button, int32_t x,
										 int32_t y)
{
	if ((previous ^ current) & mask)
		(void)WS_Pointer_Button(x, y, button, (current & mask) != 0U);
}

/* event.x/event.y are NXU's raw relative pointer accumulator (see mouse.c),
 * not absolute screen coordinates -- WS_Pointer_Move needs the latter, so
 * this tracks its own clamped absolute position from the accumulator's
 * deltas, exactly like triageOS/main.c's pointer handling does for the
 * recovery UI. Feeding the raw accumulator straight into WS_Pointer_Move
 * (as an earlier version of this function did) makes the cursor crawl
 * toward the real position over many small increments instead of tracking
 * the mouse directly.
 */
static int32_t windowserver_clamp_pointer(int64_t value, uint32_t extent)
{
	if (extent == 0U || value < 0)
		return 0;

	if ((uint64_t)value >= (uint64_t)extent)
		return (int32_t)(extent - 1U);

	return (int32_t)value;
}

/* Drains every already-normalized input event queued since the last poll
 * (nxu_recovery_input hands back one event per call, coalesced kernel-side
 * from the raw VirtIO packets -- see triageOS/main.c for the same pattern)
 * and folds it into WindowServer's pointer state. Button events are
 * synthesized from the edges of the reported button mask: the syscall
 * reports current-state-per-sync, not discrete press/release like
 * WS_Pointer_Button expects.
 */
static void windowserver_pump_input(void)
{
	nxu_recovery_input_event_t event;
	int64_t received;

	while ((received = nxu_recovery_input(&event)) > 0) {
		if (event.kind != NXU_RECOVERY_INPUT_POINTER)
			continue;

		/* Adopt the first packet as the baseline instead of jumping the
		 * cursor by motion accumulated before this process started polling. */
		if (!g_pointer_baseline) {
			g_pointer_raw_x = event.x;
			g_pointer_raw_y = event.y;
			g_pointer_baseline = true;
		}

		int64_t next_x = (int64_t)g_pointer_x + ((int64_t)event.x - g_pointer_raw_x);
		int64_t next_y = (int64_t)g_pointer_y + ((int64_t)event.y - g_pointer_raw_y);
		g_pointer_raw_x = event.x;
		g_pointer_raw_y = event.y;
		g_pointer_x = windowserver_clamp_pointer(next_x, g_display_width);
		g_pointer_y = windowserver_clamp_pointer(next_y, g_display_height);

		(void)WS_Pointer_Move(g_pointer_x, g_pointer_y);

		uint32_t previous = g_pointer_buttons;
		windowserver_dispatch_button(previous, event.buttons, WS_INPUT_BUTTON_LEFT, 0U, g_pointer_x, g_pointer_y);
		windowserver_dispatch_button(previous, event.buttons, WS_INPUT_BUTTON_RIGHT, 1U, g_pointer_x, g_pointer_y);
		windowserver_dispatch_button(previous, event.buttons, WS_INPUT_BUTTON_MIDDLE, 2U, g_pointer_x, g_pointer_y);
		g_pointer_buttons = event.buttons;
	}

	if (received < 0)
		g_pointer_baseline = false;
}

static void windowserver_action(const wsmsg_request_t *request, wsmsg_reply_t *reply)
{
	const uint32_t *a = request->action.arguments;
	uint32_t window = request->action.window_id;
	bool accepted = false;

	switch (request->action.action) {
	case WS_ACTION_CREATE:
	case WS_ACTION_BACKGROUND:
		if (!a[2] || !a[3] || a[2] > WINDOWSERVER_MAX_WIDTH || a[3] > WINDOWSERVER_MAX_HEIGHT)
			break;

		reply->window_id = request->action.action == WS_ACTION_CREATE
							   ? WS_Create_Window((int32_t)a[0], (int32_t)a[1], a[2], a[3], a[4] != 0, a[5])
							   : WS_Create_Background_Window((int32_t)a[0], (int32_t)a[1], a[2], a[3]);
		accepted = reply->window_id != 0;
		break;

	case WS_ACTION_MOVE:
		accepted = WS_Move_Window(window, (int32_t)a[0], (int32_t)a[1]);
		break;

	case WS_ACTION_LIMITS:
		accepted = WS_Set_Size_Limits(window, a[0], a[1], a[2], a[3]);
		break;

	case WS_ACTION_FOCUS:
		accepted = WS_Focus_Window(window);
		break;

	case WS_ACTION_SHADOW:
		accepted = WS_Set_Window_Shadow(window, a[0]);
		break;

	case WS_ACTION_KEY:
		accepted = WS_Set_Window_Key(window, a[0] != 0);
		break;

	case WS_ACTION_OPAQUE_INTERIOR:
		accepted = WS_Set_Window_Opaque_Interior(window, a[0] != 0);
		break;

	case WS_ACTION_SHADOW_SCALE:
		if (a[0] && a[0] <= 4000) {
			WS_Set_Shadow_Scale(a[0]);
			accepted = true;
		}
		break;

	case WS_ACTION_DISPLAY_SIZE:
		reply->window_id = (g_display_width << 16) | g_display_height;
		accepted = true;
		break;

	case WS_ACTION_POINTER:
		accepted = WS_Pointer_Move((int32_t)a[0], (int32_t)a[1]);
		break;

	case WS_ACTION_CURSOR_KIND:
		accepted = WS_Set_Cursor_Kind(a[0]);
		break;

	case WS_ACTION_CURSOR:
		if (!a[1] || !a[2] || a[1] > 256 || a[2] > 256 || a[3] != a[1])
			break;

		int64_t address = nxu_shm_map_sized(a[6], (uint64_t)a[1] * a[2] * sizeof(uint32_t));

		if (address > 0) {
			accepted = WS_Set_Cursor(a[0], (void *)(uintptr_t)address, a[1], a[2], a[3], (int32_t)a[4], (int32_t)a[5]);
			if (nxu_shm_unmap((uint64_t)address))
				accepted = false;
		}
		break;

	default:
		break;
	}

	reply->status = accepted ? 0 : -1;
}

static void windowserver_dispatch_request(const wsmsg_request_t *request, uint64_t size, wsmsg_reply_t *reply)
{
	*reply = (wsmsg_reply_t){.status = -1, .window_id = 0U};

	switch (request->opcode) {
	case WSMSG_CREATE_WINDOW: {
		if (size != sizeof(request->create_window))
			break;

		/* No corner radius over this IPC protocol yet: a client asking this
		 * server for a window gets a square one, unaffected by the shadow's
		 * own rounding (WS_Create_Window's new last argument -- see
		 * WindowServer.framework's WindowFlags::corner_radius). */
		WSWindowID id = WS_Create_Window(request->create_window.x, request->create_window.y,
										 request->create_window.width, request->create_window.height, true, 0U);
		reply->status = id != 0U ? 0 : -1;
		reply->window_id = id;
		break;
	}

	case WSMSG_RENDER_WINDOW: {
		const wsmsg_render_window_t *render = &request->render_window;
		bool shape_ok = size >= 6U * sizeof(uint32_t) && render->width && render->height &&
						render->stride == render->width &&
						(uint64_t)render->pixel_count == (uint64_t)render->width * render->height &&
						render->pixel_count <= WSMSG_MAX_INLINE_PIXELS &&
						size == 6U * sizeof(uint32_t) + (uint64_t)render->pixel_count * sizeof(uint32_t);

		bool ok = shape_ok &&
				  WS_Render_Window(render->window_id, render->pixels, render->width, render->height, render->stride);
		reply->status = ok ? 0 : -1;
		break;
	}

	case WSMSG_PRESENT:
		if (size != sizeof(request->present))
			break;

		reply->status = WS_Present() ? 0 : -1;
		break;
	case WSMSG_RENDER_WINDOW_SHM: {
		const wsmsg_render_window_shm_t *render = &request->render_window_shm;

		if (size != sizeof(*render) || !render->width || !render->height || render->stride != render->width ||
			render->width > WINDOWSERVER_MAX_WIDTH || render->height > WINDOWSERVER_MAX_HEIGHT)
			break;

		int64_t va = nxu_shm_map_sized(render->shm_id, (uint64_t)render->width * render->height * sizeof(uint32_t));
		bool ok = va > 0 && WS_Render_Window(render->window_id, (const uint32_t *)(uintptr_t)va, render->width,
											 render->height, render->stride);
		if (va > 0 && nxu_shm_unmap((uint64_t)va))
			ok = false;
		reply->status = ok ? 0 : -1;
		break;
	}

	case WSMSG_RESIZE_WINDOW: {
		const wsmsg_resize_window_t *r = &request->resize_window;

		if (size == sizeof(*r))
			reply->status = WS_Resize_Window(r->window_id, r->x, r->y, r->width, r->height) ? 0 : -1;
		break;
	}

	case WSMSG_ACTION:
		if (size == sizeof(request->action))
			windowserver_action(request, reply);
		break;

	case WSMSG_DESTROY_WINDOW:
		if (size == sizeof(request->destroy_window))
			reply->status = WS_Destroy_Window(request->destroy_window.window_id) ? 0 : -1;
		break;

	default:
		break;
	}
}

/* Public handles are kernel-issued and never framework slot indexes.
 * They remain unique across compositor restarts for the whole boot. */
#define WINDOWSERVER_CLIENT_WINDOWS 256U

typedef struct {
	nxu_ipc_peer_t owner;
	uint32_t handle;
	WSWindowID internal;
} windowserver_window_t;

static windowserver_window_t g_windows[WINDOWSERVER_CLIENT_WINDOWS];

static bool windowserver_peer_live(const nxu_ipc_peer_t *peer)
{
	nxu_ipc_peer_t live;
	return peer->uniqueid && nxu_process_identity(peer->pid, &live) == 0 && nxu_ipc_peer_equal(peer, &live) &&
		   (peer->capabilities & live.capabilities) == peer->capabilities;
}

static windowserver_window_t *windowserver_find(uint32_t handle, const nxu_ipc_peer_t *owner)
{
	for (uint32_t i = 0; i < WINDOWSERVER_CLIENT_WINDOWS; i++) {
		windowserver_window_t *window = &g_windows[i];
		if (window->handle == handle && nxu_ipc_peer_equal(&window->owner, owner))
			return window;
	}
	return 0;
}

static void windowserver_reap_windows(void)
{
	static uint32_t cursor;
	windowserver_window_t *window = &g_windows[cursor++ % WINDOWSERVER_CLIENT_WINDOWS];
	if (window->handle && !windowserver_peer_live(&window->owner)) {
		(void)WS_Destroy_Window(window->internal);
		*window = (windowserver_window_t){0};
	}
}

static void windowserver_handle_request(const wsmsg_request_t *incoming, uint64_t size, const nxu_ipc_peer_t *sender,
										wsmsg_reply_t *reply)
{
	*reply = (wsmsg_reply_t){.status = -NXU_SYS_E_DENIED};
	if (size < sizeof(uint32_t) || !windowserver_peer_live(sender))
		return;

	wsmsg_request_t request = *incoming;
	uint32_t *handle = 0;
	bool create = false;
	bool privileged = false;

	switch (request.opcode) {
	case WSMSG_CREATE_WINDOW:
		if (size != sizeof(request.create_window) || !request.create_window.width || !request.create_window.height ||
			request.create_window.width > WINDOWSERVER_MAX_WIDTH ||
			request.create_window.height > WINDOWSERVER_MAX_HEIGHT)
			return;
		create = true;
		break;
	case WSMSG_RENDER_WINDOW:
		if (size < 6 * sizeof(uint32_t))
			return;
		handle = &request.render_window.window_id;
		break;
	case WSMSG_RENDER_WINDOW_SHM:
		if (size != sizeof(request.render_window_shm))
			return;
		handle = &request.render_window_shm.window_id;
		break;
	case WSMSG_RESIZE_WINDOW:
		if (size != sizeof(request.resize_window))
			return;
		handle = &request.resize_window.window_id;
		break;
	case WSMSG_DESTROY_WINDOW:
		if (size != sizeof(request.destroy_window))
			return;
		handle = &request.destroy_window.window_id;
		break;
	case WSMSG_PRESENT:
		privileged = true;
		break;
	case WSMSG_ACTION:
		if (size != sizeof(request.action))
			return;
		switch (request.action.action) {
		case WS_ACTION_CREATE:
			create = true;
			break;
		case WS_ACTION_BACKGROUND:
			create = privileged = true;
			break;
		case WS_ACTION_DISPLAY_SIZE:
			break;
		case WS_ACTION_MOVE:
		case WS_ACTION_LIMITS:
		case WS_ACTION_FOCUS:
		case WS_ACTION_SHADOW:
		case WS_ACTION_KEY:
		case WS_ACTION_OPAQUE_INTERIOR:
			handle = &request.action.window_id;
			break;
		default:
			privileged = true;
			break;
		}
		break;
	default:
		return;
	}

	if (privileged && !(sender->capabilities & NXU_CAP_DISPLAY))
		return;

	windowserver_window_t *owned = 0;
	if (handle) {
		owned = windowserver_find(*handle, sender);
		if (!owned)
			return;
		*handle = owned->internal;
	}

	windowserver_window_t *slot = 0;
	if (create) {
		for (uint32_t i = 0; i < WINDOWSERVER_CLIENT_WINDOWS; i++) {
			if (!g_windows[i].handle) {
				slot = &g_windows[i];
				break;
			}
		}
		if (!slot)
			return;
	}

	int64_t public_handle = create ? nxu_gui_window_handle() : 0;
	if (create && public_handle <= 0)
		return;
	windowserver_dispatch_request(&request, size, reply);
	if (reply->status)
		return;

	if (create) {
		slot->owner = *sender;
		slot->internal = reply->window_id;
		slot->handle = (uint32_t)public_handle;
		reply->window_id = slot->handle;
	} else if (request.opcode == WSMSG_DESTROY_WINDOW) {
		*owned = (windowserver_window_t){0};
	}
}

int main(void)
{

	if (!windowserver_claim()) {
		windowserver_log("windowserver: display already claimed, exiting\n");
		return 1;
	}

	nxu_recovery_display_info_t info;

	if (nxu_recovery_display_info(&info) != 0 || info.width == 0U || info.height == 0U) {
		windowserver_log("windowserver: display info unavailable\n");
		return 2;
	}

	if (info.width > WINDOWSERVER_MAX_WIDTH || info.height > WINDOWSERVER_MAX_HEIGHT) {
		windowserver_log("windowserver: display too large for the shadow framebuffer\n");
		return 3;
	}

	g_display_width = info.width;
	g_display_height = info.height;
	g_pointer_x = (int32_t)(g_display_width / 2U);
	g_pointer_y = (int32_t)(g_display_height / 2U);

	uint64_t pixel_count = (uint64_t)g_display_width * (uint64_t)g_display_height;
	g_shadow_framebuffer = nxu_alloc(pixel_count * sizeof(uint32_t), 16);
	if (g_shadow_framebuffer == 0) {
		windowserver_log("windowserver: shadow framebuffer allocation failed\n");
		return 4;
	}

	for (uint64_t index = 0ULL; index < pixel_count; index++)
		g_shadow_framebuffer[index] = 0xFF000000U;

	WSDisplay display = {
		.framebuffer = g_shadow_framebuffer,
		.width = g_display_width,
		.height = g_display_height,
		.stride = g_display_width,
		.present_context = 0,
		.present = windowserver_present,
	};

	if (!WS_Initialize(display)) {
		windowserver_log("windowserver: WS_Initialize failed\n");
		return 5;
	}

	int64_t service_name = nxu_ipc_port_allocate();

	if (service_name <= 0) {
		windowserver_log("windowserver: service port allocation failed\n");
		return 6;
	}

	if (!bootstrap_client_register(WS_BOOTSTRAP_LABEL, (uint32_t)service_name)) {
		windowserver_log("windowserver: bootstrap registration failed\n");
		return 7;
	}

	windowserver_log("windowserver: registered as " WS_BOOTSTRAP_LABEL ", serving requests\n");

	for (;;) {
		wsmsg_request_t request = {0};

		uint32_t xfer_name = 0U;
		uint32_t xfer_type = 0U;
		bool did_work = false;

		nxu_ipc_peer_t sender;
		int64_t received =
			nxu_ipc_receive_peer((uint32_t)service_name, &request, sizeof(request), &xfer_name, &xfer_type, &sender);

		if (received >= 0) {
			did_work = true;

			wsmsg_reply_t reply;
			windowserver_handle_request(&request, (uint64_t)received, &sender, &reply);

			if (xfer_type == NXU_IPC_XFER_PORT && xfer_name != 0U) {
				(void)nxu_ipc_send(xfer_name, &reply, sizeof(reply), 0U);
				(void)nxu_ipc_port_deallocate(xfer_name);
			}
		}

		windowserver_reap_windows();
		windowserver_pump_input();

		/* Re-presents every iteration so cursor motion and the "shake to
		 * locate" animation stay live between client redraws; WS_Present
		 * itself is a no-op (no pacing spin, no syscall) when nothing is
		 * damaged, so this costs nothing on an idle desktop. */
		if (WS_Present())
			did_work = true;

		(void)nxu_sleep_us(did_work ? 1000 : 5000);
	}
}
