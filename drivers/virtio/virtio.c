#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <drivers/virtio/virtio.h>

#include <drivers/virtio/virtio_block.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio_mmio.h>
#include <drivers/virtio/virtio_sound.h>
#include <drivers/virtio/virtio_net.h>

#include <stdbool.h>
#include <stdint.h>

static uint32_t g_virtio_device_count;
static uint32_t g_virtio_input_count;
static uint32_t g_virtio_block_count;
static uint32_t g_virtio_gpu_count;
static uint32_t g_virtio_sound_count;
static uint32_t g_virtio_network_count;
static bool g_virtio_initialized;

#define VIRTIO_MAX_BUS_SCANNERS 4U

static virtio_bus_scan_t g_virtio_bus_scanners[VIRTIO_MAX_BUS_SCANNERS];
static uint32_t g_virtio_bus_scanner_count;

bool
virtio_bus_register(virtio_bus_scan_t scan)
{
	if (scan == 0 || g_virtio_initialized) return false;

	for (uint32_t index = 0U; index < g_virtio_bus_scanner_count; index++) {
		if (g_virtio_bus_scanners[index] == scan) return true;
	}

	if (g_virtio_bus_scanner_count >= VIRTIO_MAX_BUS_SCANNERS) return false;

	g_virtio_bus_scanners[g_virtio_bus_scanner_count++] = scan;
	return true;
}

/*
 * Match one live device to its class driver, whichever bus it was found on.
 * A GPU that fails to attach is reported and skipped; a block or input device
 * that fails to attach stops the scan.
 */
bool
virtio_bind_device(const virtio_device_t *device, const virtio_probe_policy_t *policy)
{
	if (device == 0 || policy == 0) return false;

	g_virtio_device_count++;
	const char *driver_name = "VirtIOUnknownDriver";
	switch (device->device_id) {
	case VIRTIO_DEVICE_ID_NET: driver_name = "VirtIONetworkDriver"; break;
	case VIRTIO_DEVICE_ID_GPU: driver_name = "VirtIOGPUDriver"; break;
	case VIRTIO_DEVICE_ID_BLOCK: driver_name = "VirtIOBlockDriver"; break;
	case VIRTIO_DEVICE_ID_INPUT: driver_name = "VirtIOInputDriver"; break;
	case VIRTIO_DEVICE_ID_SOUND: driver_name = "VirtIOSoundDriver"; break;
	default: break;
	}
	kverbosef("VirtIOFamily: %s via %s at 0x%llx, DMA %s, address mask 0x%llx\n", driver_name,
			  device->ops != 0 && device->ops->name != 0 ? device->ops->name : "unknown transport",
			  (unsigned long long)device->region.base, device->dma.coherent ? "coherent" : "noncoherent",
			  (unsigned long long)device->dma.address_mask);
	ioreg_set_driver_state(device->registry_id, IOREG_STATE_MATCHED, driver_name);

	if (device->device_id == VIRTIO_DEVICE_ID_NET) {
		if (policy->network) {
			if (virtio_net_attach(device)) {
				g_virtio_network_count++;
				ioreg_set_driver_state(device->registry_id, IOREG_STATE_ACTIVE, driver_name);
			} else {
				kputln("VirtIOFamily: network device failed to attach");
				ioreg_set_driver_state(device->registry_id, IOREG_STATE_FAILED, driver_name);
			}
		} else {
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_DISABLED, driver_name);
		}

		return true;
	}

	if (device->device_id == VIRTIO_DEVICE_ID_GPU) {
		if (!policy->gpu) {
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_DISABLED, driver_name);
			return true;
		}

		if (!virtio_gpu_attach(device)) {
			kprintf("VirtIOFamily: GPU at 0x%llx failed to attach\n", (unsigned long long)device->region.base);
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_FAILED, driver_name);
			return true;
		}

		g_virtio_gpu_count++;
		ioreg_set_driver_state(device->registry_id, IOREG_STATE_ACTIVE, driver_name);
		if (policy->on_gpu_ready != 0) policy->on_gpu_ready();
		return true;
	}

	if (device->device_id == VIRTIO_DEVICE_ID_BLOCK) {
		if (!policy->block) {
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_DISABLED, driver_name);
			return true;
		}

		if (!virtio_block_attach(device)) {
			kprintf("VirtIOFamily: block device at 0x%llx failed to attach\n", (unsigned long long)device->region.base);
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_FAILED, driver_name);
			return false;
		}

		g_virtio_block_count++;
		ioreg_set_driver_state(device->registry_id, IOREG_STATE_ACTIVE, driver_name);
		return true;
	}

	if (device->device_id == VIRTIO_DEVICE_ID_INPUT) {
		if (!policy->input) {
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_DISABLED, driver_name);
			return true;
		}

		if (!virtio_input_attach(device)) {
			kprintf("VirtIOFamily: input device at 0x%llx failed to attach\n", (unsigned long long)device->region.base);
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_FAILED, driver_name);
			return false;
		}

		g_virtio_input_count++;
		ioreg_set_driver_state(device->registry_id, IOREG_STATE_ACTIVE, driver_name);
		return true;
	}

	if (device->device_id == VIRTIO_DEVICE_ID_SOUND) {
		if (!policy->sound) {
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_DISABLED, driver_name);
			return true;
		}

		if (!virtio_snd_attach(device)) {
			kprintf("VirtIOFamily: sound device at 0x%llx failed to attach\n", (unsigned long long)device->region.base);
			ioreg_set_driver_state(device->registry_id, IOREG_STATE_FAILED, driver_name);
			return true;
		}

		g_virtio_sound_count++;
		ioreg_set_driver_state(device->registry_id, IOREG_STATE_ACTIVE, driver_name);
		return true;
	}

	kprintf("VirtIOFamily: unclaimed device ID %u at 0x%llx\n", device->device_id,
			(unsigned long long)device->region.base);
	ioreg_set_driver_state(device->registry_id, IOREG_STATE_UNMATCHED, 0);
	return true;
}

