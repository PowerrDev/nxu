#include <drivers/virtio/virtio_gpu.h>

#include <drivers/video/display.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <mach/machine/barrier.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTIO_GPU_CONTROLQ 0U
#define VIRTIO_GPU_QUEUE_SIZE 32U
#define VIRTIO_GPU_MAX_SCANOUTS 16U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101U
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101U

#define VIRTIO_GPU_FLAG_FENCE 0x00000001U
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2U

#define VIRTIO_GPU_RESOURCE_ID 1U
#define VIRTIO_GPU_BYTES_PER_PIXEL 4U
#define VIRTIO_GPU_MAX_WIDTH 4096U
#define VIRTIO_GPU_MAX_HEIGHT 2160U
#define VIRTIO_GPU_MAX_FRAMEBUFFER_BYTES (64ULL * 1024ULL * 1024ULL)

#define VIRTIO_GPU_CONTROL_REQUEST_OFFSET 0U
#define VIRTIO_GPU_CONTROL_RESPONSE_OFFSET 2048U
#define VIRTIO_GPU_CONTROL_RESPONSE_CAPACITY (PMM_PAGE_SIZE - VIRTIO_GPU_CONTROL_RESPONSE_OFFSET)

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1U

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t ring_idx;
	uint8_t padding[3];
} virtio_gpu_ctrl_hdr_t;

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
} virtio_gpu_rect_t;

