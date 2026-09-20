#include <drivers/virtio/virtio_sound.h>

#include <drivers/virtio/virtio_snd.h>
#include <drivers/virtio/virtio_sound_core.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Every line the driver logs starts with the name of the function that prints it. */
#define VIRTIO_SND_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

typedef struct {
	virtio_device_t transport;
	ioreg_id_t ioreg_family;
	ioreg_id_t ioreg_node;

	/* The device configuration space (section 5.14.4). */
	uint32_t jacks;
	uint32_t streams;
	uint32_t chmaps;

	bool attached;
} virtio_snd_device_t;

static virtio_snd_device_t g_virtio_snd_devices[VIRTIO_SND_MAX_DEVICES];
static uint32_t g_virtio_snd_device_count;

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

void virtio_snd_dump(void)
{
	for (uint32_t index = 0U; index < g_virtio_snd_device_count; index++) {
		const virtio_snd_device_t *device = &g_virtio_snd_devices[index];

		VIRTIO_SND_LOG("device %u, %s, %s\n", index + 1U, device->transport.ops->name, device->attached ? "attached" : "detached");
		VIRTIO_SND_LOG("device %u: %u jack(s), %u stream(s), %u channel map(s)\n", index + 1U, device->jacks, device->streams, device->chmaps);
	}
}
