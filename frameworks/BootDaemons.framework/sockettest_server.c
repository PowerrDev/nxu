#include <nxu/syscall.h>

#include <stdint.h>

/*
 * Throwaway socket echo server, spawned directly by
 * kern/tests/socket_process_test.c. Paired with sockettest_client.c;
 * rendezvous happens purely through kern/ipc/socket.h's named registry
 * (NXU_SYS_SOCKET_LISTEN/_CONNECT), not NXPC.
 */

#define SOCKETTEST_NAME "com.nxu.test.sockettest"
#define SOCKETTEST_PAYLOAD_SIZE 6000ULL

static uint8_t g_buffer[512];

int
main(void)
{
	int64_t listen_fd = nxu_socket_listen(SOCKETTEST_NAME, 1U);
	if (listen_fd < 0) return 1;

	int64_t conn_fd = nxu_socket_accept((uint64_t)listen_fd);
	if (conn_fd < 0) return 2;

	uint64_t total = 0ULL;
	while (total < SOCKETTEST_PAYLOAD_SIZE) {
		int64_t received = nxu_read((uint64_t)conn_fd, g_buffer, sizeof(g_buffer));
		if (received <= 0) return 3;

		int64_t sent = nxu_write((uint64_t)conn_fd, g_buffer, (uint64_t)received);
		if (sent != received) return 4;

		total += (uint64_t)received;
	}

	(void)nxu_close((uint64_t)conn_fd);
	(void)nxu_close((uint64_t)listen_fd);

	return 0;
}
