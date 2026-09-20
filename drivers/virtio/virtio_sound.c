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
		VIRTIO_SND_LOG("transport %s, PCI %x:%x.%x, interrupt line %u\n", transport->ops->name, (unsigned int)transport->pci.bus, (unsigned int)transport->pci.slot, (unsigned int)transport->pci.function, (unsigned int)transport->pci.interrupt_line);
		return;
	}

	VIRTIO_SND_LOG("transport %s, MMIO frame 0x%llx (%llu bytes), interrupt INTID %u\n", transport->ops->name, (unsigned long long)transport->region.base, (unsigned long long)transport->region.size, transport->intid);
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
	}
}
