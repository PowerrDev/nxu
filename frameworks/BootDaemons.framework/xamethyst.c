#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/thread.h>
#include <nxu/x11proto.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * XAmethyst: X11-protocol-compatible display server. Claims the real
 * display/input exactly like the WindowServer daemon it replaces (see
 * frameworks/BootDaemons.framework/windowserver_service.c for the
 * nxu_display_claim/_recovery_display_info/_recovery_present pattern this
 * reuses unchanged), but speaks the real X11 wire protocol
 * (frameworks/include/nxu/x11proto.h) over kern/ipc/socket.h connections
 * instead of a bespoke message format.
 *
 * v3 (this file): the connection-setup handshake (v1), enough core
 * requests to create, map and fill a window with a solid color (v2) --
 * CreateWindow, DestroyWindow, MapWindow, UnmapWindow, ConfigureWindow
 * (move only), GetGeometry, CreateGC, ChangeGC, PolyFillRectangle,
 * QueryExtension (always "not present") -- and now real input event
 * delivery: a dedicated input-pump thread translates
 * nxu_recovery_input's pointer/key events into KeyPress/KeyRelease/
 * ButtonPress/ButtonRelease/MotionNotify, delivered to whichever mapped
 * window is currently under the pointer (v3 has no focus-window concept
 * yet, so keyboard events use the same pointer-hit-test target). Expose
 * is sent to a window's own connection right after it's mapped. Anything
 * else still gets a generic Error. Compositing is synchronous: whichever
 * request changes visible state repaints the whole shadow framebuffer and
 * presents immediately, under one coarse global lock.
 *
 * One thread per connection (nxu_thread_spawn) plus one input-pump thread
 * and the main/accept thread, matching the target architecture the
 * socket/thread/mmap prerequisite work was built for. Event/reply
 * delivery never happens while holding g_state_lock -- see
 * xamethyst_pointer_target()'s doc comment for why.
 */

#define XAMETHYST_LISTEN_NAME "com.nxu.xamethyst"
#define XAMETHYST_LISTEN_BACKLOG 4U
#define XAMETHYST_MAX_CONNECTIONS 16U
#define XAMETHYST_MAX_WINDOWS 64U
#define XAMETHYST_MAX_GCS 64U
#define XAMETHYST_HANDLER_STACK_SIZE (32ULL * 1024ULL)
#define XAMETHYST_MAX_DIMENSION 4096U
#define XAMETHYST_MAX_REQUEST_BODY (1ULL * 1024ULL * 1024ULL)
#define XAMETHYST_REQUEST_BUFFER_SIZE 4096U

#define XAMETHYST_VENDOR_STRING "NXU"

typedef struct {
	volatile uint32_t value;
} xamethyst_lock_t;

static void
xamethyst_lock(xamethyst_lock_t *lock)
{
	while (__atomic_exchange_n(&lock->value, 1U, __ATOMIC_ACQUIRE) != 0U) (void)nxu_yield();
}

static void
xamethyst_unlock(xamethyst_lock_t *lock)
{
	__atomic_store_n(&lock->value, 0U, __ATOMIC_RELEASE);
}

typedef struct {
	bool in_use;
	int64_t fd;
	uint32_t resource_id_base;
	uint32_t resource_id_mask;
	/* Mirrored by the connection's own handler thread after every request
	 * so the separate input-pump thread can stamp events with the right
	 * sequence number without touching that thread's private state. */
	uint16_t sequence_number;
} xamethyst_connection_t;

typedef struct {
	bool in_use;
	uint32_t id;
	xamethyst_connection_t *owner;
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	bool mapped;
	uint32_t *pixels;
} xamethyst_window_t;

typedef struct {
	bool in_use;
	uint32_t id;
	uint32_t foreground_pixel;
	uint32_t background_pixel;
} xamethyst_gc_t;

static xamethyst_connection_t g_connections[XAMETHYST_MAX_CONNECTIONS];
static xamethyst_window_t g_windows[XAMETHYST_MAX_WINDOWS];
static xamethyst_gc_t g_gcs[XAMETHYST_MAX_GCS];

/* Guards g_windows, g_gcs, g_shadow_framebuffer and every present() call --
 * one coarse lock for v2, matching the "avoid premature optimization"
 * approach the rest of this codebase already takes for new primitives. */
static xamethyst_lock_t g_state_lock;

static uint32_t g_display_width;
static uint32_t g_display_height;
static uint32_t *g_shadow_framebuffer;

static void
xamethyst_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

static bool
xamethyst_read_full(int64_t fd, void *buffer, uint64_t length)
{
	uint8_t *destination = buffer;
	uint64_t received = 0ULL;

	while (received < length) {
		int64_t got = nxu_read((uint64_t)fd, destination + received, length - received);
		if (got <= 0) return false;
		received += (uint64_t)got;
	}

	return true;
}

static bool
xamethyst_write_full(int64_t fd, const void *buffer, uint64_t length)
{
	int64_t written = nxu_write((uint64_t)fd, buffer, length);
	return written == (int64_t)length;
}

static bool
xamethyst_discard(int64_t fd, uint64_t length)
{
	uint8_t scratch[256];

	while (length > 0ULL) {
		uint64_t chunk = length > sizeof(scratch) ? sizeof(scratch) : length;
		if (!xamethyst_read_full(fd, scratch, chunk)) return false;
		length -= chunk;
	}

	return true;
}

