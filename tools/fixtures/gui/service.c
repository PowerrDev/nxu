/* Private PID 1 fixture: exercise the production manager and dynamic service. */
#include <frameworks/CoreFoundation.framework/sbin/bootd/manager.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/windowserver_protocol.h>
#include <nxu/gui_host_protocol.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>

static bootd_manager_t manager;
static bootd_job_t *server;
static uint32_t reply_port;
#if defined(NXU_COREGRAPHICS_TEST)
static uint32_t progress_port;
#endif

static void
log_message(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static void
pump(uint64_t duration)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + duration;

	do {
		bootd_manager_poll(&manager);
		(void)nxu_sleep_us(1000);
	} while ((uint64_t)nxu_uptime_us() < deadline);
}

static uint32_t
wait_service(uint64_t previous_pid, uint32_t previous_port)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + 30000000;

	while ((uint64_t)nxu_uptime_us() < deadline) {
		pump(1000);

		if (server->state != BOOTD_JOB_RUNNING || server->pid == previous_pid) continue;

		for (uint32_t i = 0; i < manager.registry.entry_count; i++) {
			bootd_registry_entry_t *entry = &manager.registry.entries[i];

			if (!nxu_streq(entry->label, WS_BOOTSTRAP_LABEL)) continue;

			/* Keep a private send alias. Registry names are reusable slots,
			 * so their numeric equality cannot test endpoint revocation. */
			uint32_t payload = 0, alias = 0, type = 0;
			if (nxu_ipc_send(reply_port, &payload, sizeof(payload), entry->port_name) < 0 ||
				nxu_ipc_receive(reply_port, &payload, sizeof(payload), &alias, &type) != sizeof(payload) ||
				type != NXU_IPC_XFER_PORT || !alias || alias == previous_port)
				return 0;
			return alias;
		}
	}

	return 0;
}

static bool
call(uint32_t port, const wsmsg_request_t *request, uint64_t size, wsmsg_reply_t *reply)
{
	if (nxu_ipc_send(port, request, size, reply_port) < 0) return false;

	uint64_t deadline = (uint64_t)nxu_uptime_us() + 5000000;

	while ((uint64_t)nxu_uptime_us() < deadline) {
		pump(1000);

		int64_t length = nxu_ipc_receive(reply_port, reply, sizeof(*reply), 0, 0);

		if (length >= 0) return length == sizeof(*reply);

		if (length != -NXU_SYS_E_AGAIN) return false;
	}

	return false;
}

static bool
draw(uint32_t port, uint32_t color, uint32_t *window)
{
	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;

	request.create_window = (wsmsg_create_window_t){WSMSG_CREATE_WINDOW, 40, 40, 16, 16};

	if (!call(port, &request, sizeof(request.create_window), &reply) || reply.status) return false;

	*window = reply.window_id;
	request.render_window = (wsmsg_render_window_t){WSMSG_RENDER_WINDOW, *window, 16, 16, 16, 256, {0}};

	for (uint32_t i = 0; i < 256; i++)
		request.render_window.pixels[i] = color;

	if (!call(port, &request, 24 + 256 * 4, &reply) || reply.status) return false;

	pump(100000);
	return true;
}

static bool
shared_render(uint32_t port, uint32_t window)
{
	int64_t id = nxu_shm_create(4096);

	if (id <= 0) return false;

	int64_t address = nxu_shm_map((uint64_t)id);

	if (address <= 0) {
		(void)nxu_shm_withdraw((uint64_t)id);
		return false;
	}

	uint32_t *pixels = (void *)(uintptr_t)address;

	for (uint32_t i = 0; i < 256; i++)
		pixels[i] = 0xffff0000;

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;

	request.render_window_shm = (wsmsg_render_window_shm_t){
		WSMSG_RENDER_WINDOW_SHM, window, 16, 16, 16, (uint32_t)id,
	};

	bool valid = call(port, &request, sizeof(request.render_window_shm), &reply) && reply.status == 0;
	request.render_window_shm.width = request.render_window_shm.stride = 100;
	request.render_window_shm.height = 100;
	bool bounded = call(port, &request, sizeof(request.render_window_shm), &reply) && reply.status != 0;
	request.render_window_shm.width = request.render_window_shm.stride = 16;
	request.render_window_shm.height = 16;

	bool unmapped = nxu_shm_unmap((uint64_t)address) == 0;
	bool withdrawn = nxu_shm_withdraw((uint64_t)id) == 0;

	/* A withdrawn handle must be rejected on the next request. */
	return valid && bounded && unmapped && withdrawn &&
		   call(port, &request, sizeof(request.render_window_shm), &reply) && reply.status != 0;
}

