#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <drivers/virtio/virtio_block.h>

#include <drivers/block/block_device.h>
#include <kern/machine/barrier.h>
#include <platform/uart.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTIO_BLK_REQUESTQ 0U

#define VIRTIO_BLK_F_RO 5U
#define VIRTIO_BLK_F_BLK_SIZE 6U
#define VIRTIO_BLK_F_FLUSH 9U

#define VIRTIO_BLK_T_IN 0U
#define VIRTIO_BLK_T_OUT 1U
#define VIRTIO_BLK_T_FLUSH 4U

#define VIRTIO_BLK_S_OK 0U
#define VIRTIO_BLK_S_IOERR 1U
#define VIRTIO_BLK_S_UNSUPP 2U
#define VIRTIO_BLK_S_PENDING 0xFFU

#define VIRTIO_BLK_CONFIG_CAPACITY 0U
#define VIRTIO_BLK_CONFIG_BLK_SIZE 20U

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1U

_Static_assert(
	sizeof(virtio_block_request_header_t) == 16U,
	"VirtIO block request header must match the wire format"
);

static virtio_block_device_t
	g_virtio_block_devices[VIRTIO_BLOCK_MAX_DEVICES];

static uint32_t g_virtio_block_device_count;
static ioreg_id_t g_virtio_block_ioreg_family;

static ioreg_id_t virtio_block_ioreg_family(void)
{
	if (g_virtio_block_ioreg_family == 0U) {
		g_virtio_block_ioreg_family = ioreg_add(ioreg_family_storage(), "VirtIOBlockFamily", "VirtIOBlockFamily");
	}
	return g_virtio_block_ioreg_family;
}

/*
 * virtio_block_lock:
 *
 * Serialize the single request queue and shared DMA bounce page. Current
 * NXU kernel execution is not preempted while executing ordinary EL1 code,
 * so this lock cannot be stranded by another block caller on the same CPU.
 */
static void virtio_block_lock(
	virtio_block_device_t *device
)
{
	while (
		__atomic_exchange_n(
			&device->lock,
			1U,
			__ATOMIC_ACQUIRE
		) != 0U
	) {
		ml_cpu_relax();
	}
}

static void virtio_block_unlock(
	virtio_block_device_t *device
)
{
	__atomic_store_n(
		&device->lock,
		0U,
		__ATOMIC_RELEASE
	);
}

/*
 * virtio_block_allocate_page:
 *
 * Allocate one DMA-visible physical page and return its permanent direct-map
 * alias to the driver.
 */
static bool virtio_block_allocate_page(
	uint64_t *physical,
	uint8_t **virtual_address
)
{
	if (!pmm_allocate_page(physical)) {
		return false;
	}

	uint64_t higher_half;

	if (!vmm_physical_to_higher_half(
		*physical,
		&higher_half
	)) {
		(void)pmm_free_page(*physical);

		*physical = 0ULL;

		return false;
	}

	*virtual_address = (uint8_t *)higher_half;

	memset(
		*virtual_address,
		0,
		PMM_PAGE_SIZE
	);

	return true;
}

/*
 * virtio_block_alloc_chain:
 *
 * Reserve the three descriptors used by a normal read/write request. Partial
 * allocation is rolled back before failure is returned.
 */
static bool virtio_block_alloc_chain(
	virtio_block_device_t *device,
	uint16_t *header,
	uint16_t *data,
	uint16_t *status
)
{
	if (!virtqueue_alloc_descriptor(
		&device->requestq,
		header
	)) {
		return false;
	}

	if (!virtqueue_alloc_descriptor(
		&device->requestq,
		data
	)) {
		(void)virtqueue_free_descriptor(
			&device->requestq,
			*header
		);

		return false;
	}

	if (!virtqueue_alloc_descriptor(
		&device->requestq,
		status
	)) {
		(void)virtqueue_free_descriptor(
			&device->requestq,
			*data
		);

		(void)virtqueue_free_descriptor(
			&device->requestq,
			*header
		);

		return false;
	}

	return true;
}

/*
 * virtio_block_free_chain:
 *
 * Return a completed three-descriptor request chain to the queue free list.
 */
static void virtio_block_free_chain(
	virtio_block_device_t *device,
	uint16_t header,
	uint16_t data,
	uint16_t status
)
{
	(void)virtqueue_free_descriptor(
		&device->requestq,
		status
	);

	(void)virtqueue_free_descriptor(
		&device->requestq,
		data
	);

	(void)virtqueue_free_descriptor(
		&device->requestq,
		header
	);
}

/*
 * virtio_block_wait:
 *
 * Poll requestq until the submitted descriptor head is returned by the
 * device. Queue interrupts are suppressed for this synchronous driver.
 */
