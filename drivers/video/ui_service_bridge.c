#include <drivers/video/ui_service_bridge.h>

#include <kern/console/console.h>
#include <kern/lock.h>
#include <kern/machine/user.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>
#include <kern/machine/timer.h>
#include <vm/user_copy.h>

#include <stddef.h>
#include <string.h>

#if defined(NXU_UI_SERVICE)
#include <UIService.h>
#include <drivers/video/ui_service_activity.h>
#endif

#if defined(NXU_UI_SERVICE)

/* Messages waiting for one app; a full queue drops pointer moves first. */
#define UI_BRIDGE_QUEUE_LENGTH 64U

/* Dock activation requests not yet seen by the desktop. */
#define UI_BRIDGE_ACTIVATION_MAX 8U

/* How often a bounded receive looks at its queue (a sleep lasts a timer tick anyway). */
#define UI_BRIDGE_POLL_US 10000ULL

/* Activity Monitor asks for at most this many processes at a time. */
#define UI_BRIDGE_ACTIVITY_PROCESS_MAX 128U

typedef enum {
	UI_BRIDGE_FREE,
	/* Connected, not yet seen by the desktop. */
	UI_BRIDGE_PENDING,
	UI_BRIDGE_LIVE,
	/* The desktop let go of it; freed once the app's process is gone. */
	UI_BRIDGE_CLOSED
} ui_bridge_state_t;

typedef struct {
	ui_bridge_state_t state;
	proc_ident_t owner;
	nxu_ui_connect_t *info;

	nxu_spinlock_t lock;
	waitq_t inbox;
	nxu_ui_message_t *queue;
	uint32_t head;
	uint32_t count;

	/*
	 * Two frame buffers: the submit call fills `frames[spare]` from the app's
	 * memory without the lock, then swaps it with the ready one under it.
	 */
	uint32_t *frames[2];
	uint32_t frame_capacity;
	uint32_t spare;
	uint32_t ready_width;
	uint32_t ready_height;
	bool ready_new;
	bool submitting;

	nxu_ui_submit_t submitted;
	uint64_t submitted_sequence;

} ui_bridge_connection_t;

static ui_bridge_connection_t g_connections[UI_BRIDGE_CONNECTION_MAX];
static nxu_spinlock_t g_bridge_lock = NXU_SPINLOCK_INIT;

static volatile bool g_session_running;
static nxu_ui_session_t g_session;
static waitq_t g_session_wait;

static uint32_t g_activations[UI_BRIDGE_ACTIVATION_MAX];
static uint32_t g_activation_count;

/* Bundles apps asked to open, for the desktop to hand to the Dock. */
#define UI_BRIDGE_LAUNCH_MAX 4U
static char g_launches[UI_BRIDGE_LAUNCH_MAX][NXU_UI_PATH_MAX];
static uint32_t g_launch_count;

static bool ui_bridge_same_process(const proc_ident_t *left, const proc_ident_t *right)
{
	return left->pid == right->pid && left->uniqueid == right->uniqueid && left->idversion == right->idversion;
}

static bool ui_bridge_owner_alive(const ui_bridge_connection_t *connection)
{
	proc_t proc = proc_find_ident(&connection->owner);

	if (proc == 0) return false;
	proc_rele(proc);
	return true;
}

static void ui_bridge_free_locked(ui_bridge_connection_t *connection)
{
	if (connection->info != 0) (void)kfree(connection->info);
	if (connection->queue != 0) (void)kfree(connection->queue);
	if (connection->frames[0] != 0) (void)kfree(connection->frames[0]);
	if (connection->frames[1] != 0) (void)kfree(connection->frames[1]);

	*connection = (ui_bridge_connection_t) { .state = UI_BRIDGE_FREE };
}

/* Free the slots whose app is gone after the desktop let go of them. */
static void ui_bridge_reap_locked(void)
{
	for (uint32_t index = 0U; index < UI_BRIDGE_CONNECTION_MAX; index++) {
		ui_bridge_connection_t *connection = &g_connections[index];

		if (connection->state == UI_BRIDGE_CLOSED && !connection->submitting && !ui_bridge_owner_alive(connection)) {
			ui_bridge_free_locked(connection);
		}
	}
}

