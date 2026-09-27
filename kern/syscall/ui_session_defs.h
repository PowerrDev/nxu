#ifndef NXU_KERN_SYSCALL_UI_SESSION_DEFS_H
#define NXU_KERN_SYSCALL_UI_SESSION_DEFS_H

/*
 * UI sessions: how an app process (/Applications/<Name>.app) and the Dock
 * put windows on the desktop.
 *
 * The desktop (UIService's window manager, menu bar and WindowServer) runs in
 * the kernel; apps do not. An app connects once (NXU_SYS_UI_CONNECT) with its
 * name, window size and menus, then loops: receive a message
 * (NXU_SYS_UI_RECEIVE: an input event, a redraw request, a menu command, an
 * animation frame, quit), handle it, and submit what changed
 * (NXU_SYS_UI_SUBMIT: the window content's pixels, the pointer shape, the
 * menu items' states). The desktop draws the window chrome around the
 * content and routes input; the app never sees another app's pixels.
 *
 * Every structure here is the same size and layout on arm64 and i386: fixed
 * width fields only, user pointers carried as uint64_t.
 */

#include <stdint.h>

#define NXU_UI_NAME_MAX 48U
#define NXU_UI_PATH_MAX 128U
#define NXU_UI_MENU_MAX 6U
#define NXU_UI_MENU_TITLE_MAX 24U
#define NXU_UI_MENU_ITEM_MAX 48U
#define NXU_UI_MENU_ITEM_TITLE_MAX 40U
#define NXU_UI_LABEL_MAX 48U

/* nxu_ui_connect_t.kind */
#define NXU_UI_KIND_APP 1U  /* a titled window with chrome, in the menu bar */
#define NXU_UI_KIND_DOCK 2U /* the Dock: borderless, along the bottom, always in front */

/* nxu_ui_menu_item_t.command for a divider line. */
#define NXU_UI_MENU_SEPARATOR 0xFFFFFFFFU

typedef struct {
	uint32_t menu;    /* index into nxu_ui_connect_t.menu_titles */
	uint32_t command; /* passed back in NXU_UI_MSG_MENU_COMMAND, or NXU_UI_MENU_SEPARATOR */
	char title[NXU_UI_MENU_ITEM_TITLE_MAX];
} nxu_ui_menu_item_t;

/*
 * What an app tells the desktop when it connects. Sizes are physical pixels
 * at the session's scale (NXU_UI_CONTROL_SESSION), the whole window
 * including its titlebar. A Dock leaves the window fields zero: its size
 * comes with every submit (width/height of the frame).
 */
typedef struct {
	uint32_t struct_size;
	uint32_t kind;
	char name[NXU_UI_NAME_MAX];
	char bundle_path[NXU_UI_PATH_MAX];
	uint32_t width;
	uint32_t height;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t resizable;
	uint32_t titlebar_height;
	uint32_t corner_radius;
	uint32_t menu_count;
	uint32_t item_count;
	uint32_t reserved;
	char menu_titles[NXU_UI_MENU_MAX][NXU_UI_MENU_TITLE_MAX];
	nxu_ui_menu_item_t items[NXU_UI_MENU_ITEM_MAX];
} nxu_ui_connect_t;

/* nxu_ui_message_t.type */
#define NXU_UI_MSG_EVENT 1U        /* event, x, y, button, delta, character */
#define NXU_UI_MSG_REDRAW 2U       /* draw everything: first show, or the content was resized */
#define NXU_UI_MSG_MENU_COMMAND 3U /* command */
#define NXU_UI_MSG_QUIT 4U         /* the user quit the app: exit */
#define NXU_UI_MSG_FRAME 5U        /* the next animation frame (the app submitted NXU_UI_SUBMIT_ANIMATING) */
#define NXU_UI_MSG_LAUNCH 6U       /* Dock only: open the bundle at path */
#define NXU_UI_MSG_ACTIVE 7U       /* flags: the app became (NXU_UI_FLAG_ACTIVE) or stopped being frontmost */

/* nxu_ui_message_t.event, for NXU_UI_MSG_EVENT */
#define NXU_UI_EVENT_POINTER_MOVED 1U
#define NXU_UI_EVENT_POINTER_DOWN 2U
#define NXU_UI_EVENT_POINTER_UP 3U
#define NXU_UI_EVENT_POINTER_LEFT 4U
#define NXU_UI_EVENT_SCROLL 5U
#define NXU_UI_EVENT_KEY_DOWN 6U

/* nxu_ui_message_t.button for pointer events */
#define NXU_UI_BUTTON_PRIMARY 0U
#define NXU_UI_BUTTON_SECONDARY 1U
#define NXU_UI_BUTTON_MIDDLE 2U

#define NXU_UI_FLAG_ACTIVE (1U << 0U)

