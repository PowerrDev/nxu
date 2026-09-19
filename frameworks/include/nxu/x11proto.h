#ifndef NXU_X11PROTO_H
#define NXU_X11PROTO_H

#include <stdint.h>

/*
 * X11 core protocol wire structures, hand-written against the X.Org
 * protocol specification (x11protocol.html, "Connection Setup" and
 * "Common Types" sections) -- not a generated or vendored header. Field
 * order and sizes are exact; every struct is packed and size-asserted so a
 * layout mistake fails the build instead of corrupting the wire.
 */

#define X11_BYTE_ORDER_LSB_FIRST 0x6CU /* ASCII 'l' -- little-endian, the only order XAmethyst v1 accepts */
#define X11_BYTE_ORDER_MSB_FIRST 0x42U /* ASCII 'B' -- big-endian, rejected in v1 */

#define X11_PROTOCOL_MAJOR_VERSION 11U
#define X11_PROTOCOL_MINOR_VERSION 0U

#define X11_SETUP_FAILED 0U
#define X11_SETUP_SUCCESS 1U
#define X11_SETUP_AUTHENTICATE 2U

/* Client's initial connection-setup request, before any auth name/data. */
typedef struct __attribute__((packed)) {
	uint8_t byte_order;
	uint8_t pad1;
	uint16_t protocol_major_version;
	uint16_t protocol_minor_version;
	uint16_t authorization_protocol_name_length;
	uint16_t authorization_protocol_data_length;
	uint16_t pad2;
} x11_setup_request_t;
_Static_assert(sizeof(x11_setup_request_t) == 12U, "x11_setup_request_t size");

/* First 8 bytes of every setup response, regardless of success/failure. */
typedef struct __attribute__((packed)) {
	uint8_t success;
	uint8_t pad_or_reason_length;
	uint16_t protocol_major_version;
	uint16_t protocol_minor_version;
	uint16_t length; /* in 4-byte units, beyond this 8-byte prefix */
} x11_setup_prefix_t;
_Static_assert(sizeof(x11_setup_prefix_t) == 8U, "x11_setup_prefix_t size");

/* Follows x11_setup_prefix_t when success == X11_SETUP_SUCCESS. */
typedef struct __attribute__((packed)) {
	uint32_t release_number;
	uint32_t resource_id_base;
	uint32_t resource_id_mask;
	uint32_t motion_buffer_size;
	uint16_t vendor_length;
	uint16_t maximum_request_length;
	uint8_t roots_length; /* number of SCREENs */
	uint8_t pixmap_formats_length; /* number of FORMATs */
	uint8_t image_byte_order;
	uint8_t bitmap_bit_order;
	uint8_t bitmap_format_scanline_unit;
	uint8_t bitmap_format_scanline_pad;
	uint8_t min_keycode;
	uint8_t max_keycode;
	uint32_t pad2;
} x11_setup_success_t;
_Static_assert(sizeof(x11_setup_success_t) == 32U, "x11_setup_success_t size");

/* One PIXMAP FORMAT entry, pixmap_formats_length of these follow the vendor
 * string (padded to a 4-byte boundary). */
typedef struct __attribute__((packed)) {
	uint8_t depth;
	uint8_t bits_per_pixel;
	uint8_t scanline_pad;
	uint8_t pad[5];
} x11_format_t;
_Static_assert(sizeof(x11_format_t) == 8U, "x11_format_t size");

/* One VISUALTYPE entry, following a DEPTH header. */
typedef struct __attribute__((packed)) {
	uint32_t visual_id;
	uint8_t class;
	uint8_t bits_per_rgb_value;
	uint16_t colormap_entries;
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	uint32_t pad;
} x11_visualtype_t;
_Static_assert(sizeof(x11_visualtype_t) == 24U, "x11_visualtype_t size");

#define X11_VISUAL_CLASS_TRUE_COLOR 4U

/* One DEPTH entry: this header, then visuals_length x11_visualtype_t. */
typedef struct __attribute__((packed)) {
	uint8_t depth;
	uint8_t pad0;
	uint16_t visuals_length;
	uint32_t pad1;
} x11_depth_t;
_Static_assert(sizeof(x11_depth_t) == 8U, "x11_depth_t size");

/* One SCREEN entry: this header, then allowed_depths_length x11_depth_t
 * (each followed by its own visuals). */
typedef struct __attribute__((packed)) {
	uint32_t root;
	uint32_t default_colormap;
	uint32_t white_pixel;
	uint32_t black_pixel;
	uint32_t current_input_masks;
	uint16_t pixel_width;
	uint16_t pixel_height;
	uint16_t millimeter_width;
	uint16_t millimeter_height;
	uint16_t min_installed_maps;
	uint16_t max_installed_maps;
	uint32_t root_visual;
	uint8_t backing_stores;
	uint8_t save_unders;
	uint8_t root_depth;
	uint8_t allowed_depths_length;
} x11_screen_t;
_Static_assert(sizeof(x11_screen_t) == 40U, "x11_screen_t size");