/* The caller's own connection `number` (1-based), or 0. */
static ui_bridge_connection_t *ui_bridge_caller_connection(uint64_t number)
{
	if (number == 0ULL || number > UI_BRIDGE_CONNECTION_MAX) return 0;

	ui_bridge_connection_t *connection = &g_connections[number - 1ULL];
	proc_t proc = current_proc();

	if (proc == 0 || connection->state == UI_BRIDGE_FREE) return 0;
	if (!ui_bridge_same_process(&connection->owner, &proc->p_ident)) return 0;
	return connection;
}

static void ui_bridge_terminate_string(char *text, size_t capacity)
{
	text[capacity - 1U] = '\0';
}

/* Wait (interruptibly) until the desktop is up. */
static bool ui_bridge_wait_session(void)
{
	for (;;) {
		uint32_t sequence = waitq_seq(&g_session_wait);

		if (g_session_running) return true;
		if (!waitq_block_seq(&g_session_wait, sequence, true)) return false;
	}
}

int64_t ui_bridge_syscall_connect(uint64_t user_info)
{
	nxu_ui_connect_t *info = kmalloc(sizeof(*info));

	if (info == 0) return -NXU_SYS_E_NO_MEMORY;

	if (!vm_copy_from_user(info, user_info, sizeof(*info))) {
		(void)kfree(info);
		return -NXU_SYS_E_BAD_ADDRESS;
	}

	bool valid = info->struct_size == sizeof(*info) &&
		(info->kind == NXU_UI_KIND_APP || info->kind == NXU_UI_KIND_DOCK) &&
		info->menu_count <= NXU_UI_MENU_MAX &&
		info->item_count <= NXU_UI_MENU_ITEM_MAX;

	if (valid && info->kind == NXU_UI_KIND_APP) {
		valid = info->width != 0U && info->height != 0U &&
			info->max_width >= info->width && info->max_height >= info->height &&
			info->max_width <= 4096U && info->max_height <= 4096U &&
			info->titlebar_height < info->height;
	}

	if (!valid) {
		(void)kfree(info);
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	ui_bridge_terminate_string(info->name, sizeof(info->name));
	ui_bridge_terminate_string(info->bundle_path, sizeof(info->bundle_path));
	for (uint32_t menu = 0U; menu < NXU_UI_MENU_MAX; menu++) ui_bridge_terminate_string(info->menu_titles[menu], sizeof(info->menu_titles[menu]));
	for (uint32_t item = 0U; item < NXU_UI_MENU_ITEM_MAX; item++) ui_bridge_terminate_string(info->items[item].title, sizeof(info->items[item].title));

	if (!ui_bridge_wait_session()) {
		(void)kfree(info);
		return -NXU_SYS_E_INTERRUPTED;
	}

	/* An app's frame is its content; the Dock's covers a band along the bottom. */
	uint64_t capacity = info->kind == NXU_UI_KIND_APP
		? (uint64_t)info->max_width * (uint64_t)info->max_height
		: (uint64_t)g_session.screen_width * (uint64_t)(g_session.screen_height / 3U);

	nxu_ui_message_t *queue = kcalloc(UI_BRIDGE_QUEUE_LENGTH, sizeof(nxu_ui_message_t));
	uint32_t *frame0 = kmalloc((size_t)capacity * sizeof(uint32_t));
	uint32_t *frame1 = kmalloc((size_t)capacity * sizeof(uint32_t));

	if (queue == 0 || frame0 == 0 || frame1 == 0) {
		if (queue != 0) (void)kfree(queue);
		if (frame0 != 0) (void)kfree(frame0);
		if (frame1 != 0) (void)kfree(frame1);
		(void)kfree(info);
		return -NXU_SYS_E_NO_MEMORY;
	}

	nxu_spin_lock(&g_bridge_lock);
	ui_bridge_reap_locked();

	int64_t result = -NXU_SYS_E_NO_SPACE;
	bool dock_taken = false;

	for (uint32_t index = 0U; index < UI_BRIDGE_CONNECTION_MAX; index++) {
		const ui_bridge_connection_t *connection = &g_connections[index];

		if (connection->state != UI_BRIDGE_FREE && connection->state != UI_BRIDGE_CLOSED && connection->info->kind == NXU_UI_KIND_DOCK) {
			dock_taken = true;
		}
	}

	if (info->kind == NXU_UI_KIND_DOCK && dock_taken) {
		result = -NXU_SYS_E_BUSY;
	} else {
		for (uint32_t index = 0U; index < UI_BRIDGE_CONNECTION_MAX; index++) {
			ui_bridge_connection_t *connection = &g_connections[index];

			if (connection->state != UI_BRIDGE_FREE) continue;

			*connection = (ui_bridge_connection_t) {
				.state = UI_BRIDGE_PENDING,
				.owner = current_proc()->p_ident,
				.info = info,
				.lock = NXU_SPINLOCK_INIT,
				.queue = queue,
				.frames = { frame0, frame1 },
				.frame_capacity = (uint32_t)capacity,
				.spare = 1U
			};
			waitq_init(&connection->inbox);
			result = (int64_t)index + 1;
			break;
		}
	}

	nxu_spin_unlock(&g_bridge_lock);

	if (result < 0) {
		(void)kfree(queue);
		(void)kfree(frame0);
		(void)kfree(frame1);
		(void)kfree(info);
		return result;
	}

	sched_kick_sleepers();
	kprintf("ui_bridge_syscall_connect: %s (pid %u) connected as %s %u\n",
		info->name, (unsigned int)current_proc()->p_ident.pid, info->kind == NXU_UI_KIND_DOCK ? "dock" : "app", (unsigned int)result);
	return result;
}

int64_t ui_bridge_syscall_receive(uint64_t number, uint64_t user_message, uint64_t wait)
{
	ui_bridge_connection_t *connection = ui_bridge_caller_connection(number);
	uint64_t deadline = 0ULL;

	if (connection == 0) return -NXU_SYS_E_NOT_FOUND;

	for (;;) {
		uint32_t sequence = waitq_seq(&connection->inbox);
		nxu_ui_message_t message;
		bool have = false;
		bool closed;

		nxu_spin_lock(&connection->lock);
		closed = connection->state == UI_BRIDGE_CLOSED;
		if (connection->count != 0U) {
			message = connection->queue[connection->head];
			connection->head = (connection->head + 1U) % UI_BRIDGE_QUEUE_LENGTH;
			connection->count--;
			have = true;
		}
		nxu_spin_unlock(&connection->lock);

		if (have) {
			if (!vm_copy_to_user(user_message, &message, sizeof(message))) return -NXU_SYS_E_BAD_ADDRESS;
			return 1;
		}

		/* The desktop let go of this app: nothing will ever come again. */
		if (closed) return -NXU_SYS_E_NOT_FOUND;
		if (wait == 0ULL) return 0;

		if (wait == NXU_UI_WAIT_FOREVER) {
			if (!waitq_block_seq(&connection->inbox, sequence, true)) return -NXU_SYS_E_INTERRUPTED;
			continue;
		}

		/*
		 * A bounded wait (the Dock, which also watches the apps it started):
		 * short sleeps until the deadline, since a wait queue has no timeout.
		 */
		uint64_t now = timer_get_microseconds();

		if (deadline == 0ULL) deadline = now + wait * 1000ULL;
		if (now >= deadline) return 0;
		(void)sched_sleep_us(UI_BRIDGE_POLL_US);
	}
}

int64_t ui_bridge_syscall_submit(uint64_t number, uint64_t user_submit)
{
	ui_bridge_connection_t *connection = ui_bridge_caller_connection(number);

	if (connection == 0) return -NXU_SYS_E_NOT_FOUND;

	nxu_ui_submit_t submit;

	if (!vm_copy_from_user(&submit, user_submit, sizeof(submit))) return -NXU_SYS_E_BAD_ADDRESS;
	submit.label[NXU_UI_LABEL_MAX - 1U] = '\0';

	bool has_frame = submit.pixels != 0ULL;

	if (has_frame) {
		uint64_t pixels = (uint64_t)submit.width * (uint64_t)submit.height;

		if (submit.width == 0U || submit.height == 0U || submit.stride < submit.width || pixels > connection->frame_capacity) {
			return -NXU_SYS_E_INVALID_ARGUMENT;
		}
	}

	nxu_spin_lock(&connection->lock);
	bool busy = connection->submitting || connection->state == UI_BRIDGE_CLOSED;
	if (!busy) connection->submitting = true;
	uint32_t spare = connection->spare;
	nxu_spin_unlock(&connection->lock);

	if (busy) return -NXU_SYS_E_BUSY;

	/* Only this call writes the spare buffer, so the copy needs no lock. */
	bool copied = true;

	if (has_frame) {
		uint32_t *destination = connection->frames[spare];
		uint64_t row_bytes = (uint64_t)submit.width * sizeof(uint32_t);

		for (uint32_t row = 0U; row < submit.height; row++) {
			uint64_t source = submit.pixels + (uint64_t)row * submit.stride * sizeof(uint32_t);

			if (!vm_copy_from_user(destination + (uint64_t)row * submit.width, source, row_bytes)) {
				copied = false;
				break;
			}
		}
	}

	nxu_spin_lock(&connection->lock);
	if (copied) {
		if (has_frame) {
			connection->spare = 1U - spare;
			connection->ready_width = submit.width;
			connection->ready_height = submit.height;
			connection->ready_new = true;
		}

		submit.pixels = 0ULL;
		connection->submitted = submit;
		connection->submitted_sequence++;
	}
	connection->submitting = false;
	nxu_spin_unlock(&connection->lock);

	/* The desktop shows it now, not at its next tick. */
	if (copied) sched_kick_sleepers();
	return copied ? 0 : -NXU_SYS_E_BAD_ADDRESS;
}

static int64_t ui_bridge_control_activate(uint64_t pid)
{
	proc_t proc = current_proc();
	bool is_dock = false;

	nxu_spin_lock(&g_bridge_lock);

	for (uint32_t index = 0U; index < UI_BRIDGE_CONNECTION_MAX; index++) {
		const ui_bridge_connection_t *connection = &g_connections[index];

		if ((connection->state == UI_BRIDGE_PENDING || connection->state == UI_BRIDGE_LIVE) &&
			connection->info->kind == NXU_UI_KIND_DOCK &&
			ui_bridge_same_process(&connection->owner, &proc->p_ident)) {
			is_dock = true;
		}
	}

	int64_t result = -NXU_SYS_E_DENIED;

	if (is_dock) {
		result = -NXU_SYS_E_AGAIN;

		if (g_activation_count < UI_BRIDGE_ACTIVATION_MAX) {
			g_activations[g_activation_count++] = (uint32_t)pid;
			result = 0;
			sched_kick_sleepers();
		}
	}

	nxu_spin_unlock(&g_bridge_lock);
	return result;
}

static int64_t ui_bridge_control_activity(uint64_t user_request)
{
	nxu_ui_activity_request_t request;

	if (!vm_copy_from_user(&request, user_request, sizeof(request))) return -NXU_SYS_E_BAD_ADDRESS;
	if (request.activity_size != sizeof(UIServiceActivity) || request.process_size != sizeof(UIServiceProcessInfo)) {
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}

	uint32_t capacity = request.capacity < UI_BRIDGE_ACTIVITY_PROCESS_MAX ? request.capacity : UI_BRIDGE_ACTIVITY_PROCESS_MAX;
	UIServiceActivity *activity = kcalloc(1U, sizeof(UIServiceActivity));
	UIServiceProcessInfo *processes = capacity != 0U ? kcalloc(capacity, sizeof(UIServiceProcessInfo)) : 0;

	if (activity == 0 || (capacity != 0U && processes == 0)) {
		if (activity != 0) (void)kfree(activity);
		if (processes != 0) (void)kfree(processes);
		return -NXU_SYS_E_NO_MEMORY;
	}

	activity->struct_size = sizeof(UIServiceActivity);

	uint32_t count = 0U;
	int64_t result = 0;

	if (ui_service_get_activity(0, activity, processes, capacity, &count) != UI_SERVICE_STATUS_OK) {
		result = -NXU_SYS_E_IO;
	} else if (!vm_copy_to_user(request.activity, activity, sizeof(UIServiceActivity)) ||
		(count != 0U && !vm_copy_to_user(request.processes, processes, (uint64_t)count * sizeof(UIServiceProcessInfo)))) {
		result = -NXU_SYS_E_BAD_ADDRESS;
	} else {
		request.count = count;
		if (!vm_copy_to_user(user_request, &request, sizeof(request))) result = -NXU_SYS_E_BAD_ADDRESS;
	}

	(void)kfree(activity);
	if (processes != 0) (void)kfree(processes);
	return result;
}

int64_t ui_bridge_syscall_control(uint64_t operation, uint64_t argument)
{
	switch (operation) {
	case NXU_UI_CONTROL_SESSION:
		if (!ui_bridge_wait_session()) return -NXU_SYS_E_INTERRUPTED;
		if (!vm_copy_to_user(argument, &g_session, sizeof(g_session))) return -NXU_SYS_E_BAD_ADDRESS;
		return 0;

	case NXU_UI_CONTROL_ACTIVATE:
		return ui_bridge_control_activate(argument);

	case NXU_UI_CONTROL_ACTIVITY:
		return ui_bridge_control_activity(argument);

	case NXU_UI_CONTROL_LAUNCH: {
		char path[NXU_UI_PATH_MAX];

		if (!vm_copy_string_from_user(path, argument, sizeof(path))) return -NXU_SYS_E_BAD_ADDRESS;

		int64_t result = -NXU_SYS_E_AGAIN;

		nxu_spin_lock(&g_bridge_lock);
		if (g_launch_count < UI_BRIDGE_LAUNCH_MAX) {
			memcpy(g_launches[g_launch_count++], path, sizeof(path));
			result = 0;
			sched_kick_sleepers();
		}
		nxu_spin_unlock(&g_bridge_lock);
		return result;
	}

	default:
		return -NXU_SYS_E_INVALID_ARGUMENT;
	}
}

void ui_bridge_session_begin(uint32_t scale_permille, uint32_t screen_width, uint32_t screen_height, uint32_t menubar_height)
{
	g_session = (nxu_ui_session_t) {
		.scale_permille = scale_permille,
		.screen_width = screen_width,
		.screen_height = screen_height,
		.menubar_height = menubar_height
	};
	__atomic_store_n(&g_session_running, true, __ATOMIC_RELEASE);
	waitq_wake_all(&g_session_wait);
	kprintf("ui_bridge_session_begin: apps may connect (%ux%u at %u permille)\n", screen_width, screen_height, scale_permille);
}

int32_t ui_bridge_accept(void)
{
	int32_t accepted = -1;

	nxu_spin_lock(&g_bridge_lock);
	ui_bridge_reap_locked();

	for (uint32_t index = 0U; index < UI_BRIDGE_CONNECTION_MAX; index++) {
		if (g_connections[index].state != UI_BRIDGE_PENDING) continue;

		g_connections[index].state = UI_BRIDGE_LIVE;
		accepted = (int32_t)index;
		break;
	}

	nxu_spin_unlock(&g_bridge_lock);
	return accepted;
}

static ui_bridge_connection_t *ui_bridge_live(uint32_t index)
{
	if (index >= UI_BRIDGE_CONNECTION_MAX) return 0;

	ui_bridge_connection_t *connection = &g_connections[index];
	return connection->state == UI_BRIDGE_LIVE ? connection : 0;
}

const nxu_ui_connect_t *ui_bridge_info(uint32_t index)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);
	return connection != 0 ? connection->info : 0;
}

