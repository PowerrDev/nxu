#include <WindowServer/WSPrivate.h>

#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Standalone WindowServer process: links windowserver-nxu's compositor
 * (crates/windowserver-nxu in the sibling WindowServer.framework repo,
 * built as a staticlib by that repo's nxu/ wrapper crate) into a real NXU
 * userland ELF instead of the kernel. Everything the compositor needs from
 * its host -- kmalloc/kfree, a monotonic clock, and a place to present
 * composited pixels -- is provided here over ordinary syscalls instead of
 * direct kernel calls; see WSPrivate.h/WSDisplay.h for the C ABI this
 * crate already exposes (unchanged from the legacy in-kernel path).
 */

/* Sized to comfortably hold one full display-sized shadow framebuffer
 * (up to WINDOWSERVER_MAX_WIDTH x WINDOWSERVER_MAX_HEIGHT, 4 bytes/pixel,
 * ~8 MiB at the cap) plus the compositor's own small window/state records. */
#define WINDOWSERVER_ARENA_SIZE (16ULL * 1024ULL * 1024ULL)
#define WINDOWSERVER_MAX_WIDTH 1920U
#define WINDOWSERVER_MAX_HEIGHT 1080U

static uint8_t g_arena[WINDOWSERVER_ARENA_SIZE];
static uint64_t g_arena_offset = 0ULL;

/*
 * Bump allocator: never reclaimed. Acceptable for this milestone --
 * WindowServer allocates a small, bounded set of long-lived objects (window
 * records, pixel buffers) and this process does not need to survive window
 * churn heavy enough for that to matter yet.
 */
void *
kmalloc(uint64_t size)
{
	uint64_t aligned = (size + 15ULL) & ~15ULL;
	if (aligned == 0ULL || aligned > WINDOWSERVER_ARENA_SIZE - g_arena_offset) return 0;

	void *pointer = &g_arena[g_arena_offset];
	g_arena_offset += aligned;
	return pointer;
}

bool
kfree(void *address)
{
	(void)address;
	return true;
}

uint64_t
timer_get_microseconds(void)
{
	int64_t now = nxu_uptime_us();
	return now < 0 ? 0ULL : (uint64_t)now;
}

static uint32_t g_display_width;
static uint32_t g_display_height;
static uint32_t *g_shadow_framebuffer;

static void
windowserver_log(const char *message)
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
static bool
windowserver_present(void *context, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
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

static void
windowserver_dispatch_button(uint32_t previous, uint32_t current, uint32_t mask, uint32_t button, int32_t x, int32_t y)
{
	if ((previous ^ current) & mask) (void)WS_Pointer_Button(x, y, button, (current & mask) != 0U);
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
static int32_t
windowserver_clamp_pointer(int64_t value, uint32_t extent)
{
	if (extent == 0U || value < 0) return 0;
	if ((uint64_t)value >= (uint64_t)extent) return (int32_t)(extent - 1U);
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
static void
windowserver_pump_input(void)
{
	nxu_recovery_input_event_t event;
	int64_t received;

	while ((received = nxu_recovery_input(&event)) > 0) {
		if (event.kind != NXU_RECOVERY_INPUT_POINTER) continue;

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
}

static void
windowserver_handle_request(const wsmsg_request_t *request, wsmsg_reply_t *reply)
{
	*reply = (wsmsg_reply_t) { .status = -1, .window_id = 0U };

	switch (request->opcode) {
	case WSMSG_CREATE_WINDOW: {
		/* No corner radius over this IPC protocol yet: a client asking this
		 * server for a window gets a square one, unaffected by the shadow's
		 * own rounding (WS_Create_Window's new last argument -- see
		 * WindowServer.framework's WindowFlags::corner_radius). */
		WSWindowID id = WS_Create_Window(
			request->create_window.x,
			request->create_window.y,
			request->create_window.width,
			request->create_window.height,
			true,
			0U
		);
		reply->status = id != 0U ? 0 : -1;
		reply->window_id = id;
		break;
	}
	case WSMSG_RENDER_WINDOW: {
		const wsmsg_render_window_t *render = &request->render_window;
		bool shape_ok = render->pixel_count == render->width * render->height && render->pixel_count <= WSMSG_MAX_INLINE_PIXELS;
		bool ok = shape_ok && WS_Render_Window(render->window_id, render->pixels, render->width, render->height, render->stride);
		reply->status = ok ? 0 : -1;
		break;
	}
	case WSMSG_PRESENT:
		reply->status = WS_Present() ? 0 : -1;
		break;
	case WSMSG_RENDER_WINDOW_SHM: {
		const wsmsg_render_window_shm_t *render = &request->render_window_shm;
		int64_t va = nxu_shm_map(render->shm_id);
		bool ok = va > 0 && WS_Render_Window(render->window_id, (const uint32_t *)(uintptr_t)va, render->width, render->height, render->stride);
		reply->status = ok ? 0 : -1;
		break;
	}
	default:
		break;
	}
}

int
main(void)
{
	if (nxu_display_claim() != 0) {
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
	g_shadow_framebuffer = kmalloc(pixel_count * sizeof(uint32_t));
	if (g_shadow_framebuffer == 0) {
		windowserver_log("windowserver: shadow framebuffer allocation failed\n");
		return 4;
	}
	for (uint64_t index = 0ULL; index < pixel_count; index++) g_shadow_framebuffer[index] = 0xFF000000U;

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
		wsmsg_request_t request;
		uint32_t xfer_name = 0U;
		uint32_t xfer_type = 0U;
		bool did_work = false;

		int64_t received = nxu_ipc_receive((uint32_t)service_name, &request, sizeof(request), &xfer_name, &xfer_type);
		if (received >= 0) {
			did_work = true;

			wsmsg_reply_t reply;
			windowserver_handle_request(&request, &reply);

			if (xfer_type == NXU_IPC_XFER_PORT && xfer_name != 0U) {
				(void)nxu_ipc_send(xfer_name, &reply, sizeof(reply), 0U);
				(void)nxu_ipc_port_deallocate(xfer_name);
			}
		}

		windowserver_pump_input();

		/* Re-presents every iteration so cursor motion and the "shake to
		 * locate" animation stay live between client redraws; WS_Present
		 * itself is a no-op (no pacing spin, no syscall) when nothing is
		 * damaged, so this costs nothing on an idle desktop. */
		if (WS_Present()) did_work = true;

		if (!did_work) (void)nxu_yield();
	}
}
