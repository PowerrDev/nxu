/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/socket.c
 *
 * Socket object lifecycle. See kern/ipc/socket.h for the usage protocol.
 *
 * Reference counting: socket_create hands back one reference (the caller's
 * "handle"). Pairing a connect() with an accept() adds one more reference
 * on each side for the other's .peer pointer (socket_reference), so a
 * socket can never be freed out from under its peer while that peer might
 * still touch it -- socket_close releases exactly the two references that
 * accumulate this way: its own handle reference, and (if still connected)
 * the peer-link reference it holds on its peer.
 *
 * Locking: every socket has its own lock, taken only around that one
 * object's own fields -- nothing in this file ever holds two sockets'
 * locks at once (socket_close locks itself, unlocks, then locks its former
 * peer; socket_write locks only the target it is writing into). Blocking
 * (sched_block) only ever happens with the lock already dropped -- see the
 * loops in socket_connect/socket_accept/socket_read/socket_write, all of
 * which push the current thread onto the relevant socket's own wait queue
 * (thread_sched_links_t.waitq, kern/process/thread.h) before unlocking and
 * blocking, then re-lock and re-check on wakeup.
 */

#include <kern/ipc/socket.h>

#include <kern/lock.h>
#include <kern/memory/heap.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SOCKET_RING_SIZE 8192U
#define SOCKET_REGISTRY_MAX 8U

typedef enum {
	SOCKET_STATE_UNBOUND,
	SOCKET_STATE_LISTENING,
	SOCKET_STATE_CONNECTED,
	SOCKET_STATE_CLOSED
} socket_state_t;

struct socket {
	socket_state_t state;
	uint32_t references;
	nxu_spinlock_t lock;

	/* Connected-socket fields. */
	struct socket *peer;
	bool peer_closed;
	uint8_t *ring_data;
	uint32_t ring_head;
	uint32_t ring_tail;
	uint32_t ring_count;

	/* Threads blocked in connect/accept/read/write on this object. */
	waitq_t waiters;

	/* Listening-socket fields. */
	struct socket *pending_head;
	struct socket *pending_tail;
	uint32_t backlog_limit;
	uint32_t backlog_count;
	char name[SOCKET_NAME_MAX + 1U];

	/* This object's own linkage while queued on some listener's pending
	 * list (set by socket_connect, consumed by socket_accept). */
	struct socket *pending_next;

	/* Set on a still-pending (not yet accepted) client if its listener
	 * closes first -- socket_connect's wait loop treats this as failure. */
	bool refused;
};

typedef struct {
	char name[SOCKET_NAME_MAX + 1U];
	socket_t socket;
} socket_registry_entry_t;

static socket_registry_entry_t g_socket_registry[SOCKET_REGISTRY_MAX];
static nxu_spinlock_t g_socket_registry_lock;

static void
socket_waitq_push(socket_t s, thread_t thread)
{
	/* Not interruptible: socket loops re-check their condition on every
	 * wakeup but have no way to report "interrupted" to their callers. */
	waitq_enqueue(&s->waiters, thread, false);
}

static void
socket_wake_one(socket_t s)
{
	waitq_wake_one(&s->waiters);
}

static void
socket_wake_all(socket_t s)
{
	waitq_wake_all(&s->waiters);
}

static uint32_t
socket_ring_free_space(const struct socket *s)
{
	return SOCKET_RING_SIZE - s->ring_count;
}

static uint32_t
socket_ring_write(struct socket *s, const uint8_t *data, uint32_t length)
{
	uint32_t written = 0U;

	while (written < length && s->ring_count < SOCKET_RING_SIZE) {
		s->ring_data[s->ring_head] = data[written];
		s->ring_head = (s->ring_head + 1U) % SOCKET_RING_SIZE;
		s->ring_count++;
		written++;
	}

	return written;
}