uint32_t ui_bridge_pid(uint32_t index)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);
	return connection != 0 ? (uint32_t)connection->owner.pid : 0U;
}

bool ui_bridge_alive(uint32_t index)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);
	return connection != 0 && ui_bridge_owner_alive(connection);
}

static bool ui_bridge_is_pointer_move(const nxu_ui_message_t *message)
{
	return message->type == NXU_UI_MSG_EVENT && message->event == NXU_UI_EVENT_POINTER_MOVED;
}

bool ui_bridge_post(uint32_t index, const nxu_ui_message_t *message)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);

	if (connection == 0 || message == 0) return false;

	nxu_ui_message_t stamped = *message;
	stamped.time_us = timer_get_microseconds();

	bool queued = true;

	nxu_spin_lock(&connection->lock);

	uint32_t last = (connection->head + connection->count + UI_BRIDGE_QUEUE_LENGTH - 1U) % UI_BRIDGE_QUEUE_LENGTH;
	bool frame_queued = false;

	for (uint32_t offset = 0U; offset < connection->count; offset++) {
		if (connection->queue[(connection->head + offset) % UI_BRIDGE_QUEUE_LENGTH].type == NXU_UI_MSG_FRAME) frame_queued = true;
	}

	if (stamped.type == NXU_UI_MSG_FRAME && frame_queued) {
		/* One frame at a time: the app catches up with the clock when it gets to it. */
	} else if (connection->count != 0U && ui_bridge_is_pointer_move(&stamped) && ui_bridge_is_pointer_move(&connection->queue[last])) {
		connection->queue[last] = stamped;
	} else {
		if (connection->count == UI_BRIDGE_QUEUE_LENGTH) {
			if (ui_bridge_is_pointer_move(&stamped)) {
				queued = false;
			} else {
				/* Drop the oldest: the newest says more about what the user is doing now. */
				connection->head = (connection->head + 1U) % UI_BRIDGE_QUEUE_LENGTH;
				connection->count--;
			}
		}

		if (queued) {
			connection->queue[(connection->head + connection->count) % UI_BRIDGE_QUEUE_LENGTH] = stamped;
			connection->count++;
		}
	}


	nxu_spin_unlock(&connection->lock);

	waitq_wake_all(&connection->inbox);
	return queued;
}