/*
 * One message for the app. Coordinates are the content area's (0,0 is the
 * content's top-left, under the titlebar); width/height are the content
 * size right now, with every message.
 */
typedef struct {
	uint32_t type;
	uint32_t event;
	int32_t x;
	int32_t y;
	int32_t delta;
	uint32_t button;    /* pointer button, or the evdev key code for KEY_DOWN */
	uint32_t character; /* the Unicode scalar a key types, 0 for none */
	uint32_t command;
	uint32_t width;
	uint32_t height;
	uint32_t flags;
	uint32_t reserved;
	uint64_t time_us;
	char path[NXU_UI_PATH_MAX];
} nxu_ui_message_t;

/*
 * NXU_SYS_UI_RECEIVE's wait: 0 returns at once, NXU_UI_WAIT_FOREVER sleeps
 * until a message comes, anything else is a timeout in milliseconds.
 */
#define NXU_UI_WAIT_FOREVER 0xFFFFFFFFU

/* nxu_ui_submit_t.action */
#define NXU_UI_ACTION_NONE 0U
#define NXU_UI_ACTION_CLOSE 1U /* the app wants to quit: the desktop takes its window down */

/* nxu_ui_submit_t.cursor */
#define NXU_UI_CURSOR_ARROW 0U
#define NXU_UI_CURSOR_HAND 1U
#define NXU_UI_CURSOR_NOT_ALLOWED 2U

/* nxu_ui_submit_t.flags */
#define NXU_UI_SUBMIT_ANIMATING (1U << 0U)  /* send NXU_UI_MSG_FRAME on the next frame */
#define NXU_UI_SUBMIT_MENU_STATE (1U << 1U) /* menu_state is current */

/* menu_state bits, per nxu_ui_connect_t.items index */
#define NXU_UI_MENU_ENABLED (1U << 0U)
#define NXU_UI_MENU_CHECKED (1U << 1U)

/*
 * What changed. pixels (0: no new frame) points at width x height XRGB8888
 * content pixels, stride pixels apart; an app's frame is its content area,
 * a Dock's the whole window in ARGB8888 (alpha 0 is see-through).
 *
 * The panel_* and label fields are the Dock's: the rounded panel the desktop
 * fills with its translucent material under the icons (and hit-tests), and
 * the name shown over the icon under the pointer, centred on label_x.
 */
typedef struct {
	uint64_t pixels;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t action;
	uint32_t cursor;
	uint32_t flags;
	uint8_t menu_state[NXU_UI_MENU_ITEM_MAX];
	int32_t panel_x;
	int32_t panel_y;
	uint32_t panel_width;
	uint32_t panel_height;
	uint32_t panel_radius;
	int32_t label_x;
	char label[NXU_UI_LABEL_MAX];
} nxu_ui_submit_t;

/* NXU_SYS_UI_CONTROL operations */
#define NXU_UI_CONTROL_SESSION 1U  /* argument: nxu_ui_session_t *; waits for the desktop */
#define NXU_UI_CONTROL_ACTIVATE 2U /* argument: a pid; show and focus that app's window */
#define NXU_UI_CONTROL_ACTIVITY 3U /* argument: nxu_ui_activity_request_t * (Activity Monitor) */
#define NXU_UI_CONTROL_LAUNCH 4U   /* argument: a NUL-terminated bundle path; the Dock opens it */

typedef struct {
	uint32_t scale_permille; /* physical pixels per point, x1000 */
	uint32_t screen_width;
	uint32_t screen_height;
	uint32_t menubar_height;
} nxu_ui_session_t;

/*
 * The kernel fills activity (UIServiceActivity, UIService.h) and up to
 * capacity entries of processes (UIServiceProcessInfo), and stores how many
 * in count.
 */
typedef struct {
	uint64_t activity;
	uint64_t processes;
	uint32_t activity_size;
	uint32_t process_size;
	uint32_t capacity;
	uint32_t count;
} nxu_ui_activity_request_t;

/* UIService's ui-session crate mirrors these layouts and checks the same sizes. */
_Static_assert(sizeof(nxu_ui_menu_item_t) == 48U, "nxu_ui_menu_item_t layout");
_Static_assert(sizeof(nxu_ui_connect_t) == 2680U, "nxu_ui_connect_t layout");
_Static_assert(sizeof(nxu_ui_message_t) == 184U, "nxu_ui_message_t layout");
_Static_assert(sizeof(nxu_ui_submit_t) == 152U, "nxu_ui_submit_t layout");
_Static_assert(sizeof(nxu_ui_session_t) == 16U, "nxu_ui_session_t layout");
_Static_assert(sizeof(nxu_ui_activity_request_t) == 32U, "nxu_ui_activity_request_t layout");

#endif
