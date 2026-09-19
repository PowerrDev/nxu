#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/x11proto.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway input-delivery visualization, spawned (fire-and-forget,
 * alongside x11test_handshake) by kern/tests/xamethyst_process_test.c.
 * Creates a full-screen window filled red, then waits for real input
 * events XAmethyst's input-pump thread translates from
 * nxu_recovery_input: a ButtonPress repaints the window green, a KeyPress
 * repaints it blue. Verified visually (QEMU screendump before/after
 * injecting a synthetic click/keypress via the QEMU monitor) rather than
 * by exit code -- unlike every other test this session, input delivery
 * needs a real external stimulus, so it doesn't fit the spawn/wait/check-
 * exit-status pattern the rest of this codebase's tests use. Never exits
 * on its own, like windowserver_service.c.
 */

#define XAMETHYST_LISTEN_NAME "com.nxu.xamethyst"
#define X11TEST_CONNECT_ATTEMPTS 2000000U
#define X11TEST_WINDOW_ID 0x00000030U
#define X11TEST_GC_ID 0x00000031U

#define X11TEST_COLOR_INITIAL 0x00FF0000U /* red */
#define X11TEST_COLOR_BUTTON 0x0000FF00U /* green */
#define X11TEST_COLOR_KEY 0x000000FFU /* blue */

static bool
x11test_read_full(int64_t fd, void *buffer, uint64_t length)
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
x11test_send(int64_t fd, const void *header, uint64_t header_size, const void *body, uint64_t body_size)
{
	uint8_t request[sizeof(x11_request_header_t) + 32U];
	if (header_size + body_size > sizeof(request)) return false;

	memcpy(request, header, header_size);
	if (body_size != 0ULL) memcpy(request + header_size, body, body_size);

	return nxu_write((uint64_t)fd, request, header_size + body_size) == (int64_t)(header_size + body_size);
}

static void
x11test_fill(int64_t fd, uint32_t color)
{
	x11_request_header_t header;
	header.major_opcode = (uint8_t)X11_OP_CHANGE_GC;
	header.data = 0U;
	header.length = (uint16_t)((sizeof(header) + sizeof(x11_change_gc_fixed_t) + 4U) / 4U);

	x11_change_gc_fixed_t fixed;
	fixed.gc = X11TEST_GC_ID;
	fixed.value_mask = X11_GC_FOREGROUND;

	uint8_t change_body[sizeof(fixed) + 4U];
	memcpy(change_body, &fixed, sizeof(fixed));
	memcpy(change_body + sizeof(fixed), &color, sizeof(color));

	(void)x11test_send(fd, &header, sizeof(header), change_body, sizeof(change_body));

	x11_request_header_t fill_header;
	fill_header.major_opcode = (uint8_t)X11_OP_POLY_FILL_RECTANGLE;
	fill_header.data = 0U;
	fill_header.length = (uint16_t)((sizeof(fill_header) + sizeof(x11_poly_fill_rectangle_fixed_t) + sizeof(x11_rectangle_t)) / 4U);

	x11_poly_fill_rectangle_fixed_t fill_fixed;
	fill_fixed.drawable = X11TEST_WINDOW_ID;
	fill_fixed.gc = X11TEST_GC_ID;

	x11_rectangle_t rectangle;
	rectangle.x = 0;
	rectangle.y = 0;
	rectangle.width = 4096U; /* the server clips to the window's own bounds */
	rectangle.height = 4096U;

	uint8_t fill_body[sizeof(fill_fixed) + sizeof(rectangle)];
	memcpy(fill_body, &fill_fixed, sizeof(fill_fixed));
	memcpy(fill_body + sizeof(fill_fixed), &rectangle, sizeof(rectangle));

	(void)x11test_send(fd, &fill_header, sizeof(fill_header), fill_body, sizeof(fill_body));
}

static void
x11test_log(const char *message)
{
	(void)nxu_write(1U, message, nxu_strlen(message));
}