typedef struct {
	virtio_gpu_rect_t rect;
	uint32_t enabled;
	uint32_t flags;
} virtio_gpu_display_one_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	virtio_gpu_display_one_t scanouts[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	uint32_t resource_id;
	uint32_t format;
	uint32_t width;
	uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct {
	uint64_t address;
	uint32_t length;
	uint32_t padding;
} virtio_gpu_mem_entry_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	uint32_t resource_id;
	uint32_t entry_count;
	virtio_gpu_mem_entry_t entry;
} virtio_gpu_resource_attach_backing_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	virtio_gpu_rect_t rect;
	uint32_t scanout_id;
	uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	virtio_gpu_rect_t rect;
	uint64_t offset;
	uint32_t resource_id;
	uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct {
	virtio_gpu_ctrl_hdr_t hdr;
	virtio_gpu_rect_t rect;
	uint32_t resource_id;
	uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct {
	virtio_device_t transport;
	virtqueue_t controlq;

	uint64_t control_physical;
	uint8_t *control;

	uint64_t framebuffer_physical;
	uint64_t framebuffer_pages;
	uint64_t framebuffer_bytes;
	uint32_t *framebuffer;

	uint32_t scanout_id;
	uint32_t width;
	uint32_t height;
	uint64_t next_fence_id;
	uint64_t present_count;
	volatile uint32_t lock;

	display_device_t display;
	bool attached;
} virtio_gpu_device_t;

_Static_assert(sizeof(virtio_gpu_ctrl_hdr_t) == 24U, "VirtIO GPU header size mismatch");
_Static_assert(sizeof(virtio_gpu_rect_t) == 16U, "VirtIO GPU rectangle size mismatch");
_Static_assert(sizeof(virtio_gpu_resp_display_info_t) == 408U, "VirtIO GPU display-info size mismatch");
_Static_assert(sizeof(virtio_gpu_resource_create_2d_t) == 40U, "VirtIO GPU create-2D size mismatch");
_Static_assert(sizeof(virtio_gpu_resource_attach_backing_t) == 48U, "VirtIO GPU attach-backing size mismatch");
_Static_assert(sizeof(virtio_gpu_set_scanout_t) == 48U, "VirtIO GPU set-scanout size mismatch");
_Static_assert(sizeof(virtio_gpu_transfer_to_host_2d_t) == 56U, "VirtIO GPU transfer size mismatch");
_Static_assert(sizeof(virtio_gpu_resource_flush_t) == 48U, "VirtIO GPU flush size mismatch");

static virtio_gpu_device_t g_virtio_gpu_devices[VIRTIO_GPU_MAX_DEVICES];
static uint32_t g_virtio_gpu_device_count;
static ioreg_id_t g_virtio_gpu_ioreg_family;

static ioreg_id_t virtio_gpu_ioreg_family(void)
{
	if (g_virtio_gpu_ioreg_family == 0U) {
		g_virtio_gpu_ioreg_family = ioreg_add(ioreg_family_graphics(), "VirtIOGPUFamily", "VirtIOGPUFamily");
	}
	return g_virtio_gpu_ioreg_family;
}

static void virtio_gpu_lock(virtio_gpu_device_t *device)
{
	while (__atomic_exchange_n(&device->lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		ml_cpu_relax();
	}
}

static void virtio_gpu_unlock(virtio_gpu_device_t *device)
{
	__atomic_store_n(&device->lock, 0U, __ATOMIC_RELEASE);
}

static bool virtio_gpu_allocate_page(uint64_t *physical, uint8_t **virtual_address)
{
	if (!pmm_allocate_page(physical)) return false;

	uint64_t higher_half;
	if (!vmm_physical_to_higher_half(*physical, &higher_half)) {
		(void)pmm_free_page(*physical);
		*physical = 0ULL;
		return false;
	}

	*virtual_address = (uint8_t *)higher_half;
	memset(*virtual_address, 0, PMM_PAGE_SIZE);
	return true;
}

static bool virtio_gpu_wait(virtio_gpu_device_t *device, uint16_t expected_head)
{
	for (;;) {
		uint32_t id;
		uint32_t length;

		if (!virtqueue_pop_used(&device->controlq, &id, &length)) {
			ml_cpu_relax();
			continue;
		}

		if (length < sizeof(virtio_gpu_ctrl_hdr_t)) return false;
		return id == expected_head;
	}
}

static bool virtio_gpu_command(
	virtio_gpu_device_t *device,
	void *request,
	uint32_t request_size,
	uint32_t expected_response,
	uint32_t response_capacity
)
{
	if (
		device == 0 ||
		request == 0 ||
		request_size < sizeof(virtio_gpu_ctrl_hdr_t) ||
		request_size > VIRTIO_GPU_CONTROL_RESPONSE_OFFSET ||
		response_capacity < sizeof(virtio_gpu_ctrl_hdr_t) ||
		response_capacity > VIRTIO_GPU_CONTROL_RESPONSE_CAPACITY
	) {
		return false;
	}

	virtio_gpu_lock(device);

	virtio_gpu_ctrl_hdr_t *request_header = request;
	uint64_t fence_id = ++device->next_fence_id;
	request_header->flags |= VIRTIO_GPU_FLAG_FENCE;
	request_header->fence_id = fence_id;

	memcpy(device->control + VIRTIO_GPU_CONTROL_REQUEST_OFFSET, request, request_size);
	memset(device->control + VIRTIO_GPU_CONTROL_RESPONSE_OFFSET, 0, response_capacity);

	uint16_t request_desc;
	uint16_t response_desc;

	if (!virtqueue_alloc_descriptor(&device->controlq, &request_desc)) {
		virtio_gpu_unlock(device);
		return false;
	}

	if (!virtqueue_alloc_descriptor(&device->controlq, &response_desc)) {
		(void)virtqueue_free_descriptor(&device->controlq, request_desc);
		virtio_gpu_unlock(device);
		return false;
	}

	virtq_desc_t *out = &device->controlq.descriptors[request_desc];
	out->address = device->control_physical + VIRTIO_GPU_CONTROL_REQUEST_OFFSET;
	out->length = request_size;
	out->flags = VIRTQ_DESC_F_NEXT;
	out->next = response_desc;

	virtq_desc_t *in = &device->controlq.descriptors[response_desc];
	in->address = device->control_physical + VIRTIO_GPU_CONTROL_RESPONSE_OFFSET;
	in->length = response_capacity;
	in->flags = VIRTQ_DESC_F_WRITE;
	in->next = 0U;

	bool ok = virtqueue_submit(&device->controlq, request_desc);
	if (ok) {
		virtio_device_notify(&device->transport, VIRTIO_GPU_CONTROLQ);
		ok = virtio_gpu_wait(device, request_desc);
	}

	virtio_gpu_ctrl_hdr_t *response = (virtio_gpu_ctrl_hdr_t *)(device->control + VIRTIO_GPU_CONTROL_RESPONSE_OFFSET);

	if (ok) {
		ok = response->type == expected_response &&
			(response->flags & VIRTIO_GPU_FLAG_FENCE) != 0U &&
			response->fence_id == fence_id;
	}

	(void)virtqueue_free_descriptor(&device->controlq, response_desc);
	(void)virtqueue_free_descriptor(&device->controlq, request_desc);
	virtio_gpu_unlock(device);
	return ok;
}

static bool virtio_gpu_get_display_info(virtio_gpu_device_t *device)
{
	virtio_gpu_ctrl_hdr_t request;
	memset(&request, 0, sizeof(request));
	request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

	if (!virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
		sizeof(virtio_gpu_resp_display_info_t)
	)) {
		return false;
	}

	const virtio_gpu_resp_display_info_t *response = (const virtio_gpu_resp_display_info_t *)(device->control + VIRTIO_GPU_CONTROL_RESPONSE_OFFSET);

	for (uint32_t index = 0U; index < VIRTIO_GPU_MAX_SCANOUTS; index++) {
		const virtio_gpu_display_one_t *mode = &response->scanouts[index];
		if (mode->enabled == 0U || mode->rect.width == 0U || mode->rect.height == 0U) continue;

		if (mode->rect.width > VIRTIO_GPU_MAX_WIDTH || mode->rect.height > VIRTIO_GPU_MAX_HEIGHT) return false;

		device->scanout_id = index;
		device->width = mode->rect.width;
		device->height = mode->rect.height;
		return true;
	}

	return false;
}

static bool virtio_gpu_create_resource(virtio_gpu_device_t *device)
{
	virtio_gpu_resource_create_2d_t request;
	memset(&request, 0, sizeof(request));
	request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
	request.resource_id = VIRTIO_GPU_RESOURCE_ID;
	request.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	request.width = device->width;
	request.height = device->height;

	return virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_NODATA,
		sizeof(virtio_gpu_ctrl_hdr_t)
	);
}

