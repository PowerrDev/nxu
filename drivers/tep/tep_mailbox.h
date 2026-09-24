/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_mailbox.h
 *
 * Trusted Enclave driver: NXU's side of the mailbox to tepOS, the security
 * OS on the Trusted Enclave Processor (a separate machine; see
 * tep_mailbox_proto.h). The only connection is a dedicated PL011 serial
 * link (platform->mailbox_uart); NXU never sees tepOS memory.
 *
 * A monitor thread handshakes with tepOS (HELLO), then checks its health
 * every second and marks the link unavailable after repeated failures.
 * Requests fail closed: while the link is not available they are refused at
 * once, and nothing in NXU substitutes for a tepOS answer.
 */

#ifndef NXU_DRIVERS_TEP_TEP_MAILBOX_H
#define NXU_DRIVERS_TEP_TEP_MAILBOX_H

#include <drivers/tep/tep_mailbox_proto.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	TEP_LINK_ABSENT = 0,        /* no mailbox UART on this machine */
	TEP_LINK_UNAVAILABLE,       /* link present, tepOS not answering (yet) */
	TEP_LINK_AVAILABLE          /* HELLO succeeded and health checks pass */
} tep_link_state_t;

typedef enum {
	TEP_REQ_OK = 0,             /* a response arrived; see its status */
	TEP_REQ_UNAVAILABLE,        /* refused: tepOS is not available */
	TEP_REQ_TIMEOUT,            /* no matching response in time */
	TEP_REQ_INVALID             /* bad arguments or a malformed response */
} tep_request_result_t;

typedef struct {
	uint16_t status;            /* tep_mb_status_t */
	uint16_t payload_len;
	uint8_t payload[TEP_MB_MAX_PAYLOAD];
} tep_mailbox_response_t;

/*
 * tep_mailbox_start:
 *
 * Find the mailbox UART and start the monitor thread. Without the UART the
 * link stays TEP_LINK_ABSENT and every request is refused. Returns true when
 * the monitor thread was started.
 */
bool tep_mailbox_start(void);

tep_link_state_t tep_mailbox_link_state(void);

/*
 * tep_mailbox_request:
 *
 * Send one request and wait for its response. Refused with
 * TEP_REQ_UNAVAILABLE unless the link is TEP_LINK_AVAILABLE. Callers must
 * treat anything but TEP_REQ_OK with status TEP_MB_OK as a failure.
 */
tep_request_result_t tep_mailbox_request(uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response);

/*
 * tep_mailbox_health:
 *
 * The health tepOS last reported. Returns false when the link is not
 * available (the last report is then stale and not returned).
 */
bool tep_mailbox_health(tep_mb_health_t *health);

/*
 * tep_mailbox_features:
 *
 * The TEP_MB_FEATURE_* flags tepOS advertised in its last HELLO, or 0 when
 * the link is not available.
 */
uint16_t tep_mailbox_features(void);

const char *tep_link_state_name(tep_link_state_t state);
const char *tep_request_result_name(tep_request_result_t result);
const char *tep_mb_health_name(uint8_t health);

#endif