int
main(void)
{
	x11test_log("xws_main: main started\n");

	int64_t fd = -1;
	for (uint32_t attempt = 0U; attempt < X11TEST_CONNECT_ATTEMPTS; attempt++) {
		fd = nxu_socket_connect(XAMETHYST_LISTEN_NAME);
		if (fd >= 0) break;
		(void)nxu_yield();
	}
	if (fd < 0) return 1;
	x11test_log("xws_test_input: connected\n");

	x11_setup_request_t setup_request;
	setup_request.byte_order = X11_BYTE_ORDER_LSB_FIRST;
	setup_request.pad1 = 0U;
	setup_request.protocol_major_version = X11_PROTOCOL_MAJOR_VERSION;
	setup_request.protocol_minor_version = X11_PROTOCOL_MINOR_VERSION;
	setup_request.authorization_protocol_name_length = 0U;
	setup_request.authorization_protocol_data_length = 0U;
	setup_request.pad2 = 0U;

	if (nxu_write((uint64_t)fd, &setup_request, sizeof(setup_request)) != (int64_t)sizeof(setup_request)) return 2;

	x11_setup_prefix_t prefix;
	if (!x11test_read_full(fd, &prefix, sizeof(prefix))) return 3;
	if (prefix.success != X11_SETUP_SUCCESS) return 4;

	uint64_t body_bytes = (uint64_t)prefix.length * 4ULL;

	x11_setup_success_t success;
	if (body_bytes < sizeof(success) || !x11test_read_full(fd, &success, sizeof(success))) return 5;
	body_bytes -= sizeof(success);

	uint64_t vendor_padded = ((uint64_t)success.vendor_length + 3ULL) & ~(uint64_t)3U;
	uint8_t scratch[256];
	if (body_bytes < vendor_padded || !x11test_read_full(fd, scratch, vendor_padded)) return 6;
	body_bytes -= vendor_padded;

	x11_format_t format;
	if (body_bytes < sizeof(format) || !x11test_read_full(fd, &format, sizeof(format))) return 7;
	body_bytes -= sizeof(format);

	x11_screen_t screen;
	if (body_bytes < sizeof(screen) || !x11test_read_full(fd, &screen, sizeof(screen))) return 8;
	body_bytes -= sizeof(screen);

	/* Drain the DEPTH/VISUALTYPE tail -- not needed by this test. */
	while (body_bytes > 0ULL) {
		uint64_t chunk = body_bytes > sizeof(scratch) ? sizeof(scratch) : body_bytes;
		if (!x11test_read_full(fd, scratch, chunk)) return 9;
		body_bytes -= chunk;
	}

	x11test_log("xws_test_input: setup parsed, screen=");
	{
		char buf[32];
		uint32_t w = screen.pixel_width, h = screen.pixel_height;
		uint32_t i = 0;
		if (w == 0) buf[i++] = '0'; else { char tmp[10]; uint32_t n=0; while (w) { tmp[n++]=(char)('0'+w%10); w/=10; } while (n) buf[i++]=tmp[--n]; }
		buf[i++]='x';
		if (h == 0) buf[i++] = '0'; else { char tmp[10]; uint32_t n=0; while (h) { tmp[n++]=(char)('0'+h%10); h/=10; } while (n) buf[i++]=tmp[--n]; }
		buf[i++]='\n'; buf[i]='\0';
		x11test_log(buf);
	}

	/* CreateWindow covering the whole screen, filled red, so the pointer's
	 * default starting position (screen center, see xamethyst.c's main())
	 * is guaranteed to already be inside it -- no pointer movement needed
	 * to exercise hit-testing, just a click/keypress. */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_CREATE_WINDOW;
		header.data = 24U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_create_window_fixed_t) + 4U) / 4U);

		x11_create_window_fixed_t body;
		body.wid = X11TEST_WINDOW_ID;
		body.parent = XAMETHYST_ROOT_WINDOW_ID;
		body.x = 0;
		body.y = 0;
		body.width = screen.pixel_width;
		body.height = screen.pixel_height;
		body.border_width = 0U;
		body.class = 1U;
		body.visual = 0U;
		body.value_mask = X11_CW_BACK_PIXEL;

		uint8_t request_body[sizeof(body) + 4U];
		uint32_t initial_color = X11TEST_COLOR_INITIAL;
		memcpy(request_body, &body, sizeof(body));
		memcpy(request_body + sizeof(body), &initial_color, sizeof(initial_color));

		if (!x11test_send(fd, &header, sizeof(header), request_body, sizeof(request_body))) return 10;
	}
	x11test_log("xws_test_input: CreateWindow sent\n");

	/* MapWindow */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_MAP_WINDOW;
		header.data = 0U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_window_only_t)) / 4U);

		x11_window_only_t body;
		body.window = X11TEST_WINDOW_ID;

		if (!x11test_send(fd, &header, sizeof(header), &body, sizeof(body))) return 11;
	}

	x11test_log("xws_test_input: MapWindow sent, waiting for Expose\n");

	/* MapWindow always emits an Expose right after -- drain it before
	 * entering the input event loop. */
	uint8_t expose[32];
	if (!x11test_read_full(fd, expose, sizeof(expose))) return 12;
	{
		char buf[64];
		buf[0] = 'x'; buf[1]='1'; buf[2]='1'; buf[3]='t'; buf[4]=':'; buf[5]=' ';
		buf[6] = (char)('0' + (expose[0] / 10U));
		buf[7] = (char)('0' + (expose[0] % 10U));
		buf[8] = '\n'; buf[9] = '\0';
		x11test_log("xws_test_input: got expose byte0=");
		x11test_log(buf + 6);
	}

	/* CreateGC, ready for the color-swap fills below. */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_CREATE_GC;
		header.data = 0U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_create_gc_fixed_t) + 4U) / 4U);

		x11_create_gc_fixed_t fixed;
		fixed.cid = X11TEST_GC_ID;
		fixed.drawable = X11TEST_WINDOW_ID;
		fixed.value_mask = X11_GC_FOREGROUND;

		uint8_t body[sizeof(fixed) + 4U];
		uint32_t foreground = X11TEST_COLOR_INITIAL;
		memcpy(body, &fixed, sizeof(fixed));
		memcpy(body + sizeof(fixed), &foreground, sizeof(foreground));

		if (!x11test_send(fd, &header, sizeof(header), body, sizeof(body))) return 13;
	}

	for (;;) {
		uint8_t raw_event[32];
		if (!x11test_read_full(fd, raw_event, sizeof(raw_event))) break;

		uint8_t response_type = raw_event[0];
		if (response_type == (uint8_t)X11_EVENT_BUTTON_PRESS) {
			x11test_fill(fd, X11TEST_COLOR_BUTTON);
		} else if (response_type == (uint8_t)X11_EVENT_KEY_PRESS) {
			x11test_fill(fd, X11TEST_COLOR_KEY);
		}
		/* Motion, release, and anything else are ignored. */
	}

	return 0;
}
