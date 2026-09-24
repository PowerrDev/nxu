/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_mailbox_proto.h
 *
 * The NXU <-> tepOS mailbox protocol, version 1: NXU's copy of tepOS's
 * boot/include/tep/mailbox.h (TrustedEnclaveProcessor repository), which is
 * the reference. The two must stay identical in value; HELLO checks the
 * version at run time.
 *
 * Frame, all fields little endian:
 *
 *   off  size  field
 *     0     2  magic          TEP_MB_MAGIC ("TP")
 *     2     1  version        TEP_MB_VERSION
 *     3     1  type           TEP_MB_TYPE_REQUEST / TEP_MB_TYPE_RESPONSE
 *     4     2  command        tep_mb_command_t
 *     6     2  status         tep_mb_status_t (0 in requests)
 *     8     4  request_id     chosen by NXU, echoed in the response
 *    12     2  payload_len    <= TEP_MB_MAX_PAYLOAD, exact per command
 *    14     2  reserved       0
 *    16     n  payload
 *  16+n     4  crc32          IEEE 802.3 CRC-32 over bytes 0 .. 16+n-1
 *
 * The CRC only detects corruption on the link; it is not authentication.
 * No command reads or writes memory on either side.
 */

#ifndef NXU_DRIVERS_TEP_TEP_MAILBOX_PROTO_H
#define NXU_DRIVERS_TEP_TEP_MAILBOX_PROTO_H

#define TEP_MB_MAGIC 0x5054U
#define TEP_MB_VERSION 1U
#define TEP_MB_HEADER_LEN 16U
#define TEP_MB_CRC_LEN 4U
#define TEP_MB_MAX_PAYLOAD 240U
#define TEP_MB_MAX_FRAME (TEP_MB_HEADER_LEN + TEP_MB_MAX_PAYLOAD + TEP_MB_CRC_LEN)

#define TEP_MB_TYPE_REQUEST 1U
#define TEP_MB_TYPE_RESPONSE 2U

typedef enum {
	TEP_MB_CMD_HELLO = 0x0001,
	TEP_MB_CMD_GET_HEALTH = 0x0002,

	/* TEP_MB_FEATURE_CRYPTO */
	TEP_MB_CMD_SHA256 = 0x0010,       /* 1..240 bytes -> 32-byte digest */
	TEP_MB_CMD_RANDOM = 0x0011,       /* u16 n (1..64) -> n bytes */
	TEP_MB_CMD_KEY_GENERATE = 0x0020, /* u8 algorithm -> u32 handle */
	TEP_MB_CMD_KEY_PUBLIC = 0x0021,   /* u32 handle -> 32-byte public key */
	TEP_MB_CMD_KEY_SIGN = 0x0022,     /* u32 handle, 1..224 bytes -> 64-byte signature */
	TEP_MB_CMD_KEY_DELETE = 0x0023    /* u32 handle -> empty */
} tep_mb_command_t;

#define TEP_MB_ALG_ED25519 1U
#define TEP_MB_SHA256_MAX 240U
#define TEP_MB_RANDOM_MAX 64U
#define TEP_MB_SIGN_MAX 224U

typedef enum {
	TEP_MB_OK = 0,
	TEP_MB_BAD_VERSION = 1,
	TEP_MB_BAD_COMMAND = 2,
	TEP_MB_BAD_LENGTH = 3,
	TEP_MB_UNAVAILABLE = 4,
	TEP_MB_INTERNAL = 5,
	TEP_MB_NOT_FOUND = 6,
	TEP_MB_FULL = 7
} tep_mb_status_t;

/* HELLO response: u16 protocol, u16 flags, u32 tepOS version (major << 16 | minor << 8 | patch), u32 boot id. */
#define TEP_MB_HELLO_LEN 12U
#define TEP_MB_FEATURE_CRYPTO (1U << 0U) /* SHA256, RANDOM and the KEY_* commands */

/* GET_HEALTH response: u8 health, u8 n, u16 reserved, then n x (u8 id, u8 state, u8 restarts, u8 reserved). */
#define TEP_MB_MAX_SERVICES 8U
#define TEP_MB_HEALTH_LEN(n) (4U + 4U * (n))

typedef enum {
	TEP_MB_HEALTH_STARTING = 0,
	TEP_MB_HEALTH_OK = 1,
	TEP_MB_HEALTH_DEGRADED = 2,
	TEP_MB_HEALTH_FAILED = 3
} tep_mb_health_t;

typedef enum {
	TEP_MB_SVC_STOPPED = 0,
	TEP_MB_SVC_STARTING = 1,
	TEP_MB_SVC_READY = 2,
	TEP_MB_SVC_FAILED = 3,
	TEP_MB_SVC_DISABLED = 4
} tep_mb_service_state_t;

#endif