/*
 * Probe the Device-Tree-described MMIO transports, then every registered bus
 * scanner (VirtIO-PCI on x86), and report only live devices and class-driver
 * results. Empty QEMU MMIO slots are expected and remain silent.
 */
bool
virtio_init(const platform_t *platform, const virtio_probe_policy_t *policy)
{
	if (g_virtio_initialized) return true;
	if (platform == 0 || policy == 0) return false;

	for (uint32_t index = 0U; index < platform->virtio_mmio_count; index++) {
		virtio_mmio_device_t device;

		if (!virtio_mmio_probe(&platform->virtio_mmio[index], platform->virtio_mmio_intid[index],
							   platform->virtio_mmio_irq_flags[index],
							   platform->virtio_mmio_dma_coherent[index], &device)) {
			kverbosef("VirtIOFamily: MMIO slot %u base=%llx has no supported active device\n", index,
					  (unsigned long long)platform->virtio_mmio[index].base);
			continue;
		}
		kverbosef("VirtIOFamily: MMIO slot %u base=%llx IRQ=%u discovered device=%u\n", index,
				  (unsigned long long)platform->virtio_mmio[index].base, platform->virtio_mmio_intid[index],
				  device.device_id);
		device.registry_id = ioreg_add_virtio_mmio(device.region.base);

		if (!virtio_bind_device(&device, policy)) return false;
	}

	for (uint32_t index = 0U; index < g_virtio_bus_scanner_count; index++) {
		if (!g_virtio_bus_scanners[index](platform, policy)) return false;
	}

	g_virtio_initialized = true;
	virtio_snd_probe();
	kprintf("VirtIOFamily: %u device(s): %u input, %u block, %u GPU, %u sound, %u network\n", g_virtio_device_count,
			g_virtio_input_count, g_virtio_block_count, g_virtio_gpu_count, g_virtio_sound_count,
			g_virtio_network_count);
	return true;
}

uint32_t
virtio_device_count(void)
{
	return g_virtio_device_count;
}

uint32_t
virtio_input_count(void)
{
	return g_virtio_input_count;
}

uint32_t
virtio_block_count(void)
{
	return g_virtio_block_count;
}

uint32_t
virtio_gpu_count(void)
{
	return g_virtio_gpu_count;
}

uint32_t
virtio_sound_count(void)
{
	return g_virtio_sound_count;
}

void
virtio_dump(void)
{
	/* virtio_init() already printed the device count line. */
	kverbosef("VirtIOFamily: %u device(s): %u input, %u block, %u GPU, %u sound, %u network\n", g_virtio_device_count,
			  g_virtio_input_count, g_virtio_block_count, g_virtio_gpu_count, g_virtio_sound_count,
			  g_virtio_network_count);
	virtio_input_dump();
	virtio_block_dump();
	virtio_gpu_dump();
	virtio_snd_dump();
}
