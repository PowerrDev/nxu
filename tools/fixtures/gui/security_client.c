/* An unprivileged peer attempts to hijack registration and another client's windows. */
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>

static uint32_t reply_port;

static bool
call(uint32_t service, wsmsg_request_t *request, uint64_t size, wsmsg_reply_t *reply)
{
	if (nxu_ipc_send(service, request, size, reply_port) < 0) return false;

	uint64_t deadline = (uint64_t)nxu_uptime_us() + 5000000;

	while ((uint64_t)nxu_uptime_us() < deadline) {
		int64_t received = nxu_ipc_receive(reply_port, reply, sizeof(*reply), 0, 0);
		if (received >= 0) return received == sizeof(*reply);
		(void)nxu_sleep_us(1000);
	}
	return false;
}

int
main(void)
{
	uint32_t control;
	if (!bootstrap_client_lookup("com.nxu.gui-security", &control)) return 1;

	int64_t allocated = nxu_ipc_port_allocate();
	if (allocated <= 0) return 2;
	reply_port = (uint32_t)allocated;

	/* Register an endpoint we own under the protected compositor label. */
	int64_t fake = nxu_ipc_port_allocate();
	if (fake <= 0 || !bootstrap_client_register(WS_BOOTSTRAP_LABEL, (uint32_t)fake)) return 3;

	uint32_t service;
	if (!bootstrap_client_lookup(WS_BOOTSTRAP_LABEL, &service)) return 4;

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.action.opcode = WSMSG_ACTION;
	request.action.action = WS_ACTION_DISPLAY_SIZE;
	if (!call(service, &request, sizeof(request.action), &reply) || reply.status || !reply.window_id) return 5;

	/* A service send right must never permit stealing its queued requests. */
	if (nxu_ipc_receive(service, &request, sizeof(request), 0, 0) != -NXU_SYS_E_DENIED) return 6;

	request.create_window = (wsmsg_create_window_t){WSMSG_CREATE_WINDOW, 240, 240, 16, 16};
	if (!call(service, &request, sizeof(request.create_window), &reply) || reply.status) return 7;
	uint32_t own = reply.window_id;
	request.render_window = (wsmsg_render_window_t){WSMSG_RENDER_WINDOW, own, 16, 16, 16, 256, {0}};
	for (uint32_t i = 0; i < 256; i++)
		request.render_window.pixels[i] = 0xffffff00;
	if (!call(service, &request, 24 + 256 * 4, &reply) || reply.status) return 8;

	/* Transfer only send access to our private reply endpoint as a handshake. */
	if (nxu_ipc_send(control, &own, sizeof(own), reply_port) < 0) return 9;
	uint32_t target = 0;
	while (nxu_ipc_receive(reply_port, &target, sizeof(target), 0, 0) < 0)
		(void)nxu_sleep_us(1000);

	request.resize_window = (wsmsg_resize_window_t){WSMSG_RESIZE_WINDOW, target, 0, 0, 32, 32};
	if (!call(service, &request, sizeof(request.resize_window), &reply) || reply.status != -NXU_SYS_E_DENIED) return 10;
	request.destroy_window.opcode = WSMSG_DESTROY_WINDOW;
	request.destroy_window.window_id = target;
	if (!call(service, &request, sizeof(request.destroy_window), &reply) || reply.status != -NXU_SYS_E_DENIED)
		return 11;
	request.render_window = (wsmsg_render_window_t){WSMSG_RENDER_WINDOW, target, 1, 1, 1, 1, {0}};
	if (!call(service, &request, 28, &reply) || reply.status != -NXU_SYS_E_DENIED) return 12;
	request.render_window_shm = (wsmsg_render_window_shm_t){WSMSG_RENDER_WINDOW_SHM, target, 16, 16, 16, 0};
	if (!call(service, &request, sizeof(request.render_window_shm), &reply) || reply.status != -NXU_SYS_E_DENIED)
		return 13;
	request.action.opcode = WSMSG_ACTION;
	request.action.action = WS_ACTION_MOVE;
	request.action.window_id = target;
	if (!call(service, &request, sizeof(request.action), &reply) || reply.status != -NXU_SYS_E_DENIED) return 14;
	request.action.action = WS_ACTION_POINTER;
	if (!call(service, &request, sizeof(request.action), &reply) || reply.status != -NXU_SYS_E_DENIED) return 15;

	uint32_t passed = 1;
	if (nxu_ipc_send(control, &passed, sizeof(passed), 0) < 0) return 16;
	for (;;)
		(void)nxu_sleep_us(1000000);
}