static bool virtio_block_wait(
	virtio_block_device_t *device,
	uint16_t expected_head
)
{
	for (;;) {
		uint32_t id;
		uint32_t length;

		if (!virtqueue_pop_used(
			&device->requestq,
			&id,
			&length
		)) {
			ml_cpu_relax();

			continue;
		}

		(void)length;

		return id == expected_head;
	}
}

/*
 * virtio_block_transfer_one:
 *
 * Execute one request using the persistent control and bounce pages. count is
 * bounded so the data descriptor never crosses the 4 KiB bounce page.
 */
static bool virtio_block_transfer_one(
	virtio_block_device_t *device,
	bool write,
	uint64_t sector,
	uint32_t count,
	void *buffer
)
{
	if (
		count == 0U ||
		count >
			VIRTIO_BLOCK_MAX_SECTORS_PER_REQUEST
	) {
		return false;
	}

	uint32_t bytes = count * BLOCK_SECTOR_SIZE;

	if (write) {
		memcpy(
			device->bounce,
			buffer,
			bytes
		);
	}

	device->header->type =
		write
			? VIRTIO_BLK_T_OUT
			: VIRTIO_BLK_T_IN;

	device->header->reserved = 0U;
	device->header->sector = sector;

	*device->status = VIRTIO_BLK_S_PENDING;

	uint16_t header_desc;
	uint16_t data_desc;
	uint16_t status_desc;

	if (!virtio_block_alloc_chain(
		device,
		&header_desc,
		&data_desc,
		&status_desc
	)) {
		return false;
	}

	virtq_desc_t *header =
		&device->requestq.descriptors[
			header_desc
		];

	header->address = device->control_physical;

	header->length = sizeof(*device->header);

	header->flags = VIRTQ_DESC_F_NEXT;

	header->next = data_desc;

	virtq_desc_t *data =
		&device->requestq.descriptors[
			data_desc
		];

	data->address = device->bounce_physical;

	data->length = bytes;

	data->flags = VIRTQ_DESC_F_NEXT;

	if (!write) {
		data->flags |= VIRTQ_DESC_F_WRITE;
	}

	data->next = status_desc;

	virtq_desc_t *status =
		&device->requestq.descriptors[
			status_desc
		];

	status->address =
		device->control_physical +
		sizeof(*device->header);

	status->length = sizeof(*device->status);

	status->flags = VIRTQ_DESC_F_WRITE;

	status->next = 0U;

	ml_dma_wmb();

	if (!virtqueue_submit(
		&device->requestq,
		header_desc
	)) {
		virtio_block_free_chain(
			device,
			header_desc,
			data_desc,
			status_desc
		);

		return false;
	}

	virtio_device_notify(
		&device->transport,
		VIRTIO_BLK_REQUESTQ
	);

	bool completed =
		virtio_block_wait(
			device,
			header_desc
		);

	ml_dma_rmb();

	uint8_t request_status = *device->status;

	if (
		completed &&
		request_status == VIRTIO_BLK_S_OK &&
		!write
	) {
		memcpy(
			buffer,
			device->bounce,
			bytes
		);
	}

	virtio_block_free_chain(
		device,
		header_desc,
		data_desc,
		status_desc
	);

	device->request_count++;

	if (write) {
		device->write_request_count++;
	} else {
		device->read_request_count++;
	}

	return
		completed &&
		request_status == VIRTIO_BLK_S_OK;
}

/*
 * virtio_block_transfer:
 *
 * Split arbitrary block-layer requests into DMA-page-sized VirtIO requests.
 */
static bool virtio_block_transfer(
	virtio_block_device_t *device,
	bool write,
	uint64_t sector,
	uint32_t count,
	void *buffer
)
{
	uint8_t *bytes = buffer;

	uint32_t remaining = count;

	virtio_block_lock(device);

	while (remaining != 0U) {
		uint32_t chunk =
			remaining >
				VIRTIO_BLOCK_MAX_SECTORS_PER_REQUEST
				? VIRTIO_BLOCK_MAX_SECTORS_PER_REQUEST
				: remaining;

		if (!virtio_block_transfer_one(
			device,
			write,
			sector,
			chunk,
			bytes
		)) {
			virtio_block_unlock(device);

			return false;
		}

		uint32_t chunk_bytes = chunk * BLOCK_SECTOR_SIZE;

		sector += chunk;
		remaining -= chunk;
		bytes += chunk_bytes;
	}

	virtio_block_unlock(device);

	return true;
}

static bool virtio_block_read(
	block_device_t block,
	uint64_t sector,
	uint32_t count,
	void *buffer
)
{
	virtio_block_device_t *device = block->driver_data;

	if (
		device == 0 ||
		!device->attached
	) {
		return false;
	}

	return virtio_block_transfer(
		device,
		false,
		sector,
		count,
		buffer
	);
}

