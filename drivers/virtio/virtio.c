#include <kern/console/console.h>
#include <drivers/virtio/virtio.h>

#include <drivers/virtio/virtio_block.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio_mmio.h>

#include <stdbool.h>
#include <stdint.h>

static uint32_t g_virtio_device_count;
static uint32_t g_virtio_input_count;
static uint32_t g_virtio_block_count;
static uint32_t g_virtio_gpu_count;
static bool g_virtio_initialized;

/*
 * Probe the Device-Tree-described transports and report only live devices and
 * class-driver results. Empty QEMU MMIO slots are expected and remain silent.
 */
bool virtio_init(const platform_t *platform, const virtio_probe_policy_t *policy)
{
	if (g_virtio_initialized) return true;
	if (platform == 0 || policy == 0) return false;

	for (uint32_t index = 0U; index < platform->virtio_mmio_count; index++) {
		virtio_mmio_device_t device;

		if (!virtio_mmio_probe(&platform->virtio_mmio[index], platform->virtio_mmio_intid[index], platform->virtio_mmio_irq_flags[index], &device)) continue;

		g_virtio_device_count++;

		if (device.device_id == VIRTIO_DEVICE_ID_GPU) {
			if (!policy->gpu) continue;

			if (!virtio_gpu_attach(&device)) {
				kprintf("VirtIOFamily: GPU at 0x%llx failed to attach\n", (unsigned long long)device.region.base);
				continue;
			}

			g_virtio_gpu_count++;
			if (policy->on_gpu_ready != 0) policy->on_gpu_ready();
			continue;
		}

		if (device.device_id == VIRTIO_DEVICE_ID_BLOCK) {
			if (!policy->block) continue;

			if (!virtio_block_attach(&device)) {
				kprintf("VirtIOFamily: block device at 0x%llx failed to attach\n", (unsigned long long)device.region.base);
				return false;
			}

			g_virtio_block_count++;
			continue;
		}

		if (device.device_id == VIRTIO_DEVICE_ID_INPUT) {
			if (!policy->input) continue;

			if (!virtio_input_attach(&device)) {
				kprintf("VirtIOFamily: input device at 0x%llx failed to attach\n", (unsigned long long)device.region.base);
				return false;
			}

			g_virtio_input_count++;
			continue;
		}

		kprintf("VirtIOFamily: unclaimed device ID %u at 0x%llx\n", device.device_id, (unsigned long long)device.region.base);
	}

	g_virtio_initialized = true;
	kprintf("VirtIOFamily: %u device(s): %u input, %u block, %u GPU\n", g_virtio_device_count, g_virtio_input_count, g_virtio_block_count, g_virtio_gpu_count);
	return true;
}

uint32_t virtio_device_count(void)
{
	return g_virtio_device_count;
}

uint32_t virtio_input_count(void)
{
	return g_virtio_input_count;
}

uint32_t virtio_block_count(void)
{
	return g_virtio_block_count;
}

uint32_t virtio_gpu_count(void)
{
	return g_virtio_gpu_count;
}

void virtio_dump(void)
{
	kprintf("VirtIOFamily: %u device(s): %u input, %u block, %u GPU\n", g_virtio_device_count, g_virtio_input_count, g_virtio_block_count, g_virtio_gpu_count);
	virtio_input_dump();
	virtio_block_dump();
	virtio_gpu_dump();
}
