#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <drivers/virtio/virtio_input.h>

#include <mach/machine/barrier.h>
#include <mach/machine/machine_routines.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <kern/irq/irq.h>
#include <platform/uart.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTIO_INPUT_CFG_ID_NAME 0x01U
#define VIRTIO_INPUT_CFG_EV_BITS 0x11U

#define VIRTIO_INPUT_CONFIG_SELECT 0U
#define VIRTIO_INPUT_CONFIG_SUBSEL 1U
#define VIRTIO_INPUT_CONFIG_SIZE 2U
#define VIRTIO_INPUT_CONFIG_DATA 8U

#define VIRTIO_INPUT_EVENTQ 0U
#define VIRTIO_INPUT_STATUSQ 1U

#define EV_SYN 0U
#define EV_KEY 1U
#define EV_REL 2U

#define KEY_A 30U
#define KEY_ENTER 28U
#define REL_X 0U
#define REL_Y 1U
#define BTN_LEFT 0x110U

_Static_assert(
	sizeof(virtio_input_event_t) == 8U,
	"VirtIO Input event must match the eight-byte wire format"
);

static virtio_input_device_t g_virtio_input_devices[VIRTIO_INPUT_MAX_DEVICES];
static uint32_t g_virtio_input_device_count;
static uint32_t g_next_input_device_id = 1U;
static ioreg_id_t g_virtio_input_ioreg_family;

static ioreg_id_t virtio_input_ioreg_family(void)
{
	if (g_virtio_input_ioreg_family == 0U) {
		g_virtio_input_ioreg_family = ioreg_add(ioreg_family_hid(), "VirtIOInputFamily", "VirtIOInputFamily");
	}
	return g_virtio_input_ioreg_family;
}

/*
 * virtio_input_bitmap_test:
 *
 * Test one capability bit returned through the VirtIO Input configuration
 * bitmap.
 */
