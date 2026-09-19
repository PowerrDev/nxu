#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway client proving WindowServer works as a real, separate process:
 * looks it up by name through the bootstrap registry, creates a window,
 * renders a small solid-color rectangle into it, and presents -- purely
 * over NXPC, no direct function calls into WindowServer's address space.
 * Verified visually via QEMU screendump (see kern/tests -- this binary has
 * no self-check of its own beyond the request/reply status codes; the
 * rendered rectangle is confirmed by eye against the expected screen
 * region).
 */

#define WSTEST_RETRIES 2000000U
#define WSTEST_WINDOW_X 40
#define WSTEST_WINDOW_Y 40
#define WSTEST_WINDOW_WIDTH 16U
#define WSTEST_WINDOW_HEIGHT 16U
#define WSTEST_PIXEL_COLOR 0xFFFF0000U /* opaque red */

static void
wstest_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
wstest_call(uint32_t dest_name, uint32_t reply_name, const wsmsg_request_t *request, uint64_t request_size, wsmsg_reply_t *reply)
{
	if (nxu_ipc_send(dest_name, request, request_size, reply_name) != 0) return false;

	for (uint32_t attempt = 0U; attempt < WSTEST_RETRIES; attempt++) {
		int64_t received = nxu_ipc_receive(reply_name, reply, sizeof(*reply), 0, 0);
		if (received >= 0) return (uint64_t)received == sizeof(*reply);
		if (received != -NXU_SYS_E_AGAIN) return false;
		(void)nxu_yield();
	}

	return false;
}

int
main(void)
{
	uint32_t windowserver_name = 0U;
	if (!bootstrap_client_lookup(WS_BOOTSTRAP_LABEL, &windowserver_name)) {
		wstest_log("wstest_client: bootstrap lookup failed\n");
		return 1;
	}

	wstest_log("wstest_client: found " WS_BOOTSTRAP_LABEL " via bootstrap registry\n");

	int64_t reply_name = nxu_ipc_port_allocate();
	if (reply_name <= 0) {
		wstest_log("wstest_client: reply port allocation failed\n");
		return 2;
	}

	wsmsg_request_t create_request;
	create_request.create_window.opcode = (uint32_t)WSMSG_CREATE_WINDOW;
	create_request.create_window.x = WSTEST_WINDOW_X;
	create_request.create_window.y = WSTEST_WINDOW_Y;
	create_request.create_window.width = WSTEST_WINDOW_WIDTH;
	create_request.create_window.height = WSTEST_WINDOW_HEIGHT;

	wsmsg_reply_t reply;
	if (!wstest_call(windowserver_name, (uint32_t)reply_name, &create_request, sizeof(create_request.create_window), &reply) || reply.status != 0) {
		wstest_log("wstest_client: create window failed\n");
		return 3;
	}

	uint32_t window_id = reply.window_id;
	wstest_log("wstest_client: window created\n");

	wsmsg_request_t render_request;
	render_request.render_window.opcode = (uint32_t)WSMSG_RENDER_WINDOW;
	render_request.render_window.window_id = window_id;
	render_request.render_window.width = WSTEST_WINDOW_WIDTH;
	render_request.render_window.height = WSTEST_WINDOW_HEIGHT;
	render_request.render_window.stride = WSTEST_WINDOW_WIDTH;
	render_request.render_window.pixel_count = WSTEST_WINDOW_WIDTH * WSTEST_WINDOW_HEIGHT;
	for (uint32_t index = 0U; index < render_request.render_window.pixel_count; index++) render_request.render_window.pixels[index] = WSTEST_PIXEL_COLOR;

	uint64_t render_request_size = 6ULL * sizeof(uint32_t) + (uint64_t)render_request.render_window.pixel_count * sizeof(uint32_t);
	if (!wstest_call(windowserver_name, (uint32_t)reply_name, &render_request, render_request_size, &reply) || reply.status != 0) {
		wstest_log("wstest_client: render window failed\n");
		return 4;
	}

	wstest_log("wstest_client: window rendered\n");

	wsmsg_request_t present_request;
	present_request.present.opcode = (uint32_t)WSMSG_PRESENT;
	if (nxu_ipc_send(windowserver_name, &present_request, sizeof(present_request.present), 0U) != 0) {
		wstest_log("wstest_client: present send failed\n");
		return 5;
	}

	wstest_log("wstest_client: present requested, exiting\n");
	return 0;
}
