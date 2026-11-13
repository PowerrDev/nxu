#ifndef NXU_IPC_PEER_H
#define NXU_IPC_PEER_H

#include <stdint.h>
#include <stdbool.h>

/* Kernel-issued identity. Payload fields never authenticate an IPC sender. */
typedef struct {
	uint32_t pid;
	uint32_t idversion;
	uint64_t uniqueid;
	uint32_t capabilities;
	/* The sender's effective UID when it sent: 0 is root (which holds every
	 * capability whatever `capabilities` says). */
	uint32_t uid;
} nxu_ipc_peer_t;

static inline bool
nxu_ipc_peer_equal(const nxu_ipc_peer_t *a, const nxu_ipc_peer_t *b)
{
	return a->pid == b->pid && a->uniqueid == b->uniqueid && a->idversion == b->idversion;
}

_Static_assert(sizeof(nxu_ipc_peer_t) == 24, "IPC peer wire layout");

#endif
