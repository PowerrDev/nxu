#ifndef NXU_USER_BOOTSTRAP_PROTOCOL_H
#define NXU_USER_BOOTSTRAP_PROTOCOL_H

#include <stdint.h>

/*
 * Wire format for the bootstrap registry hosted by bootd (PID 1) on the
 * port every process inherits a send right to at spawn time (see
 * kern/loader/elf.c's bootstrap-port handoff, nxu_ipc_bootstrap_port()).
 * Two message shapes, one opcode field:
 *
 *   REGISTER: request only, no reply expected. The sender's own listen
 *   port is transferred along with the message (NXU_IPC_XFER_PORT); bootd
 *   keeps its own local name for it and answers later LOOKUPs for `label`
 *   by handing out a fresh reference to that same port.
 *
 *   LOOKUP: request carries the caller's own reply port transferred along
 *   with it (NXU_IPC_XFER_PORT) so bootd has somewhere to send the answer.
 *   bootd's reply is a nxu_bootstrap_reply_t sent back to that port, with
 *   the found service port transferred along on NXU_BOOTSTRAP_STATUS_OK.
 */

#define NXU_BOOTSTRAP_LABEL_MAX 64U

typedef enum {
	NXU_BOOTSTRAP_OP_REGISTER = 1,
	NXU_BOOTSTRAP_OP_LOOKUP = 2
} nxu_bootstrap_op_t;

typedef struct {
	uint32_t opcode;
	char label[NXU_BOOTSTRAP_LABEL_MAX];
} nxu_bootstrap_request_t;

typedef enum {
	NXU_BOOTSTRAP_STATUS_OK = 0,
	NXU_BOOTSTRAP_STATUS_NOT_FOUND = 1
} nxu_bootstrap_status_t;

typedef struct {
	int32_t status;
} nxu_bootstrap_reply_t;

#endif