static void
xamethyst_send_error(int64_t fd, uint8_t error_code, uint16_t sequence_number, uint32_t bad_value, uint8_t major_opcode, uint16_t minor_opcode)
{
	x11_error_t error;
	memset(&error, 0, sizeof(error));
	error.response_type = 0U;
	error.error_code = error_code;
	error.sequence_number = sequence_number;
	error.bad_value = bad_value;
	error.minor_opcode = minor_opcode;
	error.major_opcode = major_opcode;

	(void)xamethyst_write_full(fd, &error, sizeof(error));
}

/*
 * Reads out the 4-byte slot for want_bit (a single set bit) out of a
 * value-list, following the protocol's convention that only set mask bits
 * consume a slot, in ascending bit order. Returns false (leaving *out_value
 * untouched) if want_bit isn't set in value_mask or the list is too short.
 */
static bool
x11_value_list_get(const uint8_t *value_list, uint64_t value_list_bytes, uint32_t value_mask, uint32_t want_bit, uint32_t *out_value)
{
	if ((value_mask & want_bit) == 0U) return false;

	uint64_t offset = 0ULL;
	for (uint32_t bit = 1U; bit != want_bit; bit <<= 1U) {
		if ((value_mask & bit) != 0U) offset += 4ULL;
	}

	if (offset + 4ULL > value_list_bytes) return false;

	uint32_t value;
	memcpy(&value, value_list + offset, sizeof(value));
	*out_value = value;
	return true;
}

static xamethyst_window_t *
xamethyst_find_window_locked(uint32_t id)
{
	for (uint32_t index = 0U; index < XAMETHYST_MAX_WINDOWS; index++) {
		if (g_windows[index].in_use && g_windows[index].id == id) return &g_windows[index];
	}
	return 0;
}

static xamethyst_window_t *
xamethyst_alloc_window_locked(void)
{
	for (uint32_t index = 0U; index < XAMETHYST_MAX_WINDOWS; index++) {
		if (!g_windows[index].in_use) return &g_windows[index];
	}
	return 0;
}

static xamethyst_gc_t *
xamethyst_find_gc_locked(uint32_t id)
{
	for (uint32_t index = 0U; index < XAMETHYST_MAX_GCS; index++) {
		if (g_gcs[index].in_use && g_gcs[index].id == id) return &g_gcs[index];
	}
	return 0;
}

static xamethyst_gc_t *
xamethyst_alloc_gc_locked(void)
{
	for (uint32_t index = 0U; index < XAMETHYST_MAX_GCS; index++) {
		if (!g_gcs[index].in_use) return &g_gcs[index];
	}
	return 0;
}

/* Repaints the whole shadow framebuffer from scratch (background black,
 * then every mapped window in table order -- no real stacking order yet)
 * and presents it. Caller must already hold g_state_lock. */
static void
xamethyst_composite_locked(void)
{
	uint64_t pixel_count = (uint64_t)g_display_width * (uint64_t)g_display_height;
	for (uint64_t index = 0ULL; index < pixel_count; index++) g_shadow_framebuffer[index] = 0xFF000000U;

	for (uint32_t index = 0U; index < XAMETHYST_MAX_WINDOWS; index++) {
		xamethyst_window_t *window = &g_windows[index];
		if (!window->in_use || !window->mapped) continue;

		for (uint32_t row = 0U; row < window->height; row++) {
			int64_t screen_y = (int64_t)window->y + (int64_t)row;
			if (screen_y < 0 || screen_y >= (int64_t)g_display_height) continue;

			for (uint32_t col = 0U; col < window->width; col++) {
				int64_t screen_x = (int64_t)window->x + (int64_t)col;
				if (screen_x < 0 || screen_x >= (int64_t)g_display_width) continue;

				g_shadow_framebuffer[(uint64_t)screen_y * g_display_width + (uint64_t)screen_x] =
					window->pixels[(uint64_t)row * window->width + col];
			}
		}
	}

	(void)nxu_recovery_present(g_shadow_framebuffer, g_display_width, 0U, 0U, g_display_width, g_display_height);
}