static bool virtio_block_write(
	block_device_t block,
	uint64_t sector,
	uint32_t count,
	const void *buffer
)
{
	virtio_block_device_t *device = block->driver_data;

	if (
		device == 0 ||
		!device->attached ||
		device->read_only
	) {
		return false;
	}

	return virtio_block_transfer(
		device,
		true,
		sector,
		count,
		(void *)buffer
	);
}

/*
 * virtio_block_flush_request:
 *
 * Submit the two-descriptor FLUSH request defined by VirtIO Block. No data
 * descriptor is present and sector is required to be zero.
 */
static bool virtio_block_flush_request(
	virtio_block_device_t *device
)
{
	if (
		!device->flush_supported ||
		device->read_only
	) {
		return true;
	}

	virtio_block_lock(device);

	device->header->type = VIRTIO_BLK_T_FLUSH;

	device->header->reserved = 0U;

	device->header->sector = 0ULL;

	*device->status = VIRTIO_BLK_S_PENDING;

	uint16_t header_desc;
	uint16_t status_desc;

	if (!virtqueue_alloc_descriptor(
		&device->requestq,
		&header_desc
	)) {
		virtio_block_unlock(device);

		return false;
	}

	if (!virtqueue_alloc_descriptor(
		&device->requestq,
		&status_desc
	)) {
		(void)virtqueue_free_descriptor(
			&device->requestq,
			header_desc
		);

		virtio_block_unlock(device);

		return false;
	}

	virtq_desc_t *header =
		&device->requestq.descriptors[
			header_desc
		];

	header->address = device->control_physical;

	header->length = sizeof(*device->header);

	header->flags = VIRTQ_DESC_F_NEXT;

	header->next = status_desc;

	virtq_desc_t *status =
		&device->requestq.descriptors[
			status_desc
		];

	status->address =
		device->control_physical +
		sizeof(*device->header);

	status->length = sizeof(*device->status);

	status->flags = VIRTQ_DESC_F_WRITE;

	status->next = 0U;

	ml_dma_wmb();

	if (!virtqueue_submit(
		&device->requestq,
		header_desc
	)) {
		(void)virtqueue_free_descriptor(
			&device->requestq,
			status_desc
		);

		(void)virtqueue_free_descriptor(
			&device->requestq,
			header_desc
		);

		virtio_block_unlock(device);

		return false;
	}

	virtio_device_notify(
		&device->transport,
		VIRTIO_BLK_REQUESTQ
	);

	bool completed =
		virtio_block_wait(
			device,
			header_desc
		);

	ml_dma_rmb();

	uint8_t request_status = *device->status;

	(void)virtqueue_free_descriptor(
		&device->requestq,
		status_desc
	);

	(void)virtqueue_free_descriptor(
		&device->requestq,
		header_desc
	);

	device->request_count++;
	device->flush_request_count++;

	virtio_block_unlock(device);

	return
		completed &&
		request_status == VIRTIO_BLK_S_OK;
}

static bool virtio_block_flush(
	block_device_t block
)
{
	virtio_block_device_t *device = block->driver_data;

	if (
		device == 0 ||
		!device->attached
	) {
		return false;
	}

	return virtio_block_flush_request(
		device
	);
}

static const block_device_ops_t
	g_virtio_block_ops = {
		.read = virtio_block_read,
		.write = virtio_block_write,
		.flush = virtio_block_flush
	};

/*
 * virtio_block_cleanup:
 *
 * Reset the device before releasing queue or DMA memory. Once reset has
 * completed, the transport no longer owns any published descriptor storage.
 */
static void virtio_block_cleanup(
	virtio_block_device_t *device
)
{
	if (virtio_device_live(&device->transport)) {
		(void)virtio_device_reset(
			&device->transport
		);
	}

	if (device->requestq.initialized) {
		(void)virtqueue_destroy(
			&device->requestq
		);
	}

	if (
		device->bounce_physical !=
		0ULL
	) {
		(void)pmm_free_page(
			device->bounce_physical
		);
	}

	if (
		device->control_physical !=
		0ULL
	) {
		(void)pmm_free_page(
			device->control_physical
		);
	}

	memset(
		device,
		0,
		sizeof(*device)
	);
}

/*
 * virtio_block_attach:
 *
 * Negotiate one block device, configure requestq and publish the resulting
 * transport-independent block device.
 */
