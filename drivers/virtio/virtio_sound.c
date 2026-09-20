#include <drivers/virtio/virtio_sound.h>

#include <drivers/virtio/virtio_sound_internal.h>
#include <kern/machine/barrier.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/timer.h>
#include <vm/pmm.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1U

/* Spins a control request may take when the clock is not running yet (i386 before calibration). */
#define VIRTIO_SND_CONTROL_SPINS 200000000ULL

static virtio_snd_device_t g_virtio_snd_devices[VIRTIO_SND_MAX_DEVICES];
static uint32_t g_virtio_snd_device_count;

/* ---- DMA memory -------------------------------------------------------- */

bool virtio_snd_allocate_page(virtio_snd_page_t *page)
{
	uint64_t physical;
	uint64_t higher_half;

	if (!pmm_allocate_page(&physical)) return false;

	if (!vmm_physical_to_higher_half(physical, &higher_half)) {
		(void)pmm_free_page(physical);
		return false;
	}

	page->physical = physical;
	page->virtual_address = (uint8_t *)higher_half;
	memset(page->virtual_address, 0, PMM_PAGE_SIZE);
	return true;
}

void virtio_snd_free_page(virtio_snd_page_t *page)
{
	if (page->physical != 0ULL) (void)pmm_free_page(page->physical);

	page->physical = 0ULL;
	page->virtual_address = 0;
}

/* ---- where the device is ---------------------------------------------- */

/*
 * virtio_snd_describe_transport:
 *
 * Say where the device was found: the MMIO frame and GIC INTID on arm64, the
 * PCI function and PIC line on x86.
 */
static void virtio_snd_describe_transport(const virtio_device_t *transport)
{
	if (transport->pci.common != 0 || transport->pci.io_base != 0U) {
		VIRTIO_SND_LOG("transport %s, PCI %x:%x.%x\n", transport->ops->name, (unsigned int)transport->pci.bus, (unsigned int)transport->pci.slot, (unsigned int)transport->pci.function);
		VIRTIO_SND_LOG("interrupt line %u (legacy INTx through the PIC)\n", (unsigned int)transport->pci.interrupt_line);
		return;
	}

	VIRTIO_SND_LOG("transport %s, MMIO frame 0x%llx (%llu bytes)\n", transport->ops->name, (unsigned long long)transport->region.base, (unsigned long long)transport->region.size);
	VIRTIO_SND_LOG("interrupt INTID %u (GIC SPI)\n", transport->intid);
}

/* ---- feature negotiation ---------------------------------------------- */

/*
 * virtio_snd_log_features:
 *
 * One line for every feature bit the device offered: its name, its number
 * and whether the driver takes it. What the driver took is what the
 * transport wrote back to the device, so this is the negotiation as it
 * really ended, not as the driver meant it.
 */
static void virtio_snd_log_features(const virtio_snd_device_t *device)
{
	uint64_t offered = device->transport.device_features;
	uint64_t accepted = device->transport.driver_features;
	uint32_t offered_count = 0U;
	uint32_t accepted_count = 0U;

	for (uint32_t bit = 0U; bit < 64U; bit++) {
		uint64_t mask = 1ULL << bit;

		if ((offered & mask) == 0ULL) continue;

		offered_count++;
		if ((accepted & mask) != 0ULL) accepted_count++;

		VIRTIO_SND_LOG("device offers feature bit %u %s: %s\n", bit, virtio_snd_feature_name(bit), (accepted & mask) != 0ULL ? "accepted" : "declined");
	}

	VIRTIO_SND_LOG("%u feature bit(s) offered\n", offered_count);
	VIRTIO_SND_LOG("%u feature bit(s) accepted\n", accepted_count);
	kverbosef("virtio_snd_log_features: features offered 0x%llx\n", (unsigned long long)offered);
	kverbosef("virtio_snd_log_features: features accepted 0x%llx\n", (unsigned long long)accepted);
}

/*
 * virtio_snd_negotiate_features:
 *
 * Reset the device and run the feature handshake. The transport reads what
 * the device offers, accepts VERSION_1 plus whatever the class driver names
 * (nothing, for sound), and confirms with FEATURES_OK. A device that does not
 * offer VERSION_1 is a legacy device this driver cannot speak to: it is
 * reported and left in the FAILED state, and false comes back so the boot
 * goes on without sound.
 */