static void
x11_handle_create_window(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_create_window_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_create_window_fixed_t fixed;
	memcpy(&fixed, body, sizeof(fixed));
	const uint8_t *value_list = body + sizeof(fixed);
	uint64_t value_list_bytes = body_bytes - sizeof(fixed);

	if (fixed.width == 0U || fixed.height == 0U || fixed.width > XAMETHYST_MAX_DIMENSION || fixed.height > XAMETHYST_MAX_DIMENSION) {
		xamethyst_send_error(fd, X11_ERROR_VALUE, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	uint32_t background_pixel = 0x00FFFFFFU;
	(void)x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_CW_BACK_PIXEL, &background_pixel);

	xamethyst_lock(&g_state_lock);

	if (xamethyst_find_window_locked(fixed.wid) != 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_ID_CHOICE, sequence_number, fixed.wid, major_opcode, 0U);
		return;
	}

	xamethyst_window_t *window = xamethyst_alloc_window_locked();
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_ALLOC, sequence_number, fixed.wid, major_opcode, 0U);
		return;
	}

	uint64_t pixel_count = (uint64_t)fixed.width * (uint64_t)fixed.height;
	int64_t pixels_va = nxu_mmap(pixel_count * sizeof(uint32_t), NXU_MMAP_PROT_READ_WRITE);
	if (pixels_va <= 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_ALLOC, sequence_number, fixed.wid, major_opcode, 0U);
		return;
	}

	window->in_use = true;
	window->id = fixed.wid;
	window->owner = connection;
	window->x = fixed.x;
	window->y = fixed.y;
	window->width = fixed.width;
	window->height = fixed.height;
	window->mapped = false;
	window->pixels = (uint32_t *)(uintptr_t)pixels_va;

	for (uint64_t index = 0ULL; index < pixel_count; index++) window->pixels[index] = background_pixel;

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_destroy_window(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_window_only_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_window_only_t request;
	memcpy(&request, body, sizeof(request));

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_find_window_locked(request.window);
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_WINDOW, sequence_number, request.window, major_opcode, 0U);
		return;
	}

	bool was_mapped = window->mapped;
	uint64_t pixel_bytes = (uint64_t)window->width * (uint64_t)window->height * sizeof(uint32_t);
	(void)nxu_munmap((uint64_t)(uintptr_t)window->pixels, pixel_bytes);

	*window = (xamethyst_window_t) { 0 };

	if (was_mapped) xamethyst_composite_locked();

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_map_window(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode, bool map)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_window_only_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_window_only_t request;
	memcpy(&request, body, sizeof(request));

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_find_window_locked(request.window);
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_WINDOW, sequence_number, request.window, major_opcode, 0U);
		return;
	}

	window->mapped = map;
	xamethyst_composite_locked();

	uint32_t window_id = window->id;
	uint32_t width = window->width;
	uint32_t height = window->height;

	xamethyst_unlock(&g_state_lock);

	/* Never send while holding g_state_lock -- see
	 * xamethyst_pointer_target()'s doc comment. */
	if (map) {
		x11_expose_event_t expose;
		memset(&expose, 0, sizeof(expose));
		expose.response_type = X11_EVENT_EXPOSE;
		expose.sequence_number = sequence_number;
		expose.window = window_id;
		expose.width = (uint16_t)width;
		expose.height = (uint16_t)height;
		(void)xamethyst_write_full(fd, &expose, sizeof(expose));
	}
}

static void
x11_handle_configure_window(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_configure_window_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_configure_window_fixed_t fixed;
	memcpy(&fixed, body, sizeof(fixed));
	const uint8_t *value_list = body + sizeof(fixed);
	uint64_t value_list_bytes = body_bytes - sizeof(fixed);

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_find_window_locked(fixed.window);
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_WINDOW, sequence_number, fixed.window, major_opcode, 0U);
		return;
	}

	uint32_t raw_x, raw_y;
	if (x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_CONFIG_X, &raw_x)) window->x = (int32_t)(int16_t)raw_x;
	if (x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_CONFIG_Y, &raw_y)) window->y = (int32_t)(int16_t)raw_y;
	/* Width/height reconfiguration would require reallocating the pixel
	 * buffer -- not needed by any v2 test client, left for a future pass. */

	if (window->mapped) xamethyst_composite_locked();

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_get_geometry(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_get_geometry_request_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_get_geometry_request_t request;
	memcpy(&request, body, sizeof(request));

	x11_get_geometry_reply_t reply;
	memset(&reply, 0, sizeof(reply));
	reply.response_type = X11_REPLY;
	reply.sequence_number = sequence_number;
	reply.root = XAMETHYST_ROOT_WINDOW_ID;
	reply.depth = 24U;

	if (request.drawable == XAMETHYST_ROOT_WINDOW_ID) {
		reply.x = 0;
		reply.y = 0;
		reply.width = (uint16_t)g_display_width;
		reply.height = (uint16_t)g_display_height;
		xamethyst_write_full(fd, &reply, sizeof(reply));
		return;
	}

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_find_window_locked(request.drawable);
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_DRAWABLE, sequence_number, request.drawable, major_opcode, 0U);
		return;
	}

	reply.x = (int16_t)window->x;
	reply.y = (int16_t)window->y;
	reply.width = (uint16_t)window->width;
	reply.height = (uint16_t)window->height;

	xamethyst_unlock(&g_state_lock);

	(void)xamethyst_write_full(fd, &reply, sizeof(reply));
}

