#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * About sevOS as a real, separate OS process: discovers WindowServer by
 * name through the bootstrap registry, creates a window sized to match the
 * real About sevOS app (about_sevos::AboutApp::WINDOW, 1040x780 --
 * UIService.framework/apps/about-sevos/src/lib.rs), draws its content
 * itself into shared memory, and asks WindowServer to composite and
 * present it -- purely over NXPC and nxu_shm, no direct function calls
 * into WindowServer's address space and no dependency on windowserver-nxu
 * at all, matching the process boundary WindowServer itself now runs
 * behind (see windowserver_service.c).
 *
 * The window's content here is a simple placeholder (titlebar + body +
 * a mark) drawn directly in C, not the real about-sevos Rust crate's
 * font-rendered panel -- reusing that crate standalone would mean also
 * solving its text-rendering font-asset dependency outside the existing
 * UIService host-ABI build, which is out of scope for proving the process/
 * IPC architecture this milestone is actually about. The window size,
 * discovery path, rendering path (shared memory) and present path are all
 * real.
 */

#define ABOUT_WINDOW_X 120
#define ABOUT_WINDOW_Y 90
#define ABOUT_WINDOW_WIDTH 1040U
#define ABOUT_WINDOW_HEIGHT 780U
#define ABOUT_TITLEBAR_HEIGHT 32U

#define ABOUT_COLOR_TITLEBAR 0xFF3A3A3CU
#define ABOUT_COLOR_BODY 0xFFF5F5F7U
#define ABOUT_COLOR_MARK 0xFF0A84FFU
#define ABOUT_MARK_SIZE 96U

static void
about_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static void
about_draw(uint32_t *pixels, uint32_t width, uint32_t height)
{
	for (uint32_t y = 0U; y < height; y++) {
		uint32_t color = y < ABOUT_TITLEBAR_HEIGHT ? ABOUT_COLOR_TITLEBAR : ABOUT_COLOR_BODY;
		for (uint32_t x = 0U; x < width; x++) pixels[(uint64_t)y * width + x] = color;
	}

	uint32_t mark_x = (width - ABOUT_MARK_SIZE) / 2U;
	uint32_t mark_y = ABOUT_TITLEBAR_HEIGHT + (height - ABOUT_TITLEBAR_HEIGHT - ABOUT_MARK_SIZE) / 3U;
	for (uint32_t y = 0U; y < ABOUT_MARK_SIZE; y++) {
		for (uint32_t x = 0U; x < ABOUT_MARK_SIZE; x++) {
			pixels[(uint64_t)(mark_y + y) * width + (mark_x + x)] = ABOUT_COLOR_MARK;
		}
	}
}

static bool
about_call(uint32_t dest_name, uint32_t reply_name, const wsmsg_request_t *request, uint64_t request_size, wsmsg_reply_t *reply)
{
	if (nxu_ipc_send(dest_name, request, request_size, reply_name) != 0) return false;

	for (;;) {
		int64_t received = nxu_ipc_receive(reply_name, reply, sizeof(*reply), 0, 0);
		if (received >= 0) return (uint64_t)received == sizeof(*reply);
		if (received != -NXU_SYS_E_AGAIN) return false;
		(void)nxu_yield();
	}
}

int
main(void)
{
	uint32_t windowserver_name = 0U;
	if (!bootstrap_client_lookup(WS_BOOTSTRAP_LABEL, &windowserver_name)) {
		about_log("about_sevos: bootstrap lookup failed\n");
		return 1;
	}

	about_log("about_sevos: found " WS_BOOTSTRAP_LABEL " via bootstrap registry\n");

	int64_t reply_name = nxu_ipc_port_allocate();
	if (reply_name <= 0) {
		about_log("about_sevos: reply port allocation failed\n");
		return 2;
	}

	wsmsg_request_t create_request;
	create_request.create_window.opcode = (uint32_t)WSMSG_CREATE_WINDOW;
	create_request.create_window.x = ABOUT_WINDOW_X;
	create_request.create_window.y = ABOUT_WINDOW_Y;
	create_request.create_window.width = ABOUT_WINDOW_WIDTH;
	create_request.create_window.height = ABOUT_WINDOW_HEIGHT;

	wsmsg_reply_t reply;
	if (!about_call(windowserver_name, (uint32_t)reply_name, &create_request, sizeof(create_request.create_window), &reply) || reply.status != 0) {
		about_log("about_sevos: create window failed\n");
		return 3;
	}

	uint32_t window_id = reply.window_id;
	about_log("about_sevos: window created\n");

	uint64_t pixel_count = (uint64_t)ABOUT_WINDOW_WIDTH * (uint64_t)ABOUT_WINDOW_HEIGHT;
	uint64_t byte_size = pixel_count * sizeof(uint32_t);

	int64_t shm_id = nxu_shm_create(byte_size);
	if (shm_id <= 0) {
		about_log("about_sevos: shared memory allocation failed\n");
		return 4;
	}

	int64_t shm_va = nxu_shm_map((uint32_t)shm_id);
	if (shm_va <= 0) {
		about_log("about_sevos: shared memory mapping failed\n");
		return 5;
	}

	about_draw((uint32_t *)(uintptr_t)shm_va, ABOUT_WINDOW_WIDTH, ABOUT_WINDOW_HEIGHT);

	wsmsg_request_t render_request;
	render_request.render_window_shm.opcode = (uint32_t)WSMSG_RENDER_WINDOW_SHM;
	render_request.render_window_shm.window_id = window_id;
	render_request.render_window_shm.width = ABOUT_WINDOW_WIDTH;
	render_request.render_window_shm.height = ABOUT_WINDOW_HEIGHT;
	render_request.render_window_shm.stride = ABOUT_WINDOW_WIDTH;
	render_request.render_window_shm.shm_id = (uint32_t)shm_id;

	if (!about_call(windowserver_name, (uint32_t)reply_name, &render_request, sizeof(render_request.render_window_shm), &reply) || reply.status != 0) {
		about_log("about_sevos: render window failed\n");
		return 6;
	}

	about_log("about_sevos: window rendered via shared memory\n");

	wsmsg_request_t present_request;
	present_request.present.opcode = (uint32_t)WSMSG_PRESENT;
	if (nxu_ipc_send(windowserver_name, &present_request, sizeof(present_request.present), 0U) != 0) {
		about_log("about_sevos: present send failed\n");
		return 7;
	}

	about_log("about_sevos: present requested, exiting\n");
	return 0;
}