static bool virtio_snd_negotiate_features(virtio_snd_device_t *device)
{
	if (virtio_device_begin(&device->transport, 0ULL)) {
		uint64_t wanted;

		(void)virtio_snd_negotiate(device->transport.device_features, &wanted);
		virtio_snd_log_features(device);

		if (device->transport.driver_features != wanted) {
			VIRTIO_SND_LOG("the transport accepted 0x%llx, the driver decided on 0x%llx\n", (unsigned long long)device->transport.driver_features, (unsigned long long)wanted);
		}

		return true;
	}

	/* begin() has already stopped the handshake; say why. */
	uint64_t accepted;

	virtio_snd_log_features(device);

	if (!virtio_snd_negotiate(device->transport.device_features, &accepted)) {
		VIRTIO_SND_LOG("device does not offer F_VERSION_1 (bit %u), it is a legacy device: refusing it\n", VIRTIO_F_VERSION_1);
	} else {
		VIRTIO_SND_LOG("the device did not confirm the accepted features (FEATURES_OK was not set): refusing it\n");
	}

	return false;
}

/*
 * virtio_snd_read_config:
 *
 * Read the three counters of the device configuration space, one line each.
 */
static void virtio_snd_read_config(virtio_snd_device_t *device)
{
	device->jacks = virtio_device_config_read32(&device->transport, VIRTIO_SND_CONFIG_JACKS);
	device->streams = virtio_device_config_read32(&device->transport, VIRTIO_SND_CONFIG_STREAMS);
	device->chmaps = virtio_device_config_read32(&device->transport, VIRTIO_SND_CONFIG_CHMAPS);

	VIRTIO_SND_LOG("config: %u jack(s)\n", device->jacks);
	VIRTIO_SND_LOG("config: %u PCM stream(s)\n", device->streams);
	VIRTIO_SND_LOG("config: %u channel map(s)\n", device->chmaps);
}

/* ---- queues ------------------------------------------------------------ */

/*
 * virtio_snd_setup_queues:
 *
 * Create and publish the control, event and tx queues. The rx queue is only
 * for capture, which the driver does not do, so it is left unconfigured.
 * Control completions are polled, so that queue never interrupts.
 */
static bool virtio_snd_setup_queues(virtio_snd_device_t *device)
{
	struct {
		uint16_t index;
		uint16_t size;
		virtqueue_t *queue;
		const char *name;
	} queues[] = {
		{ VIRTIO_SND_VQ_CONTROL, VIRTIO_SND_CONTROL_QUEUE_SIZE, &device->controlq, "controlq" },
		{ VIRTIO_SND_VQ_EVENT, VIRTIO_SND_EVENT_QUEUE_SIZE, &device->eventq, "eventq" },
		{ VIRTIO_SND_VQ_TX, VIRTIO_SND_TX_QUEUE_SIZE, &device->txq, "txq" }
	};
	const uint32_t count = sizeof(queues) / sizeof(queues[0]);

	for (uint32_t index = 0U; index < count; index++) {
		if (!virtio_device_queue_init(&device->transport, queues[index].index, queues[index].size, queues[index].queue)) {
			VIRTIO_SND_LOG("virtqueue %u (%s) could not be created\n", queues[index].index, queues[index].name);
			return false;
		}

		VIRTIO_SND_LOG("virtqueue %u (%s): %u descriptors\n", queues[index].index, queues[index].name, (unsigned int)queues[index].queue->size);
	}

	VIRTIO_SND_LOG("virtqueue %u (rxq): not used, the driver does not capture\n", VIRTIO_SND_VQ_RX);

	*device->controlq.available_flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

	for (uint32_t index = 0U; index < count; index++) {
		if (!virtio_device_setup_queue(&device->transport, queues[index].index, queues[index].queue)) {
			VIRTIO_SND_LOG("virtqueue %u (%s) was refused by the device\n", queues[index].index, queues[index].name);
			return false;
		}
	}

	return true;
}

