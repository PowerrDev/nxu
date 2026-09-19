#ifndef NXU_USER_WINDOWSERVER_PROTOCOL_H
#define NXU_USER_WINDOWSERVER_PROTOCOL_H

#include <stdint.h>

/*
 * Wire format for the standalone WindowServer process (see
 * frameworks/BootDaemons.framework/windowserver_service.c) -- a small
 * tagged-union request per message, one reply per request except
 * WSMSG_PRESENT (fire-and-forget, matching the old in-kernel WS_Present's
 * own internal frame-pacing/throttling). Every request except WSMSG_PRESENT
 * carries the caller's own reply port transferred along with it
 * (NXU_IPC_XFER_PORT), the same one-shot-reply-port pattern
 * frameworks/CoreFoundation.framework/lib/service/bootstrap_client.c uses.
 *
 * Pixel payloads travel one of two ways: WSMSG_RENDER_WINDOW carries them
 * inline, bounded by WSMSG_MAX_INLINE_PIXELS so a request always fits
 * IPC_KMSG_MAX_INLINE_SIZE (kern/ipc/ipc_types.h) -- fine for small test
 * windows. A real app's full-size frame instead calls nxu_shm_create/_map
 * (kern/ipc/shm_registry.h) itself, writes pixels directly into the mapped
 * region, and sends WSMSG_RENDER_WINDOW_SHM carrying just the shm id --
 * WindowServer maps the same id (nxu_shm_map is safe to call from more than
 * one process; each gets its own mapping of the same underlying pages) and
 * reads the pixels from there instead of the message body.
 */

#define WS_BOOTSTRAP_LABEL "com.nxu.windowserver"

#define WSMSG_MAX_INLINE_PIXELS 512U

typedef enum {
	WSMSG_CREATE_WINDOW = 1,
	WSMSG_RENDER_WINDOW = 2,
	WSMSG_PRESENT = 3,
	WSMSG_RENDER_WINDOW_SHM = 4
} wsmsg_opcode_t;

typedef struct {
	uint32_t opcode;
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
} wsmsg_create_window_t;

typedef struct {
	uint32_t opcode;
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t pixel_count;
	uint32_t pixels[WSMSG_MAX_INLINE_PIXELS];
} wsmsg_render_window_t;

typedef struct {
	uint32_t opcode;
} wsmsg_present_t;

typedef struct {
	uint32_t opcode;
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t shm_id;
} wsmsg_render_window_shm_t;

typedef union {
	uint32_t opcode;
	wsmsg_create_window_t create_window;
	wsmsg_render_window_t render_window;
	wsmsg_present_t present;
	wsmsg_render_window_shm_t render_window_shm;
} wsmsg_request_t;

typedef struct {
	int32_t status;
	uint32_t window_id;
} wsmsg_reply_t;

#endif
