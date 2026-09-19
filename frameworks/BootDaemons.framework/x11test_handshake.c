#include <nxu/string.h>
#include <nxu/syscall.h>
#include <nxu/x11proto.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Throwaway raw-protocol X11 test, spawned directly by
 * kern/tests/xamethyst_process_test.c. Speaks the wire bytes directly
 * (nxu_socket_connect + hand-built requests) rather than through any
 * client library, to validate frameworks/BootDaemons.framework/xamethyst.c
 * in isolation before any client library exists to build against.
 *
 * Covers: the connection-setup handshake, the request loop staying in
 * sync after an unimplemented opcode, and (v2) CreateWindow + MapWindow +
 * CreateGC + PolyFillRectangle landing a solid-colored window on screen,
 * confirmed both structurally (this file, via a GetGeometry round trip)
 * and visually (by inspecting the QEMU display once this test passes).
 */

#define XAMETHYST_LISTEN_NAME "com.nxu.xamethyst"
#define X11TEST_CONNECT_ATTEMPTS 2000000U
#define X11TEST_WINDOW_ID 0x00000010U
#define X11TEST_GC_ID 0x00000011U
#define X11TEST_WINDOW_X 100
#define X11TEST_WINDOW_Y 80
#define X11TEST_WINDOW_WIDTH 200U
#define X11TEST_WINDOW_HEIGHT 150U
#define X11TEST_FILL_COLOR 0x00007FFFU

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