/*
 * virtio_snd_cleanup:
 *
 * Give back what a device that failed to attach holds. The reset comes first:
 * once it is done the device no longer reads the rings.
 */
static void virtio_snd_cleanup(virtio_snd_device_t *device)
{
	if (virtio_device_live(&device->transport)) (void)virtio_device_reset(&device->transport);

	if (device->controlq.initialized) (void)virtqueue_destroy(&device->controlq);
	if (device->eventq.initialized) (void)virtqueue_destroy(&device->eventq);
	if (device->txq.initialized) (void)virtqueue_destroy(&device->txq);

	virtio_snd_free_page(&device->control_page);
	virtio_snd_free_page(&device->event_page);
	memset(device, 0, sizeof(*device));
}

/* ---- the control queue ------------------------------------------------- */

virtio_snd_error_t virtio_snd_control(virtio_snd_device_t *device, uint32_t request_bytes, uint32_t response_capacity, uint32_t *response_bytes)
{
	if (response_bytes != 0) *response_bytes = 0U;

	if (device->control_dead) return VIRTIO_SND_E_DEAD;
	if (request_bytes == 0U || request_bytes > VIRTIO_SND_CONTROL_RESPONSE_OFFSET || response_capacity < sizeof(virtio_snd_hdr_t) || response_capacity > VIRTIO_SND_CONTROL_RESPONSE_MAX) return VIRTIO_SND_E_INVALID;

	uint8_t *request = device->control_page.virtual_address;
	uint8_t *response = request + VIRTIO_SND_CONTROL_RESPONSE_OFFSET;
	uint16_t request_desc;
	uint16_t response_desc;

	if (!virtqueue_alloc_descriptor(&device->controlq, &request_desc)) return VIRTIO_SND_E_NO_MEMORY;

	if (!virtqueue_alloc_descriptor(&device->controlq, &response_desc)) {
		(void)virtqueue_free_descriptor(&device->controlq, request_desc);
		return VIRTIO_SND_E_NO_MEMORY;
	}

	memset(response, 0, response_capacity);

	virtq_desc_t *out = &device->controlq.descriptors[request_desc];

	out->address = device->control_page.physical;
	out->length = request_bytes;
	out->flags = VIRTQ_DESC_F_NEXT;
	out->next = response_desc;

	virtq_desc_t *in = &device->controlq.descriptors[response_desc];

	in->address = device->control_page.physical + VIRTIO_SND_CONTROL_RESPONSE_OFFSET;
	in->length = response_capacity;
	in->flags = VIRTQ_DESC_F_WRITE;
	in->next = 0U;

	ml_dma_wmb();

	if (!virtqueue_submit(&device->controlq, request_desc)) {
		(void)virtqueue_free_descriptor(&device->controlq, response_desc);
		(void)virtqueue_free_descriptor(&device->controlq, request_desc);
		return VIRTIO_SND_E_IO;
	}

	device->control_requests++;
	virtio_device_notify(&device->transport, VIRTIO_SND_VQ_CONTROL);

	uint64_t deadline = timer_get_microseconds() + VIRTIO_SND_CONTROL_TIMEOUT_US;
	uint64_t spins = 0ULL;
	uint32_t id;
	uint32_t length;

	while (!virtqueue_pop_used(&device->controlq, &id, &length)) {
		spins++;

		if (timer_get_microseconds() > deadline || spins > VIRTIO_SND_CONTROL_SPINS) {
			/* The device may still write the response: the descriptors and the page stay theirs. */
			device->control_dead = true;
			device->control_failures++;
			VIRTIO_SND_LOG("request 0x%x (%s) was not answered, giving up on the device\n", ((const virtio_snd_hdr_t *)(const void *)request)->code, virtio_snd_request_name(((const virtio_snd_hdr_t *)(const void *)request)->code));
			return VIRTIO_SND_E_TIMEOUT;
		}

		ml_cpu_relax();
	}

	ml_dma_rmb();

	uint32_t request_code = ((const virtio_snd_hdr_t *)(const void *)request)->code;
	uint32_t status = ((const virtio_snd_hdr_t *)(const void *)response)->code;

	(void)virtqueue_free_descriptor(&device->controlq, response_desc);
	(void)virtqueue_free_descriptor(&device->controlq, request_desc);

	if (id != request_desc) {
		device->control_dead = true;
		device->control_failures++;
		VIRTIO_SND_LOG("request 0x%x (%s) was answered with descriptor %u, expected %u: giving up on the device\n", request_code, virtio_snd_request_name(request_code), id, (unsigned int)request_desc);
		return VIRTIO_SND_E_IO;
	}

	if (response_bytes != 0) *response_bytes = length;

	virtio_snd_error_t error = virtio_snd_error_from_status(status);

	if (error != VIRTIO_SND_E_NONE) {
		device->control_failures++;
		kverbosef("virtio_snd_control: request 0x%x (%s) answered %s (0x%x)\n", request_code, virtio_snd_request_name(request_code), virtio_snd_status_name(status), status);
	} else {
		kverbosef("virtio_snd_control: request 0x%x (%s) answered OK, %u byte(s)\n", request_code, virtio_snd_request_name(request_code), length);
	}

	return error;
}