static bool virtio_gpu_attach_backing(virtio_gpu_device_t *device)
{
	if (device->framebuffer_bytes > UINT32_MAX) return false;

	virtio_gpu_resource_attach_backing_t request;
	memset(&request, 0, sizeof(request));
	request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
	request.resource_id = VIRTIO_GPU_RESOURCE_ID;
	request.entry_count = 1U;
	request.entry.address = device->framebuffer_physical;
	request.entry.length = (uint32_t)device->framebuffer_bytes;

	return virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_NODATA,
		sizeof(virtio_gpu_ctrl_hdr_t)
	);
}

static bool virtio_gpu_set_scanout(virtio_gpu_device_t *device)
{
	virtio_gpu_set_scanout_t request;
	memset(&request, 0, sizeof(request));
	request.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
	request.rect.width = device->width;
	request.rect.height = device->height;
	request.scanout_id = device->scanout_id;
	request.resource_id = VIRTIO_GPU_RESOURCE_ID;

	return virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_NODATA,
		sizeof(virtio_gpu_ctrl_hdr_t)
	);
}

static bool virtio_gpu_transfer(
	virtio_gpu_device_t *device,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	virtio_gpu_transfer_to_host_2d_t request;
	memset(&request, 0, sizeof(request));
	request.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
	request.rect.x = x;
	request.rect.y = y;
	request.rect.width = width;
	request.rect.height = height;
	request.offset = ((uint64_t)y * device->width + x) * VIRTIO_GPU_BYTES_PER_PIXEL;
	request.resource_id = VIRTIO_GPU_RESOURCE_ID;

	return virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_NODATA,
		sizeof(virtio_gpu_ctrl_hdr_t)
	);
}

static bool virtio_gpu_flush(
	virtio_gpu_device_t *device,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	virtio_gpu_resource_flush_t request;
	memset(&request, 0, sizeof(request));
	request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
	request.rect.x = x;
	request.rect.y = y;
	request.rect.width = width;
	request.rect.height = height;
	request.resource_id = VIRTIO_GPU_RESOURCE_ID;

	return virtio_gpu_command(
		device,
		&request,
		sizeof(request),
		VIRTIO_GPU_RESP_OK_NODATA,
		sizeof(virtio_gpu_ctrl_hdr_t)
	);
}

static bool virtio_gpu_display_present(
	display_device_t *display,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height
)
{
	if (display == 0 || display->driver == 0) return false;
	virtio_gpu_device_t *device = display->driver;

	if (!virtio_gpu_transfer(device, x, y, width, height)) return false;

	if (!virtio_gpu_flush(device, x, y, width, height)) return false;
	device->present_count++;
	return true;
}

static bool virtio_gpu_allocate_framebuffer(virtio_gpu_device_t *device)
{
	uint64_t pixels = (uint64_t)device->width * device->height;
	if (pixels > UINT64_MAX / VIRTIO_GPU_BYTES_PER_PIXEL) return false;

	uint64_t bytes = pixels * VIRTIO_GPU_BYTES_PER_PIXEL;
	if (bytes == 0ULL || bytes > VIRTIO_GPU_MAX_FRAMEBUFFER_BYTES) return false;

	uint64_t pages = (bytes + PMM_PAGE_SIZE - 1ULL) / PMM_PAGE_SIZE;
	uint64_t physical;
	if (!pmm_allocate_contiguous_pages(pages, &physical)) return false;

	uint64_t higher_half;
	if (!vmm_physical_to_higher_half(physical, &higher_half)) {
		(void)pmm_free_contiguous_pages(physical, pages);
		return false;
	}

	device->framebuffer_physical = physical;
	device->framebuffer_pages = pages;
	device->framebuffer_bytes = bytes;
	device->framebuffer = (uint32_t *)higher_half;
	return true;
}

