/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/socket.h
 *
 * Streaming, blocking, arbitrary-length local sockets.
 *
 * NXPC (kern/ipc/ipc_kmsg.h/ipc_port.h) is a bounded (4KB), non-blocking,
 * poll-based datagram system -- a poor fit for something like an X11 wire
 * connection, which is an arbitrary-length byte stream. This is a
 * connect/listen/accept/read/write/close byte-stream pipe pair with real
 * scheduler-integrated blocking (kern/sched_prism/sched.h's sched_block/
 * sched_thread_wakeup), built for that shape instead.
 *
 * A single kernel-global named registry (socket_listen/socket_connect)
 * stands in for a real filesystem namespace -- enough for the handful of
 * well-known listening sockets a milestone like this needs; routing names
 * through the real VFS as socket special files is future work.
 *
 * Each connected end owns a private receive ring, written to by its peer
 * and drained by its own reads -- see kern/ipc/socket.c for the exact
 * reference-counting and blocking discipline.
 */

#ifndef NXU_KERN_IPC_SOCKET_H
#define NXU_KERN_IPC_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

struct socket;
typedef struct socket *socket_t;

#define SOCKET_NULL ((socket_t)0)
#define SOCKET_NAME_MAX 63U
#define SOCKET_BACKLOG_MAX 8U

bool socket_create(socket_t *result);
bool socket_reference(socket_t socket);

/*
 * Registers socket as the listener for name (a NUL-terminated string of at
 * most SOCKET_NAME_MAX bytes). Fails if a listener is already registered
 * under that name. backlog is clamped to SOCKET_BACKLOG_MAX.
 */
bool socket_listen(socket_t socket, const char *name, uint32_t backlog);

/*
 * Looks up the listener registered under name and enqueues a fresh
 * client-side socket on its backlog. Fails immediately (no blocking) if no
 * listener is registered under name or its backlog is full. Otherwise,
 * unless block is false, blocks until a matching socket_accept pairs
 * *result with a server-side socket, or the listener closes first (in
 * which case this fails and releases *result).
 */
bool socket_connect(const char *name, bool block, socket_t *result);

/*
 * Blocks (unless block is false) until a pending socket_connect arrives on
 * listener, then pairs it with a fresh server-side socket returned via
 * *result.
 */
bool socket_accept(socket_t listener, bool block, socket_t *result);

/*
 * Reads up to length bytes already written by socket's peer, blocking while
 * the receive ring is empty and the peer hasn't closed. Returns 0 once the
 * peer has closed and every byte it sent has been drained (EOF), or -1 if
 * socket itself is not a connected end.
 */
int64_t socket_read(socket_t socket, void *buffer, uint64_t length);

/*
 * Writes up to length bytes into the peer's receive ring, blocking while it
 * is full. Returns -1 if socket is not connected or its peer has already
 * closed (possibly after a nonzero partial write, whose count is returned
 * instead).
 */
int64_t socket_write(socket_t socket, const void *buffer, uint64_t length);

/*
 * Marks this end closed, wakes anything blocked on it or its peer (which
 * observes EOF/failure from then on), and releases the reference socket
 * came into this call already holding.
 */
void socket_close(socket_t socket);

#endif