virtio_snd_error_t virtio_snd_pcm_request(virtio_snd_device_t *device, uint32_t request, uint32_t stream_id)
{
	virtio_snd_pcm_hdr_t *message = (virtio_snd_pcm_hdr_t *)(void *)device->control_page.virtual_address;

	message->hdr.code = request;
	message->stream_id = stream_id;

	uint32_t response_bytes;

	return virtio_snd_control(device, sizeof(*message), sizeof(virtio_snd_hdr_t), &response_bytes);
}

virtio_snd_error_t virtio_snd_set_params_request(virtio_snd_device_t *device, uint32_t stream_id, uint32_t buffer_bytes, uint32_t period_bytes, const virtio_snd_pcm_format_t *format)
{
	virtio_snd_pcm_set_params_t *message = (virtio_snd_pcm_set_params_t *)(void *)device->control_page.virtual_address;

	memset(message, 0, sizeof(*message));
	message->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
	message->hdr.stream_id = stream_id;
	message->buffer_bytes = buffer_bytes;
	message->period_bytes = period_bytes;
	message->features = 0U;
	message->channels = format->channels;
	message->format = format->format;
	message->rate = format->rate;
	message->padding = 0U;

	uint32_t response_bytes;

	return virtio_snd_control(device, sizeof(*message), sizeof(virtio_snd_hdr_t), &response_bytes);
}

/*
 * virtio_snd_query_info:
 *
 * The item information request (section 5.14.6.1): `count` items of `size`
 * bytes starting at `start_id`, of jacks, streams or channel maps according
 * to `request`, copied to `items`. The response must carry them all.
 */
static virtio_snd_error_t virtio_snd_query_info(virtio_snd_device_t *device, uint32_t request, uint32_t start_id, uint32_t count, uint32_t size, void *items)
{
	if (count == 0U || size == 0U || size > VIRTIO_SND_CONTROL_RESPONSE_MAX || count > (VIRTIO_SND_CONTROL_RESPONSE_MAX - sizeof(virtio_snd_hdr_t)) / size) return VIRTIO_SND_E_INVALID;

	virtio_snd_query_info_t *message = (virtio_snd_query_info_t *)(void *)device->control_page.virtual_address;

	message->hdr.code = request;
	message->start_id = start_id;
	message->count = count;
	message->size = size;

	uint32_t wanted = (uint32_t)sizeof(virtio_snd_hdr_t) + count * size;
	uint32_t response_bytes;
	virtio_snd_error_t error = virtio_snd_control(device, sizeof(*message), wanted, &response_bytes);

	if (error != VIRTIO_SND_E_NONE) return error;

	if (response_bytes < wanted) {
		VIRTIO_SND_LOG("request %s: the device answered %u byte(s), %u were needed\n", virtio_snd_request_name(request), response_bytes, wanted);
		return VIRTIO_SND_E_IO;
	}

	ml_dma_rmb();
	memcpy(items, device->control_page.virtual_address + VIRTIO_SND_CONTROL_RESPONSE_OFFSET + sizeof(virtio_snd_hdr_t), (size_t)count * size);
	return VIRTIO_SND_E_NONE;
}