#if defined(NXU_COREGRAPHICS_TEST)
static bool
host_boundary_test(void)
{
	union {
		nxu_gui_host_hello_t request;
		nxu_gui_host_hello_reply_t reply;
	} hello = {0};

	hello.request.header = (nxu_gui_host_header_t){sizeof(hello.request), 1, 0, NXU_GUI_HOST_HELLO, 0, 0, 1};
	hello.request.required_features = hello.request.supported_features = NXU_GUI_HOST_FEATURE_SESSION;
	hello.request.max_record_size = NXU_GUI_HOST_MAX_RECORD;

	if (nxu_gui_host(&hello, sizeof(hello)) != -NXU_SYS_E_NOT_SUPPORTED) return false;

	hello.request = (nxu_gui_host_hello_t){0};
	hello.request.header = (nxu_gui_host_header_t){sizeof(hello.request), 1, 1, NXU_GUI_HOST_HELLO, 0, 0, 2};
	hello.request.required_features = hello.request.supported_features = NXU_GUI_HOST_FEATURE_SESSION;
	hello.request.max_record_size = NXU_GUI_HOST_MAX_RECORD;

	if (nxu_gui_host(&hello, sizeof(hello))) return false;

	uint64_t epoch = hello.reply.header.session_epoch;
	nxu_gui_host_desktop_t record = {0};
	record.header = (nxu_gui_host_header_t){sizeof(record), 1, 1, NXU_GUI_HOST_SESSION_BEGIN, 0, epoch + 1, 3};
	record.width = record.height = 2;
	record.scale_permille = 1000;

	if (nxu_gui_host(&record, sizeof(record)) != -NXU_SYS_E_DENIED) return false;

	record.header.session_epoch = epoch;
	record.width = 0;

	if (nxu_gui_host(&record, sizeof(record)) != -NXU_SYS_E_INVALID_ARGUMENT) return false;

	record.width = 2;

	if (nxu_gui_host(&record, sizeof(record))) return false;

	record.header.operation = NXU_GUI_HOST_DESCRIBE;
	record.connection = 0x123400000001ULL;

	if (nxu_gui_host(&record, sizeof(record)) != -NXU_SYS_E_NOT_FOUND) return false;

	record.header.operation = NXU_GUI_HOST_FRAME_ACQUIRE;
	record.stride = 1;
	record.pixels = UINT64_MAX;
	record.byte_length = UINT64_MAX;

	if (nxu_gui_host(&record, sizeof(record)) != -NXU_SYS_E_INVALID_ARGUMENT) return false;

	hello.request.header =
		(nxu_gui_host_header_t){sizeof(nxu_gui_host_header_t), 1, 1, NXU_GUI_HOST_RELEASE, 0, epoch, 4};

	if (nxu_gui_host(&hello, sizeof(hello))) return false;

	record.header.operation = NXU_GUI_HOST_SESSION_BEGIN;

	if (nxu_gui_host(&record, sizeof(record)) != -NXU_SYS_E_DENIED) return false;

	log_message("gui-service: HOST-BOUNDARY\n");
	return true;
}

static bool
wait_progress(uint32_t expected)
{
	uint64_t deadline = (uint64_t)nxu_uptime_us() + 120000000;

	while ((uint64_t)nxu_uptime_us() < deadline) {
		pump(1000);
		uint32_t stage = 0;

		if (nxu_ipc_receive(progress_port, &stage, sizeof(stage), 0, 0) == sizeof(stage) && stage >= expected)
			return true;
	}

	return false;
}