static void
x11_handle_create_gc(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_create_gc_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_create_gc_fixed_t fixed;
	memcpy(&fixed, body, sizeof(fixed));
	const uint8_t *value_list = body + sizeof(fixed);
	uint64_t value_list_bytes = body_bytes - sizeof(fixed);

	uint32_t foreground = 0x00000000U;
	uint32_t background = 0x00FFFFFFU;
	(void)x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_GC_FOREGROUND, &foreground);
	(void)x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_GC_BACKGROUND, &background);

	xamethyst_lock(&g_state_lock);

	if (xamethyst_find_gc_locked(fixed.cid) != 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_ID_CHOICE, sequence_number, fixed.cid, major_opcode, 0U);
		return;
	}

	xamethyst_gc_t *gc = xamethyst_alloc_gc_locked();
	if (gc == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_ALLOC, sequence_number, fixed.cid, major_opcode, 0U);
		return;
	}

	gc->in_use = true;
	gc->id = fixed.cid;
	gc->foreground_pixel = foreground;
	gc->background_pixel = background;

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_change_gc(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_change_gc_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_change_gc_fixed_t fixed;
	memcpy(&fixed, body, sizeof(fixed));
	const uint8_t *value_list = body + sizeof(fixed);
	uint64_t value_list_bytes = body_bytes - sizeof(fixed);

	xamethyst_lock(&g_state_lock);

	xamethyst_gc_t *gc = xamethyst_find_gc_locked(fixed.gc);
	if (gc == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_VALUE, sequence_number, fixed.gc, major_opcode, 0U);
		return;
	}

	uint32_t value;
	if (x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_GC_FOREGROUND, &value)) gc->foreground_pixel = value;
	if (x11_value_list_get(value_list, value_list_bytes, fixed.value_mask, X11_GC_BACKGROUND, &value)) gc->background_pixel = value;

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_poly_fill_rectangle(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	if (body_bytes < sizeof(x11_poly_fill_rectangle_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_poly_fill_rectangle_fixed_t fixed;
	memcpy(&fixed, body, sizeof(fixed));
	const uint8_t *rectangles = body + sizeof(fixed);
	uint64_t rectangles_bytes = body_bytes - sizeof(fixed);
	uint64_t rectangle_count = rectangles_bytes / sizeof(x11_rectangle_t);

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_find_window_locked(fixed.drawable);
	if (window == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_DRAWABLE, sequence_number, fixed.drawable, major_opcode, 0U);
		return;
	}

	xamethyst_gc_t *gc = xamethyst_find_gc_locked(fixed.gc);
	if (gc == 0) {
		xamethyst_unlock(&g_state_lock);
		xamethyst_send_error(fd, X11_ERROR_VALUE, sequence_number, fixed.gc, major_opcode, 0U);
		return;
	}

	for (uint64_t index = 0ULL; index < rectangle_count; index++) {
		x11_rectangle_t rectangle;
		memcpy(&rectangle, rectangles + index * sizeof(rectangle), sizeof(rectangle));

		for (int32_t row = 0; row < (int32_t)rectangle.height; row++) {
			int32_t window_y = (int32_t)rectangle.y + row;
			if (window_y < 0 || (uint32_t)window_y >= window->height) continue;

			for (int32_t col = 0; col < (int32_t)rectangle.width; col++) {
				int32_t window_x = (int32_t)rectangle.x + col;
				if (window_x < 0 || (uint32_t)window_x >= window->width) continue;

				window->pixels[(uint64_t)window_y * window->width + (uint64_t)window_x] = gc->foreground_pixel;
			}
		}
	}

	if (window->mapped) xamethyst_composite_locked();

	xamethyst_unlock(&g_state_lock);
}

static void
x11_handle_query_extension(xamethyst_connection_t *connection, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number, uint8_t major_opcode)
{
	int64_t fd = connection->fd;

	(void)body;

	if (body_bytes < sizeof(x11_query_extension_fixed_t)) {
		xamethyst_send_error(fd, X11_ERROR_REQUEST, sequence_number, 0U, major_opcode, 0U);
		return;
	}

	x11_query_extension_reply_t reply;
	memset(&reply, 0, sizeof(reply));
	reply.response_type = X11_REPLY;
	reply.sequence_number = sequence_number;
	reply.present = 0U; /* XAmethyst v2 implements no extensions. */

	(void)xamethyst_write_full(fd, &reply, sizeof(reply));
}

static void
xamethyst_dispatch_request(xamethyst_connection_t *connection, const x11_request_header_t *header, const uint8_t *body, uint64_t body_bytes, uint16_t sequence_number)
{
	switch (header->major_opcode) {
	case X11_OP_CREATE_WINDOW:
		x11_handle_create_window(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_DESTROY_WINDOW:
		x11_handle_destroy_window(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_MAP_WINDOW:
		x11_handle_map_window(connection, body, body_bytes, sequence_number, header->major_opcode, true);
		break;
	case X11_OP_UNMAP_WINDOW:
		x11_handle_map_window(connection, body, body_bytes, sequence_number, header->major_opcode, false);
		break;
	case X11_OP_CONFIGURE_WINDOW:
		x11_handle_configure_window(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_GET_GEOMETRY:
		x11_handle_get_geometry(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_CREATE_GC:
		x11_handle_create_gc(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_CHANGE_GC:
		x11_handle_change_gc(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_POLY_FILL_RECTANGLE:
		x11_handle_poly_fill_rectangle(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	case X11_OP_QUERY_EXTENSION:
		x11_handle_query_extension(connection, body, body_bytes, sequence_number, header->major_opcode);
		break;
	default:
		xamethyst_send_error(connection->fd, X11_ERROR_REQUEST, sequence_number, 0U, header->major_opcode, 0U);
		break;
	}
}

/*
 * Builds and sends the Setup success reply: one screen, one TrueColor
 * visual and one pixmap format matching g_shadow_framebuffer's 32bpp
 * 0xAARRGGBB layout (see frameworks/BootDaemons.framework/
 * windowserver_service.c / about_sevos_service.c for the same pixel
 * convention already in use elsewhere in this codebase).
 */
static bool
xamethyst_send_setup_success(int64_t fd, uint32_t resource_id_base, uint32_t resource_id_mask)
{
	uint8_t buffer[256];
	uint64_t offset = 0ULL;

	uint64_t vendor_length = sizeof(XAMETHYST_VENDOR_STRING) - 1ULL;
	uint64_t vendor_padded = (vendor_length + 3ULL) & ~(uint64_t)3U;

	x11_format_t format;
	memset(&format, 0, sizeof(format));
	format.depth = 24U;
	format.bits_per_pixel = 32U;
	format.scanline_pad = 32U;

	x11_visualtype_t visual;
	memset(&visual, 0, sizeof(visual));
	visual.visual_id = XAMETHYST_ROOT_VISUAL_ID;
	visual.class = X11_VISUAL_CLASS_TRUE_COLOR;
	visual.bits_per_rgb_value = 8U;
	visual.colormap_entries = 256U;
	visual.red_mask = 0x00FF0000U;
	visual.green_mask = 0x0000FF00U;
	visual.blue_mask = 0x000000FFU;

	x11_depth_t depth;
	memset(&depth, 0, sizeof(depth));
	depth.depth = 24U;
	depth.visuals_length = 1U;

	x11_screen_t screen;
	memset(&screen, 0, sizeof(screen));
	screen.root = XAMETHYST_ROOT_WINDOW_ID;
	screen.default_colormap = XAMETHYST_ROOT_COLORMAP_ID;
	screen.white_pixel = 0x00FFFFFFU;
	screen.black_pixel = 0x00000000U;
	screen.current_input_masks = 0U;
	screen.pixel_width = (uint16_t)g_display_width;
	screen.pixel_height = (uint16_t)g_display_height;
	screen.millimeter_width = 0U;
	screen.millimeter_height = 0U;
	screen.min_installed_maps = 1U;
	screen.max_installed_maps = 1U;
	screen.root_visual = XAMETHYST_ROOT_VISUAL_ID;
	screen.backing_stores = X11_BACKING_STORE_NEVER;
	screen.save_unders = 0U;
	screen.root_depth = 24U;
	screen.allowed_depths_length = 1U;

	x11_setup_success_t success;
	memset(&success, 0, sizeof(success));
	success.release_number = 0U;
	success.resource_id_base = resource_id_base;
	success.resource_id_mask = resource_id_mask;
	success.motion_buffer_size = 0U;
	success.vendor_length = (uint16_t)vendor_length;
	success.maximum_request_length = 0xFFFFU;
	success.roots_length = 1U;
	success.pixmap_formats_length = 1U;
	success.image_byte_order = 0U;
	success.bitmap_bit_order = 0U;
	success.bitmap_format_scanline_unit = 32U;
	success.bitmap_format_scanline_pad = 32U;
	success.min_keycode = 8U;
	success.max_keycode = 255U;

	uint64_t body_length =
		(uint64_t)sizeof(success) +
		vendor_padded +
		(uint64_t)sizeof(format) +
		(uint64_t)sizeof(screen) +
		(uint64_t)sizeof(depth) +
		(uint64_t)sizeof(visual);

	x11_setup_prefix_t prefix;
	memset(&prefix, 0, sizeof(prefix));
	prefix.success = X11_SETUP_SUCCESS;
	prefix.protocol_major_version = X11_PROTOCOL_MAJOR_VERSION;
	prefix.protocol_minor_version = X11_PROTOCOL_MINOR_VERSION;
	prefix.length = (uint16_t)(body_length / 4ULL);

	memcpy(&buffer[offset], &prefix, sizeof(prefix));
	offset += sizeof(prefix);
	memcpy(&buffer[offset], &success, sizeof(success));
	offset += sizeof(success);
	memcpy(&buffer[offset], XAMETHYST_VENDOR_STRING, vendor_length);
	offset += vendor_length;
	for (uint64_t pad_index = vendor_length; pad_index < vendor_padded; pad_index++) buffer[offset++] = 0U;
	memcpy(&buffer[offset], &format, sizeof(format));
	offset += sizeof(format);
	memcpy(&buffer[offset], &screen, sizeof(screen));
	offset += sizeof(screen);
	memcpy(&buffer[offset], &depth, sizeof(depth));
	offset += sizeof(depth);
	memcpy(&buffer[offset], &visual, sizeof(visual));
	offset += sizeof(visual);

	return xamethyst_write_full(fd, buffer, offset);
}

/*
 * Reads the client's setup request, drains any auth name/data (v2 ignores
 * authorization entirely -- every connection is trusted), and rejects
 * anything but little-endian clients. Our own future client library and
 * every plausible test client on this ARM64 target will be little-endian;
 * supporting big-endian clients too is future work, not needed yet.
 */
static bool
xamethyst_handle_setup(int64_t fd, uint32_t resource_id_base, uint32_t resource_id_mask)
{
	x11_setup_request_t request;
	if (!xamethyst_read_full(fd, &request, sizeof(request))) return false;

	uint64_t auth_total = (uint64_t)request.authorization_protocol_name_length + (uint64_t)request.authorization_protocol_data_length;
	uint64_t auth_padded = (auth_total + 3ULL) & ~(uint64_t)3U;
	if (!xamethyst_discard(fd, auth_padded)) return false;

	if (request.byte_order != X11_BYTE_ORDER_LSB_FIRST) return false;

	return xamethyst_send_setup_success(fd, resource_id_base, resource_id_mask);
}

static void
xamethyst_connection_thread(void *argument)
{
	xamethyst_connection_t *connection = argument;

	if (!xamethyst_handle_setup(connection->fd, connection->resource_id_base, connection->resource_id_mask)) {
		(void)nxu_close((uint64_t)connection->fd);
		connection->in_use = false;
		(void)nxu_thread_exit(0ULL);
		return;
	}

	uint16_t sequence_number = 0U;
	uint8_t body[XAMETHYST_REQUEST_BUFFER_SIZE];

	for (;;) {
		x11_request_header_t header;
		if (!xamethyst_read_full(connection->fd, &header, sizeof(header))) break;

		sequence_number++;
		connection->sequence_number = sequence_number;

		uint64_t total_bytes = (uint64_t)header.length * 4ULL;
		uint64_t body_bytes = total_bytes > sizeof(header) ? total_bytes - sizeof(header) : 0ULL;
		if (body_bytes > XAMETHYST_MAX_REQUEST_BODY) break;

		if (body_bytes > sizeof(body)) {
			/* Too large for anything v2 implements -- drain it and answer
			 * with a generic error rather than desyncing the stream. */
			if (!xamethyst_discard(connection->fd, body_bytes)) break;
			xamethyst_send_error(connection->fd, X11_ERROR_REQUEST, sequence_number, 0U, header.major_opcode, 0U);
			continue;
		}

		if (!xamethyst_read_full(connection->fd, body, body_bytes)) break;

		xamethyst_dispatch_request(connection, &header, body, body_bytes, sequence_number);
	}

	(void)nxu_close((uint64_t)connection->fd);
	connection->in_use = false;
	(void)nxu_thread_exit(0ULL);
}

/* Mirrors drivers/input/mouse.h's MOUSE_BUTTON_* and drivers/input/
 * keyboard.h's KEYBOARD_MOD_* bit layouts -- neither is available to
 * userland builds, so both are duplicated here rather than pulled in
 * (frameworks/BootDaemons.framework/windowserver_service.c does the same
 * for the button bits already). */
#define XAMETHYST_INPUT_BUTTON_LEFT 0x01U
#define XAMETHYST_INPUT_BUTTON_RIGHT 0x02U
#define XAMETHYST_INPUT_BUTTON_MIDDLE 0x04U
#define XAMETHYST_MOD_SHIFT 0x01U
#define XAMETHYST_MOD_CTRL 0x02U
#define XAMETHYST_MOD_ALT 0x04U
#define XAMETHYST_MOD_CAPS 0x08U

/* kern/syscall/syscall.c's syscall_recovery_input passes through the raw
 * evdev value: 0 = release, 1 = press, 2 = repeat. */
#define XAMETHYST_KEY_RELEASE 0U

static int32_t g_pointer_x;
static int32_t g_pointer_y;
static int32_t g_pointer_raw_x;
static int32_t g_pointer_raw_y;
static bool g_pointer_baseline;
static uint32_t g_pointer_buttons;
static uint32_t g_keyboard_modifiers;

static int64_t g_listen_descriptor;

static int32_t
xamethyst_clamp_pointer(int64_t value, uint32_t extent)
{
	if (extent == 0U || value < 0) return 0;
	if ((uint64_t)value >= (uint64_t)extent) return (int32_t)(extent - 1U);
	return (int32_t)value;
}

/* Caller must already hold g_state_lock. Iterates back-to-front so a
 * window created later (and therefore painted later/"on top" by
 * xamethyst_composite_locked's own table-order paint) wins a hit-test
 * against an older, now-overlapped window -- there is no separate
 * z-order list yet, table order doubles as stacking order. */
static xamethyst_window_t *
xamethyst_hit_test_locked(int32_t x, int32_t y)
{
	for (uint32_t index = XAMETHYST_MAX_WINDOWS; index > 0U; index--) {
		xamethyst_window_t *window = &g_windows[index - 1U];
		if (!window->in_use || !window->mapped) continue;
		if (x < window->x || x >= window->x + (int32_t)window->width) continue;
		if (y < window->y || y >= window->y + (int32_t)window->height) continue;
		return window;
	}
	return 0;
}

typedef struct {
	bool valid;
	int64_t fd;
	uint16_t sequence_number;
	uint32_t window_id;
	int32_t window_x;
	int32_t window_y;
} xamethyst_event_target_t;

/*
 * Finds whichever mapped window the pointer currently sits over and
 * copies out exactly what's needed to build/deliver an event, then drops
 * g_state_lock before returning. Delivery (nxu_write) always happens
 * without the lock held -- writing to a socket blocks if the client isn't
 * draining its end, and blocking while holding this process's one global
 * lock would freeze every other connection's requests along with it.
 *
 * v3 has no separate keyboard-focus window yet, so key events reuse this
 * same pointer-hit-test target -- a "focus follows mouse" simplification,
 * not real X11 focus semantics (SetInputFocus isn't implemented).
 */
static xamethyst_event_target_t
xamethyst_pointer_target(void)
{
	xamethyst_event_target_t target;
	memset(&target, 0, sizeof(target));

	xamethyst_lock(&g_state_lock);

	xamethyst_window_t *window = xamethyst_hit_test_locked(g_pointer_x, g_pointer_y);
	if (window != 0 && window->owner != 0) {
		target.valid = true;
		target.fd = window->owner->fd;
		target.sequence_number = window->owner->sequence_number;
		target.window_id = window->id;
		target.window_x = window->x;
		target.window_y = window->y;
	}

	xamethyst_unlock(&g_state_lock);

	return target;
}

static uint16_t
xamethyst_translate_state(uint32_t modifiers, uint32_t buttons)
{
	/* Real X11 KeyButMask bit assignments (X.h): Shift=1, Lock=2,
	 * Control=4, Mod1=8, Button1=0x100, Button2=0x200, Button3=0x400. */
	uint16_t state = 0U;
	if ((modifiers & XAMETHYST_MOD_SHIFT) != 0U) state |= 0x0001U;
	if ((modifiers & XAMETHYST_MOD_CAPS) != 0U) state |= 0x0002U;
	if ((modifiers & XAMETHYST_MOD_CTRL) != 0U) state |= 0x0004U;
	if ((modifiers & XAMETHYST_MOD_ALT) != 0U) state |= 0x0008U;
	if ((buttons & XAMETHYST_INPUT_BUTTON_LEFT) != 0U) state |= 0x0100U;
	if ((buttons & XAMETHYST_INPUT_BUTTON_MIDDLE) != 0U) state |= 0x0200U;
	if ((buttons & XAMETHYST_INPUT_BUTTON_RIGHT) != 0U) state |= 0x0400U;
	return state;
}

static void
xamethyst_fill_input_event(x11_input_event_t *event, uint8_t response_type, uint8_t detail, const xamethyst_event_target_t *target)
{
	memset(event, 0, sizeof(*event));
	event->response_type = response_type;
	event->detail = detail;
	event->sequence_number = target->sequence_number;
	event->root = XAMETHYST_ROOT_WINDOW_ID;
	event->event = target->window_id;
	event->root_x = (int16_t)g_pointer_x;
	event->root_y = (int16_t)g_pointer_y;
	event->event_x = (int16_t)(g_pointer_x - target->window_x);
	event->event_y = (int16_t)(g_pointer_y - target->window_y);
	event->state = xamethyst_translate_state(g_keyboard_modifiers, g_pointer_buttons);
	event->same_screen = 1U;
}

static void
xamethyst_send_motion(void)
{
	xamethyst_event_target_t target = xamethyst_pointer_target();
	if (!target.valid) return;

	x11_input_event_t event;
	xamethyst_fill_input_event(&event, (uint8_t)X11_EVENT_MOTION_NOTIFY, 0U, &target);
	(void)xamethyst_write_full(target.fd, &event, sizeof(event));
}

static void
xamethyst_send_button(uint8_t button_number, bool pressed)
{
	xamethyst_event_target_t target = xamethyst_pointer_target();
	if (!target.valid) return;

	x11_input_event_t event;
	xamethyst_fill_input_event(&event, pressed ? (uint8_t)X11_EVENT_BUTTON_PRESS : (uint8_t)X11_EVENT_BUTTON_RELEASE, button_number, &target);
	(void)xamethyst_write_full(target.fd, &event, sizeof(event));
}

static void
xamethyst_send_key(uint8_t x11_keycode, bool pressed)
{
	xamethyst_event_target_t target = xamethyst_pointer_target();
	if (!target.valid) return;

	x11_input_event_t event;
	xamethyst_fill_input_event(&event, pressed ? (uint8_t)X11_EVENT_KEY_PRESS : (uint8_t)X11_EVENT_KEY_RELEASE, x11_keycode, &target);
	(void)xamethyst_write_full(target.fd, &event, sizeof(event));
}

/*
 * Drains every already-normalized input event queued since the last poll
 * (nxu_recovery_input hands back one event per call, coalesced kernel-side
 * from the raw VirtIO packets) and turns it into X11 events. Pointer
 * tracking (absolute position from a relative accumulator, per-button
 * edge detection) mirrors windowserver_service.c's windowserver_pump_input
 * exactly, since it's solving the same problem against the same syscall.
 *
 * Key codes: nxu_recovery_input reports raw Linux evdev keycodes: this
 * adds 8, the standard evdev-to-X11-keycode offset (matching the
 * min_keycode=8 XAmethyst already advertises in its Setup reply), for
 * genuine wire-level compatibility rather than an arbitrary local mapping.
 */
static void
xamethyst_pump_input(void)
{
	nxu_recovery_input_event_t event;
	int64_t received;

	while ((received = nxu_recovery_input(&event)) > 0) {
		if (event.kind == NXU_RECOVERY_INPUT_POINTER) {
			if (!g_pointer_baseline) {
				g_pointer_raw_x = event.x;
				g_pointer_raw_y = event.y;
				g_pointer_baseline = true;
			}

			int64_t next_x = (int64_t)g_pointer_x + ((int64_t)event.x - g_pointer_raw_x);
			int64_t next_y = (int64_t)g_pointer_y + ((int64_t)event.y - g_pointer_raw_y);
			g_pointer_raw_x = event.x;
			g_pointer_raw_y = event.y;
			g_pointer_x = xamethyst_clamp_pointer(next_x, g_display_width);
			g_pointer_y = xamethyst_clamp_pointer(next_y, g_display_height);

			xamethyst_send_motion();

			uint32_t previous_buttons = g_pointer_buttons;
			uint32_t changed = previous_buttons ^ event.buttons;
			if ((changed & XAMETHYST_INPUT_BUTTON_LEFT) != 0U) xamethyst_send_button(1U, (event.buttons & XAMETHYST_INPUT_BUTTON_LEFT) != 0U);
			if ((changed & XAMETHYST_INPUT_BUTTON_MIDDLE) != 0U) xamethyst_send_button(2U, (event.buttons & XAMETHYST_INPUT_BUTTON_MIDDLE) != 0U);
			if ((changed & XAMETHYST_INPUT_BUTTON_RIGHT) != 0U) xamethyst_send_button(3U, (event.buttons & XAMETHYST_INPUT_BUTTON_RIGHT) != 0U);
			g_pointer_buttons = event.buttons;
		} else if (event.kind == NXU_RECOVERY_INPUT_KEY) {
			g_keyboard_modifiers = event.modifiers;

			uint8_t x11_keycode = (uint8_t)(event.code + 8U);
			bool pressed = event.value != XAMETHYST_KEY_RELEASE;
			xamethyst_send_key(x11_keycode, pressed);
		}
	}
}

static void
xamethyst_accept_thread(void *argument)
{
	(void)argument;

	for (;;) {
		int64_t connection_fd = nxu_socket_accept((uint64_t)g_listen_descriptor);
		if (connection_fd < 0) continue;

		uint32_t slot = XAMETHYST_MAX_CONNECTIONS;
		for (uint32_t index = 0U; index < XAMETHYST_MAX_CONNECTIONS; index++) {
			if (!g_connections[index].in_use) {
				slot = index;
				break;
			}
		}

		if (slot == XAMETHYST_MAX_CONNECTIONS) {
			(void)nxu_close((uint64_t)connection_fd);
			continue;
		}

		g_connections[slot].in_use = true;
		g_connections[slot].fd = connection_fd;
		g_connections[slot].resource_id_base = (slot + 1U) << 22;
		g_connections[slot].resource_id_mask = 0x003FFFFFU;
		g_connections[slot].sequence_number = 0U;

		if (nxu_thread_spawn(xamethyst_connection_thread, &g_connections[slot], XAMETHYST_HANDLER_STACK_SIZE) <= 0) {
			(void)nxu_close((uint64_t)connection_fd);
			g_connections[slot].in_use = false;
		}
	}
}

int
main(void)
{
	if (nxu_display_claim() != 0) {
		xamethyst_log("xamethyst: display already claimed, exiting\n");
		return 1;
	}

	nxu_recovery_display_info_t info;
	if (nxu_recovery_display_info(&info) != 0 || info.width == 0U || info.height == 0U) {
		xamethyst_log("xamethyst: display info unavailable\n");
		return 2;
	}
	if (info.width > XAMETHYST_MAX_DIMENSION || info.height > XAMETHYST_MAX_DIMENSION) {
		xamethyst_log("xamethyst: display too large for the shadow framebuffer\n");
		return 3;
	}

	g_display_width = info.width;
	g_display_height = info.height;

	uint64_t pixel_count = (uint64_t)g_display_width * (uint64_t)g_display_height;
	int64_t framebuffer_va = nxu_mmap(pixel_count * sizeof(uint32_t), NXU_MMAP_PROT_READ_WRITE);
	if (framebuffer_va <= 0) {
		xamethyst_log("xamethyst: shadow framebuffer allocation failed\n");
		return 4;
	}
	g_shadow_framebuffer = (uint32_t *)(uintptr_t)framebuffer_va;

	for (uint64_t index = 0ULL; index < pixel_count; index++) g_shadow_framebuffer[index] = 0xFF000000U;
	(void)nxu_recovery_present(g_shadow_framebuffer, g_display_width, 0U, 0U, g_display_width, g_display_height);

	g_listen_descriptor = nxu_socket_listen(XAMETHYST_LISTEN_NAME, XAMETHYST_LISTEN_BACKLOG);
	if (g_listen_descriptor < 0) {
		xamethyst_log("xamethyst: socket listen failed\n");
		return 5;
	}

	xamethyst_log("xamethyst: listening as " XAMETHYST_LISTEN_NAME "\n");

	if (nxu_thread_spawn(xamethyst_accept_thread, 0, XAMETHYST_HANDLER_STACK_SIZE) <= 0) {
		xamethyst_log("xamethyst: accept thread spawn failed\n");
		return 6;
	}

	g_pointer_x = (int32_t)(g_display_width / 2U);
	g_pointer_y = (int32_t)(g_display_height / 2U);

	/* The main thread becomes the input pump for the rest of this
	 * process's life -- nxu_recovery_input is non-blocking, so this loop
	 * yields between polls instead of spinning. */
	for (;;) {
		xamethyst_pump_input();
		(void)nxu_yield();
	}
}