#define X11_BACKING_STORE_NEVER 0U

/* XAmethyst's single well-known root window/visual/colormap id -- the same
 * for every connection (unlike per-connection resource ids), so both
 * server and client code need it. */
#define XAMETHYST_ROOT_WINDOW_ID 0x00000001U
#define XAMETHYST_ROOT_VISUAL_ID 0x00000021U
#define XAMETHYST_ROOT_COLORMAP_ID 0x00000002U

/* Generic 4-byte request header every core/extension request starts with. */
typedef struct __attribute__((packed)) {
	uint8_t major_opcode;
	uint8_t data;
	uint16_t length; /* in 4-byte units, includes this header */
} x11_request_header_t;
_Static_assert(sizeof(x11_request_header_t) == 4U, "x11_request_header_t size");

/* Generic 32-byte reply header; reply-specific fields fill the rest. */
typedef struct __attribute__((packed)) {
	uint8_t response_type; /* 1 */
	uint8_t data;
	uint16_t sequence_number;
	uint32_t length; /* in 4-byte units, beyond this 32-byte header */
} x11_reply_header_t;
_Static_assert(sizeof(x11_reply_header_t) == 8U, "x11_reply_header_t size");

#define X11_REPLY 1U

/* Generic 32-byte error. */
typedef struct __attribute__((packed)) {
	uint8_t response_type; /* 0 */
	uint8_t error_code;
	uint16_t sequence_number;
	uint32_t bad_value;
	uint16_t minor_opcode;
	uint8_t major_opcode;
	uint8_t pad[21];
} x11_error_t;
_Static_assert(sizeof(x11_error_t) == 32U, "x11_error_t size");

#define X11_ERROR_REQUEST 1U
#define X11_ERROR_VALUE 2U
#define X11_ERROR_WINDOW 3U
#define X11_ERROR_DRAWABLE 9U
#define X11_ERROR_ACCESS 10U
#define X11_ERROR_ALLOC 11U
#define X11_ERROR_ID_CHOICE 14U
#define X11_ERROR_IMPLEMENTATION 17U

/* Generic 32-byte event; event-specific fields fill the rest. */
typedef struct __attribute__((packed)) {
	uint8_t response_type; /* event code, 2-127 */
	uint8_t detail;
	uint16_t sequence_number;
	uint8_t data[28];
} x11_generic_event_t;
_Static_assert(sizeof(x11_generic_event_t) == 32U, "x11_generic_event_t size");

/* Core opcodes XAmethyst implements or explicitly special-cases; anything
 * else gets X11_ERROR_REQUEST. */
#define X11_OP_CREATE_WINDOW 1U
#define X11_OP_DESTROY_WINDOW 4U
#define X11_OP_MAP_WINDOW 8U
#define X11_OP_UNMAP_WINDOW 10U
#define X11_OP_CONFIGURE_WINDOW 12U
#define X11_OP_GET_GEOMETRY 14U
#define X11_OP_CREATE_GC 55U
#define X11_OP_CHANGE_GC 56U
#define X11_OP_POLY_FILL_RECTANGLE 70U
#define X11_OP_QUERY_EXTENSION 98U

/* CreateWindow/ChangeWindowAttributes value-mask bits XAmethyst v1 reads
 * out of the value-list (everything else in the mask is accepted but
 * ignored). */
#define X11_CW_BACK_PIXEL 0x00000002U

/* ConfigureWindow value-mask bits XAmethyst v1 acts on. */
#define X11_CONFIG_X 0x00000001U
#define X11_CONFIG_Y 0x00000002U

/* CreateGC/ChangeGC value-mask bits XAmethyst v1 reads. */
#define X11_GC_FOREGROUND 0x00000004U
#define X11_GC_BACKGROUND 0x00000008U

/*
 * Fixed-size prefix of a CreateWindow request body (after the 4-byte
 * header, whose data byte holds depth): wid, parent, x, y, width, height,
 * border-width, class, visual, value-mask -- 28 bytes, followed by one
 * 4-byte slot per set value-mask bit, low bit first.
 */
typedef struct __attribute__((packed)) {
	uint32_t wid;
	uint32_t parent;
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border_width;
	uint16_t class;
	uint32_t visual;
	uint32_t value_mask;
} x11_create_window_fixed_t;
_Static_assert(sizeof(x11_create_window_fixed_t) == 28U, "x11_create_window_fixed_t size");

/* DestroyWindow/MapWindow/UnmapWindow body: just the target window. */
typedef struct __attribute__((packed)) {
	uint32_t window;
} x11_window_only_t;
_Static_assert(sizeof(x11_window_only_t) == 4U, "x11_window_only_t size");

/* Fixed prefix of a ConfigureWindow request body: window, value-mask, then
 * one 4-byte slot per set bit (X and Y are 16-bit values in a 4-byte slot,
 * per the protocol's INT16-in-a-CARD32-slot convention for this request). */