bool virtio_block_attach(
	const virtio_device_t *transport
)
{
	if (
		transport == 0 ||
		transport->device_id !=
			VIRTIO_DEVICE_ID_BLOCK
	) {
		return false;
	}

	if (
		g_virtio_block_device_count >=
		VIRTIO_BLOCK_MAX_DEVICES
	) {
		return false;
	}

	virtio_block_device_t *device =
		&g_virtio_block_devices[
			g_virtio_block_device_count
		];

	memset(
		device,
		0,
		sizeof(*device)
	);

	device->transport = *transport;

	uint64_t accepted_features =
		(1ULL << VIRTIO_BLK_F_RO) |
		(1ULL << VIRTIO_BLK_F_BLK_SIZE) |
		(1ULL << VIRTIO_BLK_F_FLUSH);

	if (!virtio_device_begin(
		&device->transport,
		accepted_features
	)) {
		goto fail;
	}

	device->read_only =
		(
			device->transport.driver_features &
			(1ULL << VIRTIO_BLK_F_RO)
		) != 0ULL;

	device->flush_supported =
		(
			device->transport.driver_features &
			(1ULL << VIRTIO_BLK_F_FLUSH)
		) != 0ULL;

	if (!virtio_device_queue_init(
		&device->transport,
		VIRTIO_BLK_REQUESTQ,
		VIRTIO_BLOCK_QUEUE_SIZE,
		&device->requestq
	)) {
		goto fail;
	}

	/*
	 * Synchronous requests consume the used ring by polling.
	 */
	*device->requestq.available_flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

	if (!virtio_device_setup_queue(
		&device->transport,
		VIRTIO_BLK_REQUESTQ,
		&device->requestq
	)) {
		goto fail;
	}

	uint8_t *control;

	if (!virtio_block_allocate_page(
		&device->control_physical,
		&control
	)) {
		goto fail;
	}

	device->header =
		(virtio_block_request_header_t *)
			control;

	device->status =
		(volatile uint8_t *)(
			control +
			sizeof(*device->header)
		);

	if (!virtio_block_allocate_page(
		&device->bounce_physical,
		&device->bounce
	)) {
		goto fail;
	}

	uint64_t capacity =
		virtio_device_config_read64(
			&device->transport,
			VIRTIO_BLK_CONFIG_CAPACITY
		);

	if (capacity == 0ULL) {
		goto fail;
	}

	uint32_t logical_block_size = BLOCK_SECTOR_SIZE;

	if (
		(
			device->transport.driver_features &
			(1ULL << VIRTIO_BLK_F_BLK_SIZE)
		) != 0ULL
	) {
		uint32_t configured =
			virtio_device_config_read32(
				&device->transport,
				VIRTIO_BLK_CONFIG_BLK_SIZE
			);

		if (
			configured >=
				BLOCK_SECTOR_SIZE &&
			(
				configured %
				BLOCK_SECTOR_SIZE
			) == 0U
		) {
			logical_block_size = configured;
		}
	}

	device->block = &device->block_storage;

	device->block_storage.sector_count = capacity;

	device->block_storage.sector_size = BLOCK_SECTOR_SIZE;

	device->block_storage.logical_block_size = logical_block_size;

	device->block_storage.read_only = device->read_only;
	device->block_storage.flush_supported = device->flush_supported;

	device->block_storage.ops = &g_virtio_block_ops;

	device->block_storage.driver_data = device;

	if (!virtio_device_finish(
		&device->transport
	)) {
		goto fail;
	}

	device->attached = true;

	if (!block_device_register(
		device->block
	)) {
		goto fail_after_finish;
	}

	g_virtio_block_device_count++;

	kprintf(
		"VirtIOBlockFamily: matched virtio-blk at 0x%llx, %llu MiB, %s\n",
		(unsigned long long)device->transport.region.base,
		(unsigned long long)((capacity * (uint64_t)BLOCK_SECTOR_SIZE) / (1024ULL * 1024ULL)),
		device->read_only ? "read-only" : "read-write"
	);

	(void)ioreg_add(virtio_block_ioreg_family(), "disk0", "VirtIOBlockDevice");

	return true;

fail_after_finish:
	device->attached = false;

	virtio_block_cleanup(device);

	return false;

fail:
	virtio_block_cleanup(device);

	return false;
}

uint32_t virtio_block_device_count(void)
{
	return g_virtio_block_device_count;
}

const virtio_block_device_t *virtio_block_device(
	uint32_t index
)
{
	if (
		index >=
		g_virtio_block_device_count
	) {
		return 0;
	}

	return &g_virtio_block_devices[
		index
	];
}

/*
 * virtio_block_dump:
 *
 * Print transport and request statistics for attached block devices.
 */
void virtio_block_dump(void)
{
	for (
		uint32_t index = 0U;
		index <
			g_virtio_block_device_count;
		index++
	) {
		const virtio_block_device_t *device =
			&g_virtio_block_devices[
				index
			];

		kputs(
			"VirtIOBlockFamily: device "
		);

		kputu64(
			index + 1U
		);

		kputs(
			", sectors "
		);

		kputu64(
			device->block->sector_count
		);

		kputs(
			", requests "
		);

		kputu64(
			device->request_count
		);

		kputs(
			", read-only "
		);

		kputln(
			device->read_only
				? "yes"
				: "no"
		);
	}
}