/* ---- what the device can do ------------------------------------------- */

static void virtio_snd_probe_jacks(virtio_snd_device_t *device)
{
	if (device->jacks == 0U) {
		VIRTIO_SND_LOG("no jacks to query\n");
		return;
	}

	uint32_t count = device->jacks < 8U ? device->jacks : 8U;
	virtio_snd_jack_info_t jacks[8];
	virtio_snd_error_t error = virtio_snd_query_info(device, VIRTIO_SND_R_JACK_INFO, 0U, count, sizeof(jacks[0]), jacks);

	if (error != VIRTIO_SND_E_NONE) {
		VIRTIO_SND_LOG("JACK_INFO for %u jack(s): %s\n", count, virtio_snd_error_name(error));
		return;
	}

	for (uint32_t index = 0U; index < count; index++) {
		VIRTIO_SND_LOG("jack %u: %s\n", index, jacks[index].connected != 0U ? "connected" : "not connected");
		VIRTIO_SND_LOG("jack %u: features 0x%x%s\n", index, jacks[index].features, (jacks[index].features & (1U << VIRTIO_SND_JACK_F_REMAP)) != 0U ? " (remappable)" : "");
	}
}

static void virtio_snd_probe_chmaps(virtio_snd_device_t *device)
{
	if (device->chmaps == 0U) {
		VIRTIO_SND_LOG("no channel maps to query\n");
		return;
	}

	uint32_t count = device->chmaps < 8U ? device->chmaps : 8U;
	virtio_snd_chmap_info_t maps[8];
	virtio_snd_error_t error = virtio_snd_query_info(device, VIRTIO_SND_R_CHMAP_INFO, 0U, count, sizeof(maps[0]), maps);

	if (error != VIRTIO_SND_E_NONE) {
		VIRTIO_SND_LOG("CHMAP_INFO for %u channel map(s): %s\n", count, virtio_snd_error_name(error));
		return;
	}

	for (uint32_t index = 0U; index < count; index++) {
		char positions[128];

		(void)virtio_snd_describe_positions(maps[index].positions, maps[index].channels, positions, sizeof(positions));
		VIRTIO_SND_LOG("channel map %u: %s, %u channel(s)\n", index, virtio_snd_direction_name(maps[index].direction), (unsigned int)maps[index].channels);
		VIRTIO_SND_LOG("channel map %u: positions %s\n", index, positions);
	}
}

static void virtio_snd_log_stream(uint32_t stream_id, const virtio_snd_pcm_info_t *info)
{
	char formats[256];
	char rates[192];

	(void)virtio_snd_describe_formats(info->formats, formats, sizeof(formats));
	(void)virtio_snd_describe_rates(info->rates, rates, sizeof(rates));

	VIRTIO_SND_LOG("stream %u: %s\n", stream_id, virtio_snd_direction_name(info->direction));
	VIRTIO_SND_LOG("stream %u: %u to %u channel(s)\n", stream_id, (unsigned int)info->channels_min, (unsigned int)info->channels_max);
	VIRTIO_SND_LOG("stream %u: formats %s\n", stream_id, formats);
	VIRTIO_SND_LOG("stream %u: rates %s Hz\n", stream_id, rates);
	VIRTIO_SND_LOG("stream %u: features 0x%x\n", stream_id, info->features);
	kverbosef("virtio_snd_log_stream: stream %u: HDA function node %u\n", stream_id, info->hdr.hda_fn_nid);
}

/*
 * virtio_snd_probe_streams:
 *
 * PCM_INFO for every stream (up to what the driver keeps), logged in full,
 * then the choice of the stream /dev/audio0 plays on: the first output stream
 * that takes 16-bit stereo at 44.1 kHz, the one format every WAV is converted to.
 */