bool ui_bridge_state(uint32_t index, nxu_ui_submit_t *state, uint64_t *sequence)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);

	if (connection == 0 || state == 0 || sequence == 0) return false;

	nxu_spin_lock(&connection->lock);
	bool newer = connection->submitted_sequence != *sequence;
	*state = connection->submitted;
	*sequence = connection->submitted_sequence;
	nxu_spin_unlock(&connection->lock);

	return newer;
}

bool ui_bridge_take_frame(
	uint32_t index,
	uint32_t *destination,
	uint32_t destination_stride,
	uint32_t max_width,
	uint32_t max_height,
	uint32_t *width_out,
	uint32_t *height_out
)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);

	if (connection == 0 || destination == 0) return false;

	nxu_spin_lock(&connection->lock);

	bool taken = connection->ready_new;

	if (taken) {
		const uint32_t *source = connection->frames[1U - connection->spare];
		uint32_t width = connection->ready_width < max_width ? connection->ready_width : max_width;
		uint32_t height = connection->ready_height < max_height ? connection->ready_height : max_height;

		for (uint32_t row = 0U; row < height; row++) {
			memcpy(destination + (size_t)row * destination_stride, source + (size_t)row * connection->ready_width, (size_t)width * sizeof(uint32_t));
		}

		connection->ready_new = false;
		if (width_out != 0) *width_out = connection->ready_width;
		if (height_out != 0) *height_out = connection->ready_height;
	}

	nxu_spin_unlock(&connection->lock);
	return taken;
}

