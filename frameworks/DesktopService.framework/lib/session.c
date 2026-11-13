/* Copy-based desktop adapter. The kernel's pointers never cross this ABI. */
#include <nxu/gui_host_protocol.h>
#include <nxu/runtime.h>
#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/bootstrap_protocol.h>
#include <nxu/services.h>

#define CONNECTION_MAX 8U

static uint64_t g_epoch;
static uint64_t g_request_id;
static uint16_t g_minor;
static bool g_started;
static int64_t g_last_status;
static uint64_t g_connections[CONNECTION_MAX];
static nxu_ui_connect_t g_information[CONNECTION_MAX];

void *
cg_host_alloc(size_t size)
{
	return nxu_alloc(size, 16);
}

bool
cg_host_free(void *address)
{
	return nxu_free(address);
}

void
cg_host_log(const char *message)
{
	if (!message) return;

	uint64_t length = 0;
	while (length < 256 && message[length])
		length++;

	if (length) (void)nxu_write(1, message, length);
	(void)nxu_write(1, "\n", 1);
}

static bool
exchange(nxu_gui_host_desktop_t *record, uint32_t operation, uint32_t connection)
{
	if (!g_epoch || g_request_id == UINT64_MAX) return false;

	record->header = (nxu_gui_host_header_t){
		sizeof(*record), NXU_GUI_HOST_MAJOR, g_minor, operation, 0, g_epoch, ++g_request_id,
	};

	if (connection < CONNECTION_MAX) record->connection = g_connections[connection];

	g_last_status = nxu_gui_host(record, sizeof(*record));
	return g_last_status == 0;
}

void
cg_host_session_begin(uint32_t scale, uint32_t width, uint32_t height, uint32_t menubar)
{
	if (!g_epoch) {
		union {
			nxu_gui_host_hello_t request;
			nxu_gui_host_hello_reply_t reply;
		} hello = {0};

		hello.request.header = (nxu_gui_host_header_t){
			sizeof(hello.request), NXU_GUI_HOST_MAJOR, NXU_GUI_HOST_MINOR, NXU_GUI_HOST_HELLO, 0, 0, 1,
		};
		hello.request.required_features = hello.request.supported_features = NXU_GUI_HOST_FEATURE_SESSION;
		hello.request.max_record_size = NXU_GUI_HOST_MAX_RECORD;

		if (nxu_gui_host(&hello, sizeof(hello))) return;

		g_epoch = hello.reply.header.session_epoch;
		g_minor = hello.reply.header.minor;
		g_request_id = 1;
	}

	nxu_gui_host_desktop_t record = {0};
	record.width = width;
	record.height = height;
	record.scale_permille = scale;
	record.menubar_height = menubar;
	g_started = exchange(&record, NXU_GUI_HOST_SESSION_BEGIN, CONNECTION_MAX) && record.result;
}

int32_t
cg_host_accept(void)
{
	nxu_gui_host_desktop_t record = {0};

	if (!exchange(&record, NXU_GUI_HOST_ACCEPT, CONNECTION_MAX) || !record.result) return -1;

	uint32_t index = (uint32_t)record.connection - 1;

	if (index >= CONNECTION_MAX) return -1;

	g_connections[index] = record.connection;
	return (int32_t)index;
}

const nxu_ui_connect_t *
cg_host_info(uint32_t connection)
{
	nxu_gui_host_desktop_t record = {0};

	if (connection >= CONNECTION_MAX || !exchange(&record, NXU_GUI_HOST_DESCRIBE, connection)) return 0;

	g_information[connection] = record.payload.connect;
	return &g_information[connection];
}

uint32_t
cg_host_pid(uint32_t connection)
{
	nxu_gui_host_desktop_t record = {0};
	return exchange(&record, NXU_GUI_HOST_DESCRIBE, connection) ? (uint32_t)record.sequence : 0;
}

