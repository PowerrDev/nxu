#ifndef NXU_DRIVERS_VIDEO_UI_SERVICE_BRIDGE_H
#define NXU_DRIVERS_VIDEO_UI_SERVICE_BRIDGE_H

/*
 * The UI session bridge: app processes on one side (the NXU_SYS_UI_*
 * system calls, see kern/syscall/ui_session_defs.h), the desktop on the
 * other (UIService's window manager, running in the kernel, calls the
 * ui_bridge_* functions below from its one thread).
 *
 * A connection holds what the app said when it connected, a queue of
 * messages for it, and its latest frame. The app's frame is copied out of
 * its address space by the submit call into a buffer the desktop then copies
 * from under the connection's lock, so the desktop never touches user memory
 * and an app can never stall it. Shared by the arm64 and i386 kernels;
 * without NXU_UI_SERVICE every system call answers "not supported".
 */

#include <kern/syscall/syscall_defs.h>

#include <stdbool.h>
#include <stdint.h>

#define UI_BRIDGE_CONNECTION_MAX 8U

/* The system calls: each returns a value >= 0, or -NXU_SYS_E_*. */
int64_t ui_bridge_syscall_connect(uint64_t user_info);
int64_t ui_bridge_syscall_receive(uint64_t connection, uint64_t user_message, uint64_t wait);
int64_t ui_bridge_syscall_submit(uint64_t connection, uint64_t user_submit);
int64_t ui_bridge_syscall_control(uint64_t operation, uint64_t argument);

/*
 * The desktop's side. Connections are numbered 0..UI_BRIDGE_CONNECTION_MAX-1
 * here (the app sees that number plus one).
 */

/* The desktop is up: app connects stop waiting. */
void ui_bridge_session_begin(uint32_t scale_permille, uint32_t screen_width, uint32_t screen_height, uint32_t menubar_height);

/* A connection the desktop has not taken yet, or -1. Taking it is once. */
int32_t ui_bridge_accept(void);

/* What the app sent when it connected; valid until ui_bridge_release. */
const nxu_ui_connect_t *ui_bridge_info(uint32_t connection);

uint32_t ui_bridge_pid(uint32_t connection);

/* The app's process still exists. */
bool ui_bridge_alive(uint32_t connection);

/* Queue a message for the app and wake it. Pointer moves coalesce. */
bool ui_bridge_post(uint32_t connection, const nxu_ui_message_t *message);

/*
 * The latest submit: its fields apart from pixels, and whether it is new
 * since the last call (*sequence is where the caller keeps the count).
 */
bool ui_bridge_state(uint32_t connection, nxu_ui_submit_t *state, uint64_t *sequence);

/*
 * If a frame came in since the last call, copy it into destination
 * (destination_stride pixels apart, at most max_width x max_height) and store
 * its size. Returns whether it did.
 */
bool ui_bridge_take_frame(
	uint32_t connection,
	uint32_t *destination,
	uint32_t destination_stride,
	uint32_t max_width,
	uint32_t max_height,
	uint32_t *width_out,
	uint32_t *height_out
);

/* The next bundle an app asked to open (NXU_UI_CONTROL_LAUNCH), into path; false if none. */
bool ui_bridge_take_launch(char path[NXU_UI_PATH_MAX]);

/* The next pid the Dock asked to bring to the front, or 0. */
uint32_t ui_bridge_take_activation(void);

/* End the app's process (it would not quit when asked). */
void ui_bridge_terminate(uint32_t connection);

/* The desktop is done with the connection: its slot is free again. */
void ui_bridge_release(uint32_t connection);

#endif