bool ui_bridge_take_launch(char path[NXU_UI_PATH_MAX])
{
	bool taken = false;

	nxu_spin_lock(&g_bridge_lock);

	if (g_launch_count != 0U) {
		memcpy(path, g_launches[0], NXU_UI_PATH_MAX);
		for (uint32_t index = 1U; index < g_launch_count; index++) memcpy(g_launches[index - 1U], g_launches[index], NXU_UI_PATH_MAX);
		g_launch_count--;
		taken = true;
	}

	nxu_spin_unlock(&g_bridge_lock);
	return taken;
}

uint32_t ui_bridge_take_activation(void)
{
	uint32_t pid = 0U;

	nxu_spin_lock(&g_bridge_lock);

	if (g_activation_count != 0U) {
		pid = g_activations[0];
		for (uint32_t index = 1U; index < g_activation_count; index++) g_activations[index - 1U] = g_activations[index];
		g_activation_count--;
	}

	nxu_spin_unlock(&g_bridge_lock);
	return pid;
}

void ui_bridge_terminate(uint32_t index)
{
	ui_bridge_connection_t *connection = ui_bridge_live(index);

	if (connection == 0) return;

#if MACHINE_USER_CONTEXT
	(void)signal_send(proc_kernel(), connection->owner.pid, NXU_SIGKILL);
#endif
}