static uint32_t
socket_ring_read(struct socket *s, uint8_t *data, uint32_t length)
{
	uint32_t read = 0U;

	while (read < length && s->ring_count > 0U) {
		data[read] = s->ring_data[s->ring_tail];
		s->ring_tail = (s->ring_tail + 1U) % SOCKET_RING_SIZE;
		s->ring_count--;
		read++;
	}

	return read;
}

static bool
socket_ring_alloc(struct socket *s)
{
	s->ring_data = kmalloc(SOCKET_RING_SIZE);
	if (s->ring_data == 0) return false;

	s->ring_head = 0U;
	s->ring_tail = 0U;
	s->ring_count = 0U;
	return true;
}

static void
socket_release_internal(socket_t s)
{
	if (s == SOCKET_NULL) return;

	nxu_spin_lock(&s->lock);
	bool destroy = false;
	if (s->references != 0U) {
		s->references--;
		destroy = s->references == 0U;
	}
	nxu_spin_unlock(&s->lock);

	if (!destroy) return;

	if (s->ring_data != 0) (void)kfree(s->ring_data);
	(void)kfree(s);
}

static void
socket_registry_remove(socket_t s)
{
	nxu_spin_lock(&g_socket_registry_lock);

	for (uint32_t index = 0U; index < SOCKET_REGISTRY_MAX; index++) {
		if (g_socket_registry[index].socket == s) {
			g_socket_registry[index].socket = SOCKET_NULL;
			g_socket_registry[index].name[0] = '\0';
			break;
		}
	}

	nxu_spin_unlock(&g_socket_registry_lock);
}

static socket_t
socket_registry_lookup(const char *name)
{
	nxu_spin_lock(&g_socket_registry_lock);

	socket_t found = SOCKET_NULL;

	for (uint32_t index = 0U; index < SOCKET_REGISTRY_MAX; index++) {
		if (g_socket_registry[index].socket != SOCKET_NULL && strcmp(g_socket_registry[index].name, name) == 0) {
			found = g_socket_registry[index].socket;
			break;
		}
	}

	if (found != SOCKET_NULL && !socket_reference(found)) found = SOCKET_NULL;

	nxu_spin_unlock(&g_socket_registry_lock);

	return found;
}

bool socket_create(socket_t *result)
{
	if (result != 0) *result = SOCKET_NULL;
	if (result == 0) return false;

	socket_t s = kmalloc(sizeof(struct socket));
	if (s == 0) return false;

	*s = (struct socket) { 0 };
	s->state = SOCKET_STATE_UNBOUND;
	s->references = 1U;

	*result = s;
	return true;
}

bool socket_reference(socket_t s)
{
	if (s == SOCKET_NULL) return false;

	nxu_spin_lock(&s->lock);
	bool valid = s->references != 0U && s->references != UINT32_MAX;
	if (valid) s->references++;
	nxu_spin_unlock(&s->lock);

	return valid;
}

bool socket_listen(socket_t s, const char *name, uint32_t backlog)
{
	if (s == SOCKET_NULL || name == 0) return false;

	uint64_t name_length = strlen(name);
	if (name_length == 0ULL || name_length > SOCKET_NAME_MAX) return false;

	nxu_spin_lock(&s->lock);
	bool valid = s->state == SOCKET_STATE_UNBOUND;
	if (valid) {
		s->state = SOCKET_STATE_LISTENING;
		s->backlog_limit = backlog == 0U
			? 1U
			: (backlog > SOCKET_BACKLOG_MAX ? SOCKET_BACKLOG_MAX : backlog);

		for (uint64_t index = 0ULL; index < name_length; index++) s->name[index] = name[index];
		s->name[name_length] = '\0';
	}
	nxu_spin_unlock(&s->lock);

	if (!valid) return false;

	nxu_spin_lock(&g_socket_registry_lock);

	bool duplicate = false;
	int32_t free_slot = -1;

	for (uint32_t index = 0U; index < SOCKET_REGISTRY_MAX; index++) {
		if (g_socket_registry[index].socket != SOCKET_NULL && strcmp(g_socket_registry[index].name, s->name) == 0) {
			duplicate = true;
			break;
		}
		if (free_slot < 0 && g_socket_registry[index].socket == SOCKET_NULL) free_slot = (int32_t)index;
	}

	bool inserted = false;
	if (!duplicate && free_slot >= 0) {
		uint32_t slot = (uint32_t)free_slot;
		for (uint64_t index = 0ULL; index <= name_length; index++) g_socket_registry[slot].name[index] = s->name[index];
		g_socket_registry[slot].socket = s;
		inserted = true;
	}

	nxu_spin_unlock(&g_socket_registry_lock);

	if (!inserted) {
		nxu_spin_lock(&s->lock);
		s->state = SOCKET_STATE_UNBOUND;
		nxu_spin_unlock(&s->lock);
		return false;
	}

	return true;
}

