/* Copy-based desktop adapter. The kernel's pointers never cross this ABI. */
#include <nxu/gui_host_protocol.h>
#include <nxu/runtime.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

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