bool
cg_host_alive(uint32_t connection)
{
	nxu_gui_host_desktop_t record = {0};
	if (exchange(&record, NXU_GUI_HOST_DESCRIBE, connection)) return record.result;

	/* A trusted overlay temporarily excludes all desktop host operations.
	 * Exclusion is not evidence that the app exited. */
	return g_connections[connection] && g_last_status == -NXU_SYS_E_DENIED;
}

bool
cg_host_post(uint32_t connection, const nxu_ui_message_t *message)
{
	if (!message) return false;

	nxu_gui_host_desktop_t record = {0};
	record.payload.message = *message;
	return exchange(&record, NXU_GUI_HOST_EVENT_POST, connection) && record.result;
}

bool
cg_host_state(uint32_t connection, nxu_ui_submit_t *state, uint64_t *sequence)
{
	if (!state || !sequence) return false;

	nxu_gui_host_desktop_t record = {0};
	record.sequence = *sequence;

	if (!exchange(&record, NXU_GUI_HOST_STATE, connection)) return false;

	*state = record.payload.submit;
	*sequence = record.sequence;
	return record.result != 0;
}

bool
cg_host_take_frame(uint32_t connection, uint32_t *destination, uint32_t stride, uint32_t width, uint32_t height,
				   uint32_t *width_out, uint32_t *height_out)
{
	if (!destination || !height || !width || stride < width) return false;

	nxu_gui_host_desktop_t record = {0};
	record.pixels = (uint64_t)(uintptr_t)destination;
	record.byte_length = ((uint64_t)(height - 1) * stride + width) * sizeof(uint32_t);
	record.stride = stride;
	record.width = width;
	record.height = height;

	if (!exchange(&record, NXU_GUI_HOST_FRAME_ACQUIRE, connection) || !record.result) return false;

	if (width_out) *width_out = record.width;

	if (height_out) *height_out = record.height;

	return true;
}

bool
cg_host_take_launch(char *path)
{
	nxu_gui_host_desktop_t record = {0};

	if (!path || !exchange(&record, NXU_GUI_HOST_LAUNCH_TAKE, CONNECTION_MAX) || !record.result) return false;

	memcpy(path, record.payload.path, NXU_UI_PATH_MAX);
	return true;
}

uint32_t
cg_host_take_activation(void)
{
	nxu_gui_host_desktop_t record = {0};
	return exchange(&record, NXU_GUI_HOST_ACTIVATION_TAKE, CONNECTION_MAX) ? (uint32_t)record.result : 0;
}

void
cg_host_terminate(uint32_t connection)
{
	nxu_gui_host_desktop_t record = {0};
	record.sequence = 1;
	(void)exchange(&record, NXU_GUI_HOST_CONNECTION_CLOSE, connection);
}

void
cg_host_release(uint32_t connection)
{
	nxu_gui_host_desktop_t record = {0};
	(void)exchange(&record, NXU_GUI_HOST_CONNECTION_CLOSE, connection);

	if (connection < CONNECTION_MAX) g_connections[connection] = 0;
}

/*
 * Read the file at path (a whole-volume path, /Volumes/mainOS/...) into
 * buffer, at most capacity bytes. Returns the file's size -- with capacity 0
 * (and no buffer), only that -- or a negative error. The desktop reads the
 * wallpaper preferences and the picture they name with this.
 */
int64_t
cg_host_read_file(const char *path, void *buffer, size_t capacity)
{
	nxu_stat_t info;

	if (!path) return -1;

	int64_t status = nxu_stat(path, &info);

	if (status < 0) return status;
	if (!capacity || !buffer) return (int64_t)info.size;

	int64_t descriptor = nxu_open(path, NXU_O_READ);

	if (descriptor < 0) return descriptor;

	uint64_t wanted = info.size < capacity ? info.size : capacity, filled = 0;

	while (filled < wanted) {
		int64_t count = nxu_read((uint64_t)descriptor, (uint8_t *)buffer + filled, wanted - filled);

		if (count <= 0) break;
		filled += (uint64_t)count;
	}
	(void)nxu_close((uint64_t)descriptor);
	return filled == wanted ? (int64_t)info.size : -1;
}