typedef struct __attribute__((packed)) {
	uint32_t window;
	uint16_t value_mask;
	uint16_t pad;
} x11_configure_window_fixed_t;
_Static_assert(sizeof(x11_configure_window_fixed_t) == 8U, "x11_configure_window_fixed_t size");

/* GetGeometry request body: just the target drawable. */
typedef struct __attribute__((packed)) {
	uint32_t drawable;
} x11_get_geometry_request_t;
_Static_assert(sizeof(x11_get_geometry_request_t) == 4U, "x11_get_geometry_request_t size");

typedef struct __attribute__((packed)) {
	uint8_t response_type;
	uint8_t depth;
	uint16_t sequence_number;
	uint32_t length; /* 0 */
	uint32_t root;
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t border_width;
	uint8_t pad[10];
} x11_get_geometry_reply_t;
_Static_assert(sizeof(x11_get_geometry_reply_t) == 32U, "x11_get_geometry_reply_t size");

/* Fixed prefix of a CreateGC request body: cid, drawable, value-mask, then
 * one 4-byte slot per set bit. */
typedef struct __attribute__((packed)) {
	uint32_t cid;
	uint32_t drawable;
	uint32_t value_mask;
} x11_create_gc_fixed_t;
_Static_assert(sizeof(x11_create_gc_fixed_t) == 12U, "x11_create_gc_fixed_t size");

/* Fixed prefix of a ChangeGC request body: gc, value-mask, then one 4-byte
 * slot per set bit. */
typedef struct __attribute__((packed)) {
	uint32_t gc;
	uint32_t value_mask;
} x11_change_gc_fixed_t;
_Static_assert(sizeof(x11_change_gc_fixed_t) == 8U, "x11_change_gc_fixed_t size");

/* Fixed prefix of a PolyFillRectangle request body: drawable, gc, then one
 * RECTANGLE (8 bytes: x, y, width, height) per rectangle. */
typedef struct __attribute__((packed)) {
	uint32_t drawable;
	uint32_t gc;
} x11_poly_fill_rectangle_fixed_t;
_Static_assert(sizeof(x11_poly_fill_rectangle_fixed_t) == 8U, "x11_poly_fill_rectangle_fixed_t size");

typedef struct __attribute__((packed)) {
	int16_t x;
	int16_t y;
	uint16_t width;
	uint16_t height;
} x11_rectangle_t;
_Static_assert(sizeof(x11_rectangle_t) == 8U, "x11_rectangle_t size");

/* QueryExtension request body: name-length, then the name padded to 4. */
typedef struct __attribute__((packed)) {
	uint16_t name_length;
	uint16_t pad;
} x11_query_extension_fixed_t;
_Static_assert(sizeof(x11_query_extension_fixed_t) == 4U, "x11_query_extension_fixed_t size");

typedef struct __attribute__((packed)) {
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence_number;
	uint32_t length; /* 0 */
	uint8_t present;
	uint8_t major_opcode;
	uint8_t first_event;
	uint8_t first_error;
	uint8_t pad[20];
} x11_query_extension_reply_t;
_Static_assert(sizeof(x11_query_extension_reply_t) == 32U, "x11_query_extension_reply_t size");

#define X11_EVENT_KEY_PRESS 2U
#define X11_EVENT_KEY_RELEASE 3U
#define X11_EVENT_BUTTON_PRESS 4U
#define X11_EVENT_BUTTON_RELEASE 5U
#define X11_EVENT_MOTION_NOTIFY 6U
#define X11_EVENT_EXPOSE 12U

/*
 * Shared 32-byte layout of KeyPress/KeyRelease/ButtonPress/ButtonRelease/
 * MotionNotify -- detail holds the keycode, button number, or 0 for
 * motion. state uses the real X11 KeyButMask bit assignments (Shift=1,
 * Lock=2, Control=4, Mod1=8, Button1=0x100, Button2=0x200, Button3=0x400).
 */
typedef struct __attribute__((packed)) {
	uint8_t response_type;
	uint8_t detail;
	uint16_t sequence_number;
	uint32_t time;
	uint32_t root;
	uint32_t event;
	uint32_t child;
	int16_t root_x;
	int16_t root_y;
	int16_t event_x;
	int16_t event_y;
	uint16_t state;
	uint8_t same_screen;
	uint8_t pad;
} x11_input_event_t;
_Static_assert(sizeof(x11_input_event_t) == 32U, "x11_input_event_t size");

typedef struct __attribute__((packed)) {
	uint8_t response_type;
	uint8_t pad0;
	uint16_t sequence_number;
	uint32_t window;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	uint16_t count;
	uint8_t pad[14];
} x11_expose_event_t;
_Static_assert(sizeof(x11_expose_event_t) == 32U, "x11_expose_event_t size");

#endif