static bool virtio_input_bitmap_test(
	const uint8_t *bitmap,
	uint32_t size,
	uint32_t bit
)
{
	uint32_t byte = bit / 8U;
	if (bitmap == 0 || byte >= size) return false;
	return (bitmap[byte] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

/*
 * virtio_input_read_config:
 *
 * Select one VirtIO Input configuration item and copy its bounded payload
 * into caller storage.
 */
static uint32_t virtio_input_read_config(
	virtio_device_t *transport,
	uint8_t select,
	uint8_t subsel,
	uint8_t *buffer,
	uint32_t capacity
)
{
	virtio_device_config_write8(transport, VIRTIO_INPUT_CONFIG_SELECT, select);
	virtio_device_config_write8(transport, VIRTIO_INPUT_CONFIG_SUBSEL, subsel);
	ml_dma_mb();

	uint32_t size = virtio_device_config_read8(transport, VIRTIO_INPUT_CONFIG_SIZE);
	if (size > capacity) size = capacity;

	for (uint32_t index = 0U; index < size; index++) {
		buffer[index] = virtio_device_config_read8(
			transport,
			VIRTIO_INPUT_CONFIG_DATA + index
		);
	}

	return size;
}

/*
 * virtio_input_classify:
 *
 * Classify the device from advertised event capabilities without relying
 * on its transport slot or name.
 */
static input_device_class_t virtio_input_classify(
	virtio_device_t *transport
)
{
	uint8_t bitmap[128];
	memset(bitmap, 0, sizeof(bitmap));

	uint32_t rel_size = virtio_input_read_config(
		transport,
		VIRTIO_INPUT_CFG_EV_BITS,
		EV_REL,
		bitmap,
		sizeof(bitmap)
	);

	bool relative_pointer =
		virtio_input_bitmap_test(bitmap, rel_size, REL_X) &&
		virtio_input_bitmap_test(bitmap, rel_size, REL_Y);

	memset(bitmap, 0, sizeof(bitmap));
	uint32_t key_size = virtio_input_read_config(
		transport,
		VIRTIO_INPUT_CFG_EV_BITS,
		EV_KEY,
		bitmap,
		sizeof(bitmap)
	);

	if (
		relative_pointer &&
		virtio_input_bitmap_test(bitmap, key_size, BTN_LEFT)
	) {
		return INPUT_DEVICE_MOUSE;
	}

	if (
		virtio_input_bitmap_test(bitmap, key_size, KEY_A) &&
		virtio_input_bitmap_test(bitmap, key_size, KEY_ENTER)
	) {
		return INPUT_DEVICE_KEYBOARD;
	}

	return INPUT_DEVICE_UNKNOWN;
}

/*
 * virtio_input_read_name:
 *
 * Read the optional device name into fixed driver-owned storage.
 */
static void virtio_input_read_name(virtio_input_device_t *device)
{
	memset(device->name, 0, sizeof(device->name));
	uint32_t size = virtio_input_read_config(
		&device->transport,
		VIRTIO_INPUT_CFG_ID_NAME,
		0U,
		(uint8_t *)device->name,
		VIRTIO_INPUT_NAME_MAX
	);

	device->name[size] = '\0';
}

/*
 * virtio_input_allocate_buffers:
 *
 * Allocate the physically backed event-buffer array used for device
 * writes.
 */
static bool virtio_input_allocate_buffers(virtio_input_device_t *device)
{
	if (!pmm_allocate_page(&device->event_buffer_physical)) return false;

	uint64_t virtual_address;
	if (!vmm_physical_to_higher_half(
		device->event_buffer_physical,
		&virtual_address
	)) {
		(void)pmm_free_page(device->event_buffer_physical);
		device->event_buffer_physical = 0ULL;
		return false;
	}

	device->event_buffers = (volatile virtio_input_event_t *)virtual_address;
	memset((void *)device->event_buffers, 0, PMM_PAGE_SIZE);
	return true;
}

/*
 * virtio_input_populate_eventq:
 *
 * Populate eventq with one device-writable descriptor for every persistent
 * event buffer.
 */
static bool virtio_input_populate_eventq(virtio_input_device_t *device)
{
	for (uint16_t buffer = 0U; buffer < VIRTIO_INPUT_EVENT_QUEUE_SIZE; buffer++) {
		uint16_t descriptor;
		if (!virtqueue_alloc_descriptor(&device->eventq, &descriptor)) return false;

		device->descriptor_to_buffer[descriptor] = buffer;
		virtq_desc_t *desc = &device->eventq.descriptors[descriptor];
		desc->address = device->event_buffer_physical +
			(uint64_t)buffer * sizeof(virtio_input_event_t);
		desc->length = sizeof(virtio_input_event_t);
		desc->flags = VIRTQ_DESC_F_WRITE;
		desc->next = 0U;

		if (!virtqueue_submit(&device->eventq, descriptor)) return false;
	}

	return true;
}

/*
 * virtio_input_route_event:
 *
 * Normalize one completed VirtIO Input event, publish it to the raw queue,
 * and update class state.
 */
static void virtio_input_route_event(
	virtio_input_device_t *device,
	const virtio_input_event_t *raw
)
{
	input_event_t event = {
		.device_id = device->input_device_id,
		.device_class = device->device_class,
		.type = raw->type,
		.code = raw->code,
		.value = (int32_t)raw->value
	};

	(void)input_publish(&event);

	if (device->device_class == INPUT_DEVICE_KEYBOARD) {
		keyboard_handle_event(&event);
	} else if (device->device_class == INPUT_DEVICE_MOUSE) {
		mouse_handle_event(&event);
	}

	device->event_count++;
}

/*
 * virtio_input_process_eventq:
 *
 * Drain completed event descriptors and recycle each buffer immediately
 * back to eventq.
 */
static void virtio_input_process_eventq(virtio_input_device_t *device)
{
	bool requeued = false;

	for (;;) {
		uint32_t id;
		uint32_t length;

		if (!virtqueue_pop_used(&device->eventq, &id, &length)) break;

		if (id >= device->eventq.size) continue;

		uint16_t buffer = device->descriptor_to_buffer[id];

		if (
			buffer < VIRTIO_INPUT_EVENT_QUEUE_SIZE &&
			length >= sizeof(virtio_input_event_t)
		) {
			ml_dma_rmb();
			virtio_input_event_t raw = device->event_buffers[buffer];
			virtio_input_route_event(device, &raw);
		}

		if (virtqueue_submit(&device->eventq, (uint16_t)id)) requeued = true;
	}

	if (requeued) virtio_device_notify(&device->transport, VIRTIO_INPUT_EVENTQ);
}

/*
 * virtio_input_irq:
 *
 * Acknowledge VirtIO transport interrupt status and process completed
 * input events in IRQ context.
 */
static void virtio_input_irq(uint32_t intid, void *context)
{
	virtio_input_device_t *device = context;
	if (device == 0 || !device->attached || device->transport.intid != intid) return;

	uint32_t status = virtio_device_interrupt_status(&device->transport);
	if (status == 0U) return;

	virtio_device_interrupt_ack(&device->transport, status);
	device->irq_count++;

	if ((status & VIRTIO_INTERRUPT_USED_BUFFER) != 0U) {
		virtio_input_process_eventq(device);
	}

	/* Configuration-change interrupts require no action for fixed QEMU HID. */
	(void)(status & VIRTIO_INTERRUPT_CONFIG);
}

/*
 * virtio_input_cleanup:
 *
 * Release partially initialized driver resources before the device becomes
 * externally visible.
 */
static void virtio_input_cleanup(virtio_input_device_t *device)
{
	if (device->eventq.initialized) (void)virtqueue_destroy(&device->eventq);
	if (device->statusq.initialized) (void)virtqueue_destroy(&device->statusq);

	if (device->event_buffer_physical != 0ULL) {
		(void)pmm_free_page(device->event_buffer_physical);
	}

	memset(device, 0, sizeof(*device));
}

/*
 * virtio_input_attach:
 *
 * Negotiate and attach one VirtIO Input device, initialize its queues,
 * classify it, and register its interrupt source.
 */
bool virtio_input_attach(const virtio_device_t *transport)
{
	if (
		transport == 0 ||
		transport->device_id != VIRTIO_DEVICE_ID_INPUT ||
		g_virtio_input_device_count >= VIRTIO_INPUT_MAX_DEVICES
	) {
		return false;
	}

	virtio_input_device_t *device = &g_virtio_input_devices[g_virtio_input_device_count];
	memset(device, 0, sizeof(*device));
	device->transport = *transport;

	if (!virtio_device_begin(&device->transport, 0ULL)) goto fail;

	virtio_input_read_name(device);
	device->device_class = virtio_input_classify(&device->transport);
	if (device->device_class == INPUT_DEVICE_UNKNOWN) goto fail;

	device->input_device_id = g_next_input_device_id++;

	if (!virtio_device_queue_init(&device->transport, VIRTIO_INPUT_EVENTQ, VIRTIO_INPUT_EVENT_QUEUE_SIZE, &device->eventq)) goto fail;
	if (!virtio_device_queue_init(&device->transport, VIRTIO_INPUT_STATUSQ, VIRTIO_INPUT_STATUS_QUEUE_SIZE, &device->statusq)) goto fail;
	if (!virtio_input_allocate_buffers(device)) goto fail;
	if (!virtio_input_populate_eventq(device)) goto fail;

	if (!virtio_device_setup_queue(
		&device->transport,
		VIRTIO_INPUT_EVENTQ,
		&device->eventq
	)) {
		goto fail;
	}

	if (!virtio_device_setup_queue(
		&device->transport,
		VIRTIO_INPUT_STATUSQ,
		&device->statusq
	)) {
		goto fail;
	}

	if (!virtio_device_finish(&device->transport)) goto fail;

	if (device->device_class == INPUT_DEVICE_KEYBOARD) {
		if (!keyboard_attach(device->input_device_id)) goto fail;
	} else if (device->device_class == INPUT_DEVICE_MOUSE) {
		if (!mouse_attach(device->input_device_id)) goto fail;
	}

	if (!virtio_device_irq_attach(&device->transport, virtio_input_irq, device)) goto fail;

	device->attached = true;
	g_virtio_input_device_count++;

	kprintf(
		"VirtIOInputFamily: matched %s \"%s\" at 0x%llx\n",
		device->device_class == INPUT_DEVICE_KEYBOARD ? "keyboard" : device->device_class == INPUT_DEVICE_MOUSE ? "mouse" : "input device",
		device->name[0] != '\0' ? device->name : "unnamed",
		(unsigned long long)device->transport.region.base
	);

	(void)ioreg_add(
		virtio_input_ioreg_family(),
		device->device_class == INPUT_DEVICE_KEYBOARD ? "Keyboard" : device->device_class == INPUT_DEVICE_MOUSE ? "Mouse" : "InputDevice",
		"VirtIOInputDevice"
	);

	virtio_device_notify(&device->transport, VIRTIO_INPUT_EVENTQ);
	return true;

fail:
	virtio_device_fail(&device->transport);
	virtio_input_cleanup(device);
	return false;
}

/*
 * virtio_input_service:
 *
 * Drain input completions from thread/host-loop context as a fallback for
 * platforms where the VirtIO MMIO SPI has not started delivering IRQs yet.
 * The same queue is also serviced by virtio_input_irq(), so mask local IRQs
 * while polling to keep last_used_index and descriptor recycling serialized.
 */
void virtio_input_service(void)
{
	uint64_t irq_state = ml_irq_save();

	for (uint32_t index = 0U; index < g_virtio_input_device_count; index++) {
		virtio_input_device_t *device = &g_virtio_input_devices[index];
		if (!device->attached) continue;
		virtio_input_process_eventq(device);
	}

	ml_irq_restore(irq_state);
}

uint32_t virtio_input_device_count(void)
{
	return g_virtio_input_device_count;
}

const virtio_input_device_t *virtio_input_device(uint32_t index)
{
	if (index >= g_virtio_input_device_count) return 0;
	return &g_virtio_input_devices[index];
}

/*
 * virtio_input_dump:
 *
 * Print the set of attached input devices and their bring-up counters.
 */
void virtio_input_dump(void)
{
	for (uint32_t index = 0U; index < g_virtio_input_device_count; index++) {
		const virtio_input_device_t *device = &g_virtio_input_devices[index];

		kputs("VirtIOInputFamily: device ");
		kputu64(device->input_device_id);
		kputs(", class ");

		if (device->device_class == INPUT_DEVICE_KEYBOARD) {
			kputs("keyboard");
		} else if (device->device_class == INPUT_DEVICE_MOUSE) {
			kputs("mouse");
		} else {
			kputs("unknown");
		}

		kputs(", name ");
		kputs(device->name[0] != '\0' ? device->name : "unnamed");
		kputs(", INTID ");
		kputu64(device->transport.intid);
		kputc('\n');
	}
}