static bool
desktop_test(void)
{
	if (!host_boundary_test()) return false;

	if (manager.job_count == BOOTD_MAX_JOBS) return false;

	service_config_t config = {0};
	memcpy(config.label, "com.nxu.desktop", sizeof("com.nxu.desktop"));
	memcpy(config.program, "/disk/System/Library/CoreServices/desktop_service",
		   sizeof("/disk/System/Library/CoreServices/desktop_service"));
	config.keep_alive = true;
	config.allow_display = true;

	bootd_job_t *job = &manager.jobs[manager.job_count++];

	if (!bootd_job_init(job, &config, "", false)) return false;

	bootd_job_activate(job);
	pump(5000000);

	int64_t allocated = nxu_ipc_port_allocate();

	if (allocated <= 0 || manager.registry.entry_count >= BOOTD_REGISTRY_MAX_ENTRIES) return false;

	progress_port = (uint32_t)allocated;
	/* PID 1 owns the registry; a synchronous request to itself cannot progress. */
	bootd_registry_entry_t *entry = &manager.registry.entries[manager.registry.entry_count++];
	memcpy(entry->label, "com.nxu.gui-progress", sizeof("com.nxu.gui-progress"));
	entry->port_name = progress_port;
	int64_t client = nxu_spawn("/disk/System/Library/CoreServices/coregraphics-client", "coregraphics-client");

	if (client <= 0) return false;

	pump(5000000);
	log_message("gui-service: DESKTOP\n");
	if (!wait_progress(1)) return false;

	uint64_t old_server = server->pid, old_desktop = job->pid;

	if (nxu_kill(old_server, 9) < 0) return false;

	pump(12000000);

	if (server->pid == old_server || job->pid == old_desktop || nxu_waitpid((uint64_t)client, 0) != -NXU_SYS_E_AGAIN)
		return false;

	log_message("gui-service: COMPOSITOR-RECOVERED\n");
	if (!wait_progress(2)) return false;
	old_desktop = job->pid;
	old_server = server->pid;

	if (nxu_kill(old_desktop, 9) < 0) return false;

	pump(12000000);

	if (job->pid == old_desktop || server->pid == old_server || nxu_waitpid((uint64_t)client, 0) != -NXU_SYS_E_AGAIN)
		return false;

	log_message("gui-service: DESKTOP-RECOVERED\n");
	if (!wait_progress(3)) return false;

	if (nxu_kill((uint64_t)client, 9) < 0) return false;

	pump(3000000);
	log_message("gui-service: APP-CLOSED\n");
	pump(3000000);
	return true;
}
#endif

static bool
security_test(uint32_t port, uint32_t victim)
{
	int64_t allocated = nxu_ipc_port_allocate();
	if (allocated <= 0 || manager.registry.entry_count >= BOOTD_REGISTRY_MAX_ENTRIES) return false;
	uint32_t control = (uint32_t)allocated;
	bootd_registry_entry_t *entry = &manager.registry.entries[manager.registry.entry_count++];
	memcpy(entry->label, "com.nxu.gui-security", sizeof("com.nxu.gui-security"));
	entry->port_name = control;
	int64_t child = nxu_spawn_caps("/disk/System/Library/CoreServices/gui-security-client", "gui-security-client", 0);
	if (child <= 0) return false;

	uint32_t own = 0, transferred = 0, type = 0;
	nxu_ipc_peer_t peer, actual;
	uint64_t deadline = (uint64_t)nxu_uptime_us() + 30000000;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		pump(1000);
		if (nxu_ipc_receive_peer(control, &own, sizeof(own), &transferred, &type, &peer) == sizeof(own)) break;
	}
	if (!own || !transferred || type != NXU_IPC_XFER_PORT || peer.pid != (uint32_t)child ||
		nxu_process_identity((uint32_t)child, &actual) < 0 || !nxu_ipc_peer_equal(&peer, &actual) || peer.capabilities)
		return false;
	if (nxu_ipc_receive(transferred, &own, sizeof(own), 0, 0) != -NXU_SYS_E_DENIED) return false;
	if (nxu_ipc_send(transferred, &victim, sizeof(victim), 0) < 0) return false;
	uint32_t passed = 0;
	while ((uint64_t)nxu_uptime_us() < deadline) {
		pump(1000);
		if (nxu_ipc_receive_peer(control, &passed, sizeof(passed), 0, 0, &peer) == sizeof(passed)) break;
	}
	if (passed != 1 || peer.pid != (uint32_t)child || !nxu_ipc_peer_equal(&peer, &actual)) return false;

	log_message("gui-service: SECURITY-WINDOW\n");
	pump(3000000);
	if (nxu_kill((uint32_t)child, 9) < 0) return false;
	pump(3000000);
	if (nxu_ipc_send(transferred, &victim, sizeof(victim), 0) >= 0) return false;
	(void)nxu_ipc_port_deallocate(transferred);
	log_message("gui-service: SECURITY-REAPED\n");
	pump(3000000);

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;
	request.destroy_window.opcode = WSMSG_DESTROY_WINDOW;
	request.destroy_window.window_id = victim;
	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status) return false;
	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status != -NXU_SYS_E_DENIED)
		return false;
	uint32_t replacement;
	if (!draw(port, 0xffff0000, &replacement) || replacement == victim) return false;
	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status != -NXU_SYS_E_DENIED)
		return false;
	request.destroy_window.window_id = replacement;
	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status) return false;
	log_message("gui-service: SECURITY-PASS\n");
	return true;
}