static void virtio_snd_probe_streams(virtio_snd_device_t *device)
{
	device->playback_stream = VIRTIO_SND_NO_STREAM;

	if (device->streams == 0U) {
		VIRTIO_SND_LOG("the device has no PCM streams\n");
		return;
	}

	uint32_t count = device->streams < VIRTIO_SND_MAX_STREAMS ? device->streams : VIRTIO_SND_MAX_STREAMS;

	if (count < device->streams) VIRTIO_SND_LOG("only the first %u of %u streams are kept\n", count, device->streams);

	virtio_snd_error_t error = virtio_snd_query_info(device, VIRTIO_SND_R_PCM_INFO, 0U, count, sizeof(device->pcm_info[0]), device->pcm_info);

	if (error != VIRTIO_SND_E_NONE) {
		VIRTIO_SND_LOG("PCM_INFO for %u stream(s): %s\n", count, virtio_snd_error_name(error));
		return;
	}

	device->pcm_info_count = count;

	for (uint32_t index = 0U; index < count; index++) virtio_snd_log_stream(index, &device->pcm_info[index]);

	virtio_snd_pcm_format_t wanted = { .channels = 2U, .format = VIRTIO_SND_PCM_FMT_S16, .rate = VIRTIO_SND_PCM_RATE_44100 };

	for (uint32_t index = 0U; index < count && device->playback_stream == VIRTIO_SND_NO_STREAM; index++) {
		if (device->pcm_info[index].direction != VIRTIO_SND_D_OUTPUT) continue;
		if (!virtio_snd_pcm_supports(&device->pcm_info[index], &wanted)) continue;

		device->playback_stream = index;
	}

	if (device->playback_stream == VIRTIO_SND_NO_STREAM) {
		VIRTIO_SND_LOG("no output stream takes 16-bit stereo at 44100 Hz: no playback\n");
		return;
	}

	VIRTIO_SND_LOG("playback stream %u: 16-bit stereo at 44100 Hz is supported\n", device->playback_stream);
}

/* ---- attach ------------------------------------------------------------ */

/*
 * virtio_snd_bring_up:
 *
 * Everything between the feature handshake and DRIVER_OK: the control page,
 * the queues. False leaves the device for virtio_snd_cleanup().
 */
static bool virtio_snd_bring_up(virtio_snd_device_t *device)
{
	if (!virtio_snd_allocate_page(&device->control_page) || !virtio_snd_allocate_page(&device->event_page)) {
		VIRTIO_SND_LOG("no memory for the control and event buffers\n");
		return false;
	}

	if (!virtio_snd_setup_queues(device)) return false;

	if (!virtio_snd_post_events(device)) {
		VIRTIO_SND_LOG("the event buffers could not be posted\n");
		return false;
	}

	VIRTIO_SND_LOG("%u event buffer(s) posted on the eventq\n", VIRTIO_SND_EVENT_QUEUE_SIZE);

	if (!virtio_device_irq_attach(&device->transport, virtio_snd_irq, device)) {
		VIRTIO_SND_LOG("the interrupt could not be attached\n");
		return false;
	}

	if (device->transport.irq_bound) {
		VIRTIO_SND_LOG("interrupt handler attached: completions arrive by interrupt\n");
	} else {
		VIRTIO_SND_LOG("interrupt delivery is off on this transport: completions are polled\n");
	}

	if (!virtio_device_finish(&device->transport)) {
		VIRTIO_SND_LOG("the device did not accept DRIVER_OK\n");
		return false;
	}

	device->ready = true;
	virtio_device_notify(&device->transport, VIRTIO_SND_VQ_EVENT);
	VIRTIO_SND_LOG("device is live (DRIVER_OK)\n");
	return true;
}