int
main(void)
{
	int64_t fd = -1;
	for (uint32_t attempt = 0U; attempt < X11TEST_CONNECT_ATTEMPTS; attempt++) {
		fd = nxu_socket_connect(XAMETHYST_LISTEN_NAME);
		if (fd >= 0) break;
		(void)nxu_yield();
	}
	if (fd < 0) return 1;

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
	if (prefix.protocol_major_version != X11_PROTOCOL_MAJOR_VERSION) return 5;
	if (prefix.protocol_minor_version != X11_PROTOCOL_MINOR_VERSION) return 6;

	uint64_t body_bytes = (uint64_t)prefix.length * 4ULL;

	x11_setup_success_t success;
	if (body_bytes < sizeof(success)) return 7;
	if (!x11test_read_full(fd, &success, sizeof(success))) return 8;
	body_bytes -= sizeof(success);

	if (success.roots_length != 1U) return 9;
	if (success.pixmap_formats_length != 1U) return 10;
	if (success.resource_id_mask == 0U) return 11;
	if ((success.resource_id_base & success.resource_id_mask) != 0U) return 12;

	uint64_t vendor_padded = ((uint64_t)success.vendor_length + 3ULL) & ~(uint64_t)3U;
	if (body_bytes < vendor_padded) return 13;

	char vendor[64];
	if (success.vendor_length >= sizeof(vendor)) return 14;
	if (!x11test_read_full(fd, vendor, vendor_padded)) return 15;
	body_bytes -= vendor_padded;
	vendor[success.vendor_length] = '\0';

	if (vendor[0] != 'N' || vendor[1] != 'X' || vendor[2] != 'U') return 16;

	/* Drain the FORMAT/SCREEN/DEPTH/VISUALTYPE tail -- already validated
	 * structurally by the fact prefix.length was self-consistent above. */
	uint8_t scratch[256];
	while (body_bytes > 0ULL) {
		uint64_t chunk = body_bytes > sizeof(scratch) ? sizeof(scratch) : body_bytes;
		if (!x11test_read_full(fd, scratch, chunk)) return 17;
		body_bytes -= chunk;
	}

	/* Send one bogus request (an opcode nothing implements) and confirm
	 * the server replies with a generic Error carrying the right sequence
	 * number and echoed major opcode, proving the request loop stays
	 * correctly in sync after the handshake. */
	x11_request_header_t bogus_request;
	bogus_request.major_opcode = 200U;
	bogus_request.data = 0U;
	bogus_request.length = 1U;
	if (nxu_write((uint64_t)fd, &bogus_request, sizeof(bogus_request)) != (int64_t)sizeof(bogus_request)) return 18;

	x11_error_t error;
	if (!x11test_read_full(fd, &error, sizeof(error))) return 19;
	if (error.response_type != 0U) return 20;
	if (error.error_code != X11_ERROR_REQUEST) return 21;
	if (error.sequence_number != 1U) return 22;
	if (error.major_opcode != 200U) return 23;

	uint16_t expected_sequence = 1U;

	/* CreateWindow */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_CREATE_WINDOW;
		header.data = 24U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_create_window_fixed_t)) / 4U);

		x11_create_window_fixed_t body;
		body.wid = X11TEST_WINDOW_ID;
		body.parent = XAMETHYST_ROOT_WINDOW_ID;
		body.x = X11TEST_WINDOW_X;
		body.y = X11TEST_WINDOW_Y;
		body.width = X11TEST_WINDOW_WIDTH;
		body.height = X11TEST_WINDOW_HEIGHT;
		body.border_width = 0U;
		body.class = 1U;
		body.visual = 0U;
		body.value_mask = 0U;

		if (!x11test_send(fd, &header, sizeof(header), &body, sizeof(body))) return 24;
		expected_sequence++;
	}

	/* MapWindow */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_MAP_WINDOW;
		header.data = 0U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_window_only_t)) / 4U);

		x11_window_only_t body;
		body.window = X11TEST_WINDOW_ID;

		if (!x11test_send(fd, &header, sizeof(header), &body, sizeof(body))) return 25;
		expected_sequence++;
	}

	/* MapWindow always emits an Expose right after -- drain it before
	 * sending anything else, or it'll be mistaken for the next reply. */
	{
		uint8_t expose[32];
		if (!x11test_read_full(fd, expose, sizeof(expose))) return 34;
		if (expose[0] != (uint8_t)X11_EVENT_EXPOSE) return 35;
	}

	/* CreateGC with a distinctive foreground color */
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
		uint32_t foreground = X11TEST_FILL_COLOR;
		memcpy(body, &fixed, sizeof(fixed));
		memcpy(body + sizeof(fixed), &foreground, sizeof(foreground));

		if (!x11test_send(fd, &header, sizeof(header), body, sizeof(body))) return 26;
		expected_sequence++;
	}

	/* PolyFillRectangle covering the whole window */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_POLY_FILL_RECTANGLE;
		header.data = 0U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_poly_fill_rectangle_fixed_t) + sizeof(x11_rectangle_t)) / 4U);

		x11_poly_fill_rectangle_fixed_t fixed;
		fixed.drawable = X11TEST_WINDOW_ID;
		fixed.gc = X11TEST_GC_ID;

		x11_rectangle_t rectangle;
		rectangle.x = 0;
		rectangle.y = 0;
		rectangle.width = (uint16_t)X11TEST_WINDOW_WIDTH;
		rectangle.height = (uint16_t)X11TEST_WINDOW_HEIGHT;

		uint8_t body[sizeof(fixed) + sizeof(rectangle)];
		memcpy(body, &fixed, sizeof(fixed));
		memcpy(body + sizeof(fixed), &rectangle, sizeof(rectangle));

		if (!x11test_send(fd, &header, sizeof(header), body, sizeof(body))) return 27;
		expected_sequence++;
	}

	/* GetGeometry -- the reply is our synchronization point: since one
	 * connection's requests are processed strictly in order, its arrival
	 * proves every request sent above already landed. */
	{
		x11_request_header_t header;
		header.major_opcode = (uint8_t)X11_OP_GET_GEOMETRY;
		header.data = 0U;
		header.length = (uint16_t)((sizeof(header) + sizeof(x11_get_geometry_request_t)) / 4U);

		x11_get_geometry_request_t body;
		body.drawable = X11TEST_WINDOW_ID;

		if (!x11test_send(fd, &header, sizeof(header), &body, sizeof(body))) return 28;
		expected_sequence++;
	}

	x11_get_geometry_reply_t geometry_reply;
	if (!x11test_read_full(fd, &geometry_reply, sizeof(geometry_reply))) return 29;
	if (geometry_reply.response_type != X11_REPLY) return 30;
	if (geometry_reply.sequence_number != expected_sequence) return 31;
	if (geometry_reply.x != X11TEST_WINDOW_X || geometry_reply.y != X11TEST_WINDOW_Y) return 32;
	if (geometry_reply.width != X11TEST_WINDOW_WIDTH || geometry_reply.height != X11TEST_WINDOW_HEIGHT) return 33;

	(void)nxu_close((uint64_t)fd);
	return 0;
}
