#ifndef NXU_GUI_HOST_PROTOCOL_H
#define NXU_GUI_HOST_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <kern/syscall/ui_session_defs.h>

/* Process-boundary ABI. Minor 0 implements lease negotiation. Minor 1 adds
 * copied desktop-session records; SURFACE/DAMAGE/INPUT remain unadvertised.
 * Independent of the in-process graphics callback ABI v5. All records are
 * little endian; handles are scoped to the authenticated session epoch. */
#define NXU_GUI_HOST_MAJOR 1U
#define NXU_GUI_HOST_MINOR 1U
#define NXU_GUI_HOST_MAX_RECORD 4096U
#define NXU_GUI_HOST_FEATURE_SURFACE (1ULL << 0)
#define NXU_GUI_HOST_FEATURE_DAMAGE (1ULL << 1)
#define NXU_GUI_HOST_FEATURE_INPUT (1ULL << 2)
#define NXU_GUI_HOST_FEATURE_SESSION (1ULL << 3)
#define NXU_GUI_HOST_FEATURE_LEASE (1ULL << 4)

enum nxu_gui_host_operation {
	NXU_GUI_HOST_HELLO = 1,
	NXU_GUI_HOST_DISPLAY_ATTACH,
	NXU_GUI_HOST_PRESENT,
	NXU_GUI_HOST_INPUT_RECEIVE,
	NXU_GUI_HOST_SESSION_BEGIN,
	NXU_GUI_HOST_ACCEPT,
	NXU_GUI_HOST_DESCRIBE,
	NXU_GUI_HOST_FRAME_ACQUIRE,
	NXU_GUI_HOST_FRAME_RELEASE,
	NXU_GUI_HOST_EVENT_POST,
	NXU_GUI_HOST_CONNECTION_CLOSE,
	NXU_GUI_HOST_LAUNCH_TAKE,
	NXU_GUI_HOST_ACTIVATION_TAKE,
	NXU_GUI_HOST_RELEASE,
	NXU_GUI_HOST_STATE,
	NXU_GUI_HOST_TIME
};

typedef struct {
	uint32_t size;
	uint16_t major, minor;
	uint32_t operation, flags;
	uint64_t session_epoch, request_id;
} nxu_gui_host_header_t;

typedef struct {
	nxu_gui_host_header_t header;
	uint64_t required_features, supported_features;
	uint32_t max_record_size, reserved;
} nxu_gui_host_hello_t;

typedef struct {
	nxu_gui_host_header_t header;
	uint64_t enabled_features;
	int32_t status;
	uint32_t max_record_size;
} nxu_gui_host_hello_reply_t;

/* A process-local SHM mapping is obtained separately with existing syscalls.
 * The handle is an opaque generation-tagged host object, never a pointer. */
typedef struct {
	nxu_gui_host_header_t header;
	uint64_t surface_handle;
	uint32_t width, height, stride_pixels, pixel_format;
	uint64_t byte_offset, byte_length;
	uint32_t access_flags, reserved;
} nxu_gui_host_surface_t;

/* Copy-based desktop bridge. Frame addresses refer only to the caller's memory.
 * Connection handles include a generation and are scoped to session_epoch. */
typedef struct {
	nxu_gui_host_header_t header;
	uint64_t connection, sequence;
	int64_t result;
	uint64_t pixels, byte_length;
	uint32_t stride, width, height, scale_permille, menubar_height, reserved;

	union {
		nxu_ui_connect_t connect;
		nxu_ui_submit_t submit;
		nxu_ui_message_t message;
		char path[NXU_UI_PATH_MAX];
	} payload;
} nxu_gui_host_desktop_t;

_Static_assert(sizeof(nxu_gui_host_desktop_t) <= NXU_GUI_HOST_MAX_RECORD, "GUI desktop record limit");

_Static_assert(sizeof(nxu_gui_host_header_t) == 32, "GUI header layout");
_Static_assert(offsetof(nxu_gui_host_header_t, session_epoch) == 16, "GUI epoch offset");
_Static_assert(sizeof(nxu_gui_host_hello_t) == 56, "GUI hello layout");
_Static_assert(sizeof(nxu_gui_host_hello_reply_t) == 48, "GUI hello reply layout");
_Static_assert(sizeof(nxu_gui_host_surface_t) == 80, "GUI surface layout");
_Static_assert(offsetof(nxu_gui_host_surface_t, byte_offset) == 56, "GUI surface offset");

#endif