bool virtio_gpu_attach(const virtio_device_t *transport)
{
	if (transport == 0 || transport->device_id != VIRTIO_DEVICE_ID_GPU) return false;

	if (g_virtio_gpu_device_count >= VIRTIO_GPU_MAX_DEVICES) return false;

	virtio_gpu_device_t *device = &g_virtio_gpu_devices[g_virtio_gpu_device_count];
	const char *failure_stage = "transport initialization";
	memset(device, 0, sizeof(*device));
	device->transport = *transport;

	kprintf("VirtIOGPUFamily: probe 0x%llx, INTID %u\n", (unsigned long long)transport->region.base, transport->intid);

	if (!virtio_device_begin(&device->transport, 0ULL)) {
		kputln("VirtIOGPUFamily: transport feature negotiation failed before queue setup");
		return false;
	}

	/*
	 * NXU composites the pointer in software, so the early display driver only
	 * claims controlq. QEMU does not require cursorq for 2D scanout commands.
	 */
	failure_stage = "control queue allocation";
	if (!virtio_device_queue_init(&device->transport, VIRTIO_GPU_CONTROLQ, VIRTIO_GPU_QUEUE_SIZE, &device->controlq)) goto fail;

	*device->controlq.available_flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

	failure_stage = "control queue transport setup";
	if (!virtio_device_setup_queue(&device->transport, VIRTIO_GPU_CONTROLQ, &device->controlq)) goto fail;

	failure_stage = "control request page allocation";
	if (!virtio_gpu_allocate_page(&device->control_physical, &device->control)) goto fail;
	failure_stage = "DRIVER_OK transition";
	if (!virtio_device_finish(&device->transport)) goto fail;

	failure_stage = "GET_DISPLAY_INFO";
	if (!virtio_gpu_get_display_info(device)) goto fail;
	failure_stage = "framebuffer allocation";
	if (!virtio_gpu_allocate_framebuffer(device)) goto fail;
	failure_stage = "RESOURCE_CREATE_2D";
	if (!virtio_gpu_create_resource(device)) goto fail;

	failure_stage = "RESOURCE_ATTACH_BACKING";
	if (!virtio_gpu_attach_backing(device)) goto fail;

	failure_stage = "SET_SCANOUT";
	if (!virtio_gpu_set_scanout(device)) goto fail;

	device->display.framebuffer = device->framebuffer;
	device->display.width = device->width;
	device->display.height = device->height;
	device->display.stride = device->width;
	device->display.bytes_per_pixel = VIRTIO_GPU_BYTES_PER_PIXEL;
	device->display.driver = device;
	device->display.present = virtio_gpu_display_present;

	/*
	 * No initial clear/present here: the guest-side framebuffer is left
	 * unflushed on purpose so the scanout shows nothing until the first
	 * real caller (the boot splash) paints and flushes it -- avoiding an
	 * extra, unnecessary frame between SET_SCANOUT and that first paint.
	 * The command round-trips above (GET_DISPLAY_INFO, RESOURCE_CREATE_2D,
	 * RESOURCE_ATTACH_BACKING, SET_SCANOUT) already exercise the same
	 * virtqueue plumbing TRANSFER_TO_HOST_2D/RESOURCE_FLUSH would, so
	 * skipping it here does not skip meaningful validation.
	 */

	failure_stage = "display registry insertion";
	if (!display_register(&device->display)) goto fail;

	device->attached = true;
	g_virtio_gpu_device_count++;
	kprintf("VirtIOGPUFamily: scanout %u online at %ux%u, framebuffer %llu bytes\n", device->scanout_id, device->width, device->height, (unsigned long long)device->framebuffer_bytes);
	(void)ioreg_add(virtio_gpu_ioreg_family(), "Display0", "VirtIOGPUDevice");
	return true;

fail:
	kprintf("VirtIOGPUFamily: attach failed during %s\n", failure_stage);
	virtio_device_fail(&device->transport);

	if (device->framebuffer_pages != 0ULL) {
		(void)pmm_free_contiguous_pages(device->framebuffer_physical, device->framebuffer_pages);
	}

	if (device->control_physical != 0ULL) (void)pmm_free_page(device->control_physical);
	if (device->controlq.initialized) (void)virtqueue_destroy(&device->controlq);
	memset(device, 0, sizeof(*device));
	return false;
}

uint32_t virtio_gpu_device_count(void)
{
	return g_virtio_gpu_device_count;
}

void virtio_gpu_dump(void)
{
	for (uint32_t index = 0U; index < g_virtio_gpu_device_count; index++) {
		const virtio_gpu_device_t *device = &g_virtio_gpu_devices[index];
		kprintf(
			"VirtIOGPUFamily: device %u, scanout %u, %ux%u, framebuffer %llu bytes, presents %llu\n",
			index + 1U,
			device->scanout_id,
			device->width,
			device->height,
			(unsigned long long)device->framebuffer_bytes,
			(unsigned long long)device->present_count
		);
	}
}