void ui_bridge_release(uint32_t index)
{
	if (index >= UI_BRIDGE_CONNECTION_MAX) return;

	ui_bridge_connection_t *connection = &g_connections[index];

	nxu_spin_lock(&g_bridge_lock);
	nxu_spin_lock(&connection->lock);
	if (connection->state == UI_BRIDGE_LIVE || connection->state == UI_BRIDGE_PENDING) connection->state = UI_BRIDGE_CLOSED;
	nxu_spin_unlock(&connection->lock);
	ui_bridge_reap_locked();
	nxu_spin_unlock(&g_bridge_lock);

	/* A receive blocked on it sees the connection is gone. */
	waitq_wake_all(&connection->inbox);
}

#else

int64_t ui_bridge_syscall_connect(uint64_t user_info)
{
	(void)user_info;
	return -NXU_SYS_E_NOT_SUPPORTED;
}

int64_t ui_bridge_syscall_receive(uint64_t connection, uint64_t user_message, uint64_t wait)
{
	(void)connection;
	(void)user_message;
	(void)wait;
	return -NXU_SYS_E_NOT_SUPPORTED;
}

int64_t ui_bridge_syscall_submit(uint64_t connection, uint64_t user_submit)
{
	(void)connection;
	(void)user_submit;
	return -NXU_SYS_E_NOT_SUPPORTED;
}

int64_t ui_bridge_syscall_control(uint64_t operation, uint64_t argument)
{
	(void)operation;
	(void)argument;
	return -NXU_SYS_E_NOT_SUPPORTED;
}

#endif