bool virtio_snd_attach(const virtio_device_t *transport)
{
	if (transport == 0 || transport->device_id != VIRTIO_DEVICE_ID_SOUND) return false;

	if (g_virtio_snd_device_count >= VIRTIO_SND_MAX_DEVICES) {
		VIRTIO_SND_LOG("a VirtIO Sound device is already attached, ignoring this one\n");
		return false;
	}

	virtio_snd_device_t *device = &g_virtio_snd_devices[g_virtio_snd_device_count];
	memset(device, 0, sizeof(*device));
	device->transport = *transport;

	VIRTIO_SND_LOG("VirtIO Sound device found\n");
	virtio_snd_describe_transport(&device->transport);
	VIRTIO_SND_LOG("vendor ID 0x%x, device ID %u\n", transport->vendor_id, transport->device_id);

	if (!virtio_snd_negotiate_features(device)) {
		memset(device, 0, sizeof(*device));
		return false;
	}

	virtio_snd_read_config(device);

	if (!virtio_snd_bring_up(device)) {
		VIRTIO_SND_LOG("bring-up failed, the device is left unused\n");
		virtio_device_fail(&device->transport);
		virtio_snd_cleanup(device);
		return false;
	}

	virtio_snd_probe_jacks(device);
	virtio_snd_probe_streams(device);
	virtio_snd_probe_chmaps(device);

	device->ioreg_family = ioreg_add(ioreg_family_audio(), "VirtIOSoundFamily", "VirtIOSoundFamily");
	device->ioreg_node = ioreg_add(device->ioreg_family, "Audio0", "VirtIOSoundDevice");
	VIRTIO_SND_LOG("registered Audio0 under DriverKitAudioFamily in the I/O registry\n");

	device->attached = true;
	g_virtio_snd_device_count++;
	return true;
}

void virtio_snd_probe(void)
{
	if (g_virtio_snd_device_count == 0U) {
		VIRTIO_SND_LOG("no VirtIO Sound device\n");
		return;
	}

	VIRTIO_SND_LOG("%u VirtIO Sound device(s) attached\n", g_virtio_snd_device_count);
}

uint32_t virtio_snd_device_count(void)
{
	return g_virtio_snd_device_count;
}

virtio_snd_device_t *virtio_snd_device_at(uint32_t index)
{
	if (index >= g_virtio_snd_device_count) return 0;

	virtio_snd_device_t *device = &g_virtio_snd_devices[index];

	return device->attached && device->playback_stream != VIRTIO_SND_NO_STREAM ? device : 0;
}

/*
 * virtio_snd_dump:
 *
 * The end-of-boot summary: what the device is, what it was asked and what
 * has come of it. The counters that only mean something once audio has
 * played are detail, they are behind -v.
 */
void virtio_snd_dump(void)
{
	for (uint32_t index = 0U; index < g_virtio_snd_device_count; index++) {
		const virtio_snd_device_t *device = &g_virtio_snd_devices[index];
		const virtio_snd_playback_t *playback = &device->playback;

		VIRTIO_SND_LOG("device %u, %s, %s\n", index + 1U, device->transport.ops->name, device->attached ? "attached" : "detached");
		VIRTIO_SND_LOG("device %u: %u jack(s), %u stream(s), %u channel map(s)\n", index + 1U, device->jacks, device->streams, device->chmaps);

		if (device->playback_stream == VIRTIO_SND_NO_STREAM) {
			VIRTIO_SND_LOG("device %u: no playback stream\n", index + 1U);
		} else {
			VIRTIO_SND_LOG("device %u: playback on stream %u, /dev/audio0\n", index + 1U, device->playback_stream);
		}

		VIRTIO_SND_LOG("device %u: interrupts %s\n", index + 1U, device->transport.irq_bound ? "by interrupt" : "polled");
		VIRTIO_SND_LOG("device %u: %llu playback open(s)\n", index + 1U, (unsigned long long)playback->opens);
		VIRTIO_SND_LOG("device %u: %llu period(s) played\n", index + 1U, (unsigned long long)playback->total_periods);
		VIRTIO_SND_LOG("device %u: %llu underrun(s)\n", index + 1U, (unsigned long long)playback->total_xruns);
		kverbosef("virtio_snd_dump: device %u: %llu control request(s), %llu failed\n", index + 1U, (unsigned long long)device->control_requests, (unsigned long long)device->control_failures);
		kverbosef("virtio_snd_dump: device %u: %llu interrupt(s)\n", index + 1U, (unsigned long long)device->irq_count);
		kverbosef("virtio_snd_dump: device %u: %llu event(s), %llu xrun, %llu jack\n", index + 1U, (unsigned long long)device->event_count, (unsigned long long)device->xrun_events, (unsigned long long)device->jack_events);
		if (device->event_count != 0ULL) kverbosef("virtio_snd_dump: device %u: last event %s (0x%x) for %u\n", index + 1U, virtio_snd_event_name(device->last_event), device->last_event, device->last_event_data);
	}
}