bool socket_connect(const char *name, bool block, socket_t *result)
{
	if (result != 0) *result = SOCKET_NULL;
	if (name == 0 || result == 0) return false;

	socket_t listener = socket_registry_lookup(name);
	if (listener == SOCKET_NULL) return false;

	socket_t client;
	if (!socket_create(&client)) {
		socket_release_internal(listener);
		return false;
	}

	nxu_spin_lock(&listener->lock);
	bool has_room = listener->state == SOCKET_STATE_LISTENING && listener->backlog_count < listener->backlog_limit;
	if (has_room) {
		client->pending_next = 0;
		if (listener->pending_tail != 0) {
			listener->pending_tail->pending_next = client;
		} else {
			listener->pending_head = client;
		}
		listener->pending_tail = client;
		listener->backlog_count++;
		socket_wake_one(listener);
	}
	nxu_spin_unlock(&listener->lock);

	socket_release_internal(listener);

	if (!has_room) {
		socket_release_internal(client);
		return false;
	}

	nxu_spin_lock(&client->lock);
	while (client->peer == SOCKET_NULL && !client->refused) {
		if (!block) break;
		socket_waitq_push(client, current_thread());
		nxu_spin_unlock(&client->lock);
		sched_block(false);
		nxu_spin_lock(&client->lock);
	}
	bool connected = client->peer != SOCKET_NULL;
	nxu_spin_unlock(&client->lock);

	if (!connected) {
		socket_release_internal(client);
		return false;
	}

	*result = client;
	return true;
}

bool socket_accept(socket_t listener, bool block, socket_t *result)
{
	if (result != 0) *result = SOCKET_NULL;
	if (listener == SOCKET_NULL || result == 0) return false;

	nxu_spin_lock(&listener->lock);

	if (listener->state != SOCKET_STATE_LISTENING) {
		nxu_spin_unlock(&listener->lock);
		return false;
	}

	while (listener->pending_head == SOCKET_NULL) {
		if (!block) {
			nxu_spin_unlock(&listener->lock);
			return false;
		}
		socket_waitq_push(listener, current_thread());
		nxu_spin_unlock(&listener->lock);
		sched_block(false);
		nxu_spin_lock(&listener->lock);

		if (listener->state != SOCKET_STATE_LISTENING) {
			nxu_spin_unlock(&listener->lock);
			return false;
		}
	}

	socket_t client = listener->pending_head;
	listener->pending_head = client->pending_next;
	if (listener->pending_head == SOCKET_NULL) listener->pending_tail = SOCKET_NULL;
	listener->backlog_count--;
	client->pending_next = 0;

	nxu_spin_unlock(&listener->lock);

	socket_t server;
	if (!socket_create(&server)) return false;

	if (!socket_ring_alloc(server)) {
		socket_release_internal(server);
		return false;
	}

	nxu_spin_lock(&client->lock);
	bool client_ready = socket_ring_alloc(client);
	if (client_ready) {
		client->peer = server;
		client->state = SOCKET_STATE_CONNECTED;
		(void)socket_reference(server);
		socket_wake_all(client);
	}
	nxu_spin_unlock(&client->lock);

	if (!client_ready) {
		socket_release_internal(server);
		return false;
	}

	server->peer = client;
	server->state = SOCKET_STATE_CONNECTED;
	(void)socket_reference(client);

	*result = server;
	return true;
}