/*
 * Whether any preferences changed since the last call. The desktop reads its
 * preference files (wallpaper, clock, time zone) only when this says so, not
 * on a timer: preferencesd sends a notice to a port the desktop gave it
 * (NXU_PREFERENCES_WATCH). Until preferencesd answers, every call says yes
 * at most every couple of seconds, so nothing is missed while it starts.
 * One non-blocking receive when there is nothing to say.
 */
#define WATCH_RETRY_US 2000000LL

static uint32_t g_watch_port;
static int64_t g_watch_attempt_us;

static uint32_t
preferences_lookup(void)
{
	int64_t bootstrap = nxu_ipc_bootstrap_port();
	int64_t reply = nxu_ipc_port_allocate();
	nxu_bootstrap_request_t request = {0};
	nxu_bootstrap_reply_t answer = {0};
	uint32_t service = 0, type = 0;

	if (bootstrap <= 0 || reply <= 0) return 0;

	request.opcode = NXU_BOOTSTRAP_OP_LOOKUP;
	memcpy(request.label, NXU_PREFERENCES_SERVICE, sizeof(NXU_PREFERENCES_SERVICE));
	if (nxu_ipc_send((uint32_t)bootstrap, &request, sizeof(request), (uint32_t)reply) == 0 &&
		nxu_ipc_receive_timed((uint32_t)reply, &answer, sizeof(answer), &service, &type, 200000U) >= 0 &&
		answer.status != NXU_BOOTSTRAP_STATUS_OK) {
		service = 0;
	}
	(void)nxu_ipc_port_deallocate((uint32_t)reply);
	return service;
}

bool
cg_host_preferences_changed(void)
{
	static nxu_service_message_t message; /* a notice, or the watch's answer */
	uint32_t transfer = 0, type = 0;
	bool changed = false;

	if (g_watch_port == 0) {
		int64_t now = nxu_uptime_us();

		if (g_watch_attempt_us && now - g_watch_attempt_us < WATCH_RETRY_US) return false;
		g_watch_attempt_us = now;

		uint32_t service = preferences_lookup();
		int64_t port = service ? nxu_ipc_port_allocate() : 0;

		if (port > 0) {
			memset(&message, 0, sizeof(message));
			message.version = NXU_SERVICES_VERSION;
			message.operation = NXU_PREFERENCES_WATCH;
			if (nxu_ipc_send(service, &message, sizeof(message), (uint32_t)port) == 0) g_watch_port = (uint32_t)port;
			else (void)nxu_ipc_port_deallocate((uint32_t)port);
		}
		/* Not watching yet: look at the files anyway, now and then. */
		return true;
	}

	while (nxu_ipc_receive(g_watch_port, &message, sizeof(message), &transfer, &type) >= 0) changed = true;
	return changed;
}

/* Put an app's process in a QoS class: the desktop raises the app in front. */
bool
cg_host_set_qos(uint32_t pid, uint32_t qos)
{
	return nxu_set_qos(pid, qos) == 0;
}

bool
DesktopServiceBeginSession(uint32_t scale, uint32_t width, uint32_t height, uint32_t menubar)
{
	cg_host_session_begin(scale, width, height, menubar);
	return g_started;
}

bool
DesktopServiceGetTime(uint64_t *seconds)
{
	nxu_gui_host_desktop_t record = {0};

	if (!seconds || !exchange(&record, NXU_GUI_HOST_TIME, CONNECTION_MAX) || !record.result) return false;

	*seconds = record.sequence;
	return true;
}

void
DesktopServiceShutdown(void)
{
	if (!g_epoch) return;

	union {
		nxu_gui_host_header_t request;
		nxu_gui_host_hello_reply_t reply;
	} record = {0};

	record.request = (nxu_gui_host_header_t){
		sizeof(record.request), NXU_GUI_HOST_MAJOR, g_minor, NXU_GUI_HOST_RELEASE, 0, g_epoch, ++g_request_id,
	};
	(void)nxu_gui_host(&record, sizeof(record));
	g_epoch = 0;
	g_started = false;
	memset(g_connections, 0, sizeof(g_connections));
}