static bool
exercise(void)
{
	if (nxu_getpid() != 1 || !bootd_manager_init(&manager)) return false;

	for (uint32_t i = 0; i < manager.job_count; i++) {
		if (nxu_streq(manager.jobs[i].config.label, WS_BOOTSTRAP_LABEL)) {
			server = &manager.jobs[i];

			/* The boot flag disables the kernel desktop. Enable only this job. */
			service_config_t config = server->config;

			if (!bootd_job_init(server, &config, "", false)) return false;
		}
	}

	if (!server) return false;

	bootd_manager_start(&manager);

	int64_t allocated = nxu_ipc_port_allocate();

	if (allocated <= 0) return false;

	reply_port = (uint32_t)allocated;

	uint32_t port = wait_service(0, 0);
	uint32_t window;
	if (!port || !draw(port, 0xffff0000, &window) || !security_test(port, window)) return false;

	if (!port || !draw(port, 0xffff0000, &window) || !shared_render(port, window)) return false;

	log_message("gui-service: RED\n");
	pump(4000000);

	wsmsg_request_t request = {0};
	wsmsg_reply_t reply;

	request.render_window.opcode = WSMSG_RENDER_WINDOW;

	if (!call(port, &request, 4, &reply) || reply.status == 0) return false;

	request.resize_window = (wsmsg_resize_window_t){WSMSG_RESIZE_WINDOW, window, 80, 80, 16, 16};

	if (!call(port, &request, sizeof(request.resize_window), &reply) || reply.status) return false;

	log_message("gui-service: RESIZED\n");
	pump(4000000);

	request.destroy_window.opcode = WSMSG_DESTROY_WINDOW;
	request.destroy_window.window_id = window;

	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status) return false;

	if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status == 0) return false;

	log_message("gui-service: DESTROYED\n");
	pump(4000000);

	const uint32_t signals[] = {6, 9}; /* SIGABRT capture, then SIGKILL cleanup. */

	for (uint32_t i = 0; i < 2; i++) {
		uint64_t previous_pid = server->pid;
		uint32_t previous_port = port;
		uint32_t previous_window = window;

		if (nxu_kill(previous_pid, signals[i]) < 0) return false;

		port = wait_service(previous_pid, previous_port);

		request.present.opcode = WSMSG_PRESENT;

		if (nxu_ipc_send(previous_port, &request, sizeof(request.present), 0) >= 0) return false;
		(void)nxu_ipc_port_deallocate(previous_port);

		if (!port || !draw(port, 0xff0000ff, &window) || window == previous_window) return false;
		request.destroy_window.opcode = WSMSG_DESTROY_WINDOW;
		request.destroy_window.window_id = previous_window;
		if (!call(port, &request, sizeof(request.destroy_window), &reply) || reply.status != -NXU_SYS_E_DENIED)
			return false;

		log_message("gui-service: RESTARTED\n");
		pump(1000000);
	}

	log_message("gui-service: BLUE\n");
	pump(4000000);
#if defined(NXU_COREGRAPHICS_TEST)
	if (!desktop_test()) return false;
#endif

	return true;
}

int
main(void)
{
	log_message(exercise() ? "gui-service: PASS\n" : "gui-service: FAILED\n");

	for (;;)
		pump(10000);
}