int64_t socket_read(socket_t s, void *buffer, uint64_t length)
{
	if (s == SOCKET_NULL || buffer == 0) return -1;

	nxu_spin_lock(&s->lock);

	if (s->state != SOCKET_STATE_CONNECTED) {
		nxu_spin_unlock(&s->lock);
		return -1;
	}

	while (s->ring_count == 0U && !s->peer_closed) {
		socket_waitq_push(s, current_thread());
		nxu_spin_unlock(&s->lock);
		sched_block(false);
		nxu_spin_lock(&s->lock);
	}

	uint64_t capacity = length > (uint64_t)UINT32_MAX ? (uint64_t)UINT32_MAX : length;
	uint32_t read = socket_ring_read(s, (uint8_t *)buffer, (uint32_t)capacity);

	if (read > 0U) socket_wake_one(s);

	nxu_spin_unlock(&s->lock);

	return (int64_t)read;
}

int64_t socket_write(socket_t s, const void *buffer, uint64_t length)
{
	if (s == SOCKET_NULL || buffer == 0) return -1;

	nxu_spin_lock(&s->lock);
	socket_t target = s->state == SOCKET_STATE_CONNECTED ? s->peer : SOCKET_NULL;
	nxu_spin_unlock(&s->lock);

	if (target == SOCKET_NULL) return -1;

	const uint8_t *src = (const uint8_t *)buffer;
	uint64_t written = 0ULL;

	nxu_spin_lock(&target->lock);

	while (written < length) {
		if (target->state != SOCKET_STATE_CONNECTED) break;

		uint32_t free_space = socket_ring_free_space(target);
		if (free_space == 0U) {
			socket_waitq_push(target, current_thread());
			nxu_spin_unlock(&target->lock);
			sched_block(false);
			nxu_spin_lock(&target->lock);
			continue;
		}

		uint64_t remaining = length - written;
		uint32_t chunk = remaining > (uint64_t)free_space ? free_space : (uint32_t)remaining;
		written += socket_ring_write(target, src + written, chunk);

		socket_wake_one(target);
	}

	nxu_spin_unlock(&target->lock);

	if (written > 0ULL) return (int64_t)written;
	return length == 0ULL ? 0 : -1;
}

void socket_close(socket_t s)
{
	if (s == SOCKET_NULL) return;

	nxu_spin_lock(&s->lock);

	if (s->state == SOCKET_STATE_CLOSED) {
		nxu_spin_unlock(&s->lock);
		socket_release_internal(s);
		return;
	}

	socket_state_t previous_state = s->state;
	s->state = SOCKET_STATE_CLOSED;

	socket_t peer = s->peer;
	s->peer = 0;

	socket_t pending = previous_state == SOCKET_STATE_LISTENING ? s->pending_head : SOCKET_NULL;
	s->pending_head = 0;
	s->pending_tail = 0;

	socket_wake_all(s);

	nxu_spin_unlock(&s->lock);

	if (previous_state == SOCKET_STATE_LISTENING) {
		socket_registry_remove(s);

		while (pending != SOCKET_NULL) {
			socket_t next = pending->pending_next;

			nxu_spin_lock(&pending->lock);
			pending->refused = true;
			pending->pending_next = 0;
			socket_wake_all(pending);
			nxu_spin_unlock(&pending->lock);

			pending = next;
		}
	}

	if (peer != SOCKET_NULL) {
		nxu_spin_lock(&peer->lock);
		peer->peer_closed = true;
		socket_wake_all(peer);
		nxu_spin_unlock(&peer->lock);

		socket_release_internal(peer);
	}

	socket_release_internal(s);
}
