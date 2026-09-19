#include <nxu/syscall.h>

#include <stdint.h>

/*
 * Throwaway socket echo client, spawned directly by
 * kern/tests/socket_process_test.c. Streams a payload deliberately larger
 * than NXPC's old 4096-byte inline limit (kern/ipc/ipc_types.h's
 * IPC_KMSG_MAX_INLINE_SIZE), in awkwardly-sized chunks, to prove this is a
 * real byte stream and not a relabeled bounded message.
 */

#define SOCKETTEST_NAME "com.nxu.test.sockettest"
#define SOCKETTEST_PAYLOAD_SIZE 6000ULL
#define SOCKETTEST_CHUNK_SIZE 777ULL
#define SOCKETTEST_CONNECT_ATTEMPTS 2000000U

static uint8_t g_send_buffer[SOCKETTEST_PAYLOAD_SIZE];
static uint8_t g_recv_buffer[SOCKETTEST_PAYLOAD_SIZE];

int
main(void)
{
	for (uint64_t index = 0ULL; index < SOCKETTEST_PAYLOAD_SIZE; index++) {
		g_send_buffer[index] = (uint8_t)(index * 7ULL + 3ULL);
	}

	int64_t fd = -1;
	for (uint32_t attempt = 0U; attempt < SOCKETTEST_CONNECT_ATTEMPTS; attempt++) {
		fd = nxu_socket_connect(SOCKETTEST_NAME);
		if (fd >= 0) break;
		(void)nxu_yield();
	}
	if (fd < 0) return 1;

	uint64_t sent = 0ULL;
	while (sent < SOCKETTEST_PAYLOAD_SIZE) {
		uint64_t remaining = SOCKETTEST_PAYLOAD_SIZE - sent;
		uint64_t chunk = remaining > SOCKETTEST_CHUNK_SIZE ? SOCKETTEST_CHUNK_SIZE : remaining;

		int64_t written = nxu_write((uint64_t)fd, g_send_buffer + sent, chunk);
		if (written <= 0) return 2;

		sent += (uint64_t)written;
	}

	uint64_t received = 0ULL;
	while (received < SOCKETTEST_PAYLOAD_SIZE) {
		int64_t got = nxu_read((uint64_t)fd, g_recv_buffer + received, SOCKETTEST_PAYLOAD_SIZE - received);
		if (got <= 0) return 3;
		received += (uint64_t)got;
	}

	for (uint64_t index = 0ULL; index < SOCKETTEST_PAYLOAD_SIZE; index++) {
		if (g_recv_buffer[index] != g_send_buffer[index]) return 4;
	}

	(void)nxu_close((uint64_t)fd);
	return 0;
}
