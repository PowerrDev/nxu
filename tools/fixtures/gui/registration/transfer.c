/* Run the production transfer adapter against an acknowledging mock server. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <frameworks/CoreGraphics.framework/lib/windowserver.c>

static uint32_t creates, maps, withdrawals, unmaps, sends;
static uint64_t next_id;
static unsigned char *memory;
static bool fail_send, fail_map, fail_create, reject, timeout;
static wsmsg_reply_t response;
static uint64_t clock_us;

bool
bootstrap_client_lookup(const char *label, uint32_t *port)
{
	(void)label;
	*port = 1;
	return true;
}

int64_t
nxu_ipc_port_allocate(void)
{
	return 2;
}

int64_t
nxu_ipc_port_deallocate(uint32_t port)
{
	(void)port;
	return 0;
}

int64_t
nxu_uptime_us(void)
{
	return clock_us;
}

int64_t
nxu_sleep_us(uint64_t delay)
{
	clock_us += delay;
	return 0;
}

int64_t
nxu_ipc_send(uint32_t port, const void *bytes, uint64_t size, uint32_t reply)
{
	(void)port;
	(void)size;
	(void)reply;
	sends++;
	const wsmsg_request_t *request = bytes;
	if (request->opcode == WSMSG_RENDER_WINDOW_SHM) {
		const wsmsg_render_window_shm_t *frame = &request->render_window_shm;
		assert(frame->shm_id == g_transfer_id && frame->stride == frame->width);
		const uint32_t *pixels = (const void *)memory;
		for (uint32_t y = 0; y < frame->height; y++)
			for (uint32_t x = 0; x < frame->width; x++)
				assert(pixels[y * frame->width + x] == y * 100 + x);
	}
	response = (wsmsg_reply_t){.status = reject ? -1 : 0};
	return fail_send ? -1 : 0;
}

int64_t
nxu_ipc_receive_timed(uint32_t port, void *buffer, uint64_t size, uint32_t *transfer, uint32_t *type,
					  uint32_t timeout_us)
{
	(void)port;
	(void)size;
	(void)transfer;
	(void)type;
	assert(timeout_us == 5000000);
	if (timeout) {
		clock_us += timeout_us;
		return -NXU_SYS_E_AGAIN;
	}
	memcpy(buffer, &response, sizeof(response));
	return sizeof(response);
}

int64_t
nxu_shm_create(uint64_t bytes)
{
	creates++;
	if (fail_create) return -1;
	memory = malloc(bytes);
	assert(memory);
	return ++next_id;
}

int64_t
nxu_shm_map(uint64_t id)
{
	(void)id;
	maps++;
	return fail_map ? -1 : (int64_t)(uintptr_t)memory;
}

int64_t
nxu_shm_withdraw(uint64_t id)
{
	(void)id;
	withdrawals++;
	if (fail_map) {
		free(memory);
		memory = NULL;
	}
	return 0;
}

int64_t
nxu_shm_unmap(uint64_t address)
{
	assert(address == (uint64_t)(uintptr_t)memory);
	unmaps++;
	free(memory);
	memory = NULL;
	return 0;
}

int
main(void)
{
	uint32_t pixels[32 * 32];
	for (uint32_t y = 0; y < 32; y++)
		for (uint32_t x = 0; x < 32; x++)
			pixels[y * 32 + x] = y * 100 + x;

	for (uint32_t i = 0; i < 1000; i++)
		assert(CG_Render_Window(1, pixels, 16, 16, 32));
	assert(creates == 1 && maps == 1 && !unmaps && !withdrawals && sends == 1000);
	puts("PASS 1000 padded-stride frames: one SHM allocation/map, no per-frame teardown");

	assert(CG_Render_Window(2, pixels, 32, 32, 32));
	assert(creates == 2 && unmaps == 1 && withdrawals == 1);
	assert(CG_Render_Window(1, pixels, 8, 8, 32) && creates == 2);
	assert(CG_Set_Cursor(0, pixels, 8, 8, 32, 0, 0) && creates == 2);
	assert(!CG_Render_Window(1, pixels, 33, 8, 32));
	assert(!CG_Render_Window(1, pixels, 3841, 8, 3841));

	reject = true;
	assert(!CG_Render_Window(1, pixels, 8, 8, 32) && !g_transfer_id);
	reject = false;
	fail_send = true;
	assert(!CG_Render_Window(1, pixels, 8, 8, 32) && !g_transfer_id);
	fail_send = false;
	timeout = true;
	assert(!CG_Render_Window(1, pixels, 8, 8, 32) && !g_transfer_id);
	timeout = false;
	assert(CG_Render_Window(1, pixels, 8, 8, 32));
	release_transfer();

	fail_map = true;
	assert(!CG_Render_Window(1, pixels, 8, 8, 32) && !g_transfer_id && !memory);
	fail_map = false;
	fail_create = true;
	assert(!CG_Render_Window(1, pixels, 8, 8, 32) && !g_transfer_id);
	/* Exercise the maximum frame size and reuse it without another allocation. */
	fail_create = false;
	uint32_t *large = malloc(3840ULL * 2160 * sizeof(*large));
	assert(large);
	for (uint32_t y = 0; y < 2160; y++)
		for (uint32_t x = 0; x < 3840; x++)
			large[y * 3840 + x] = y * 100 + x;
	assert(CG_Render_Window(1, large, 3840, 2160, 3840));
	uint32_t before = creates;
	assert(CG_Render_Window(1, large, 3840, 2160, 3840) && creates == before);
	assert(g_transfer_capacity == 3840ULL * 2160 * 4);
	free(large);
	release_transfer();
	puts("PASS full 4K frame transfer, exact capacity bound and reuse");
	puts("PASS resizing, bounds, rejected/failed/timed-out requests and allocation failure cleanup");
	return 0;
}
