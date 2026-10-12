/* Software presentation through a bootloader-provided GOP framebuffer. */

#include <drivers/video/firmware.h>
#include <drivers/video/display.h>
#include <kern/i386/pmap.h>
#include <kern/memory/heap.h>
#include <kern/console/console.h>
#include <libk/string.h>

static display_device_t display;
static volatile uint32_t *scanout;
static uint32_t pitch;
static bool rgb_order;

static bool
present(display_device_t *device, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	for (uint32_t row = y; row < y + height; row++) {
		for (uint32_t col = x; col < x + width; col++) {
			uint32_t pixel = device->framebuffer[row * device->stride + col];

			if (rgb_order) {
				pixel = (pixel & 0xff00ff00U) | ((pixel & 0xffU) << 16) | ((pixel >> 16) & 0xffU);
			}

			scanout[row * pitch + col] = pixel;
		}
	}

	return true;
}

bool
firmware_display_init(const i386_boot_info_t *boot)
{
	if (display_primary()) {
		return true;
	}

	if (!boot || !boot->multiboot || !(boot->multiboot->flags & (1U << 12))) {
		return false;
	}

	const uint8_t *info = (const void *)boot->multiboot;
	uint64_t address;
	uint32_t stride;
	uint32_t width;
	uint32_t height;

	memcpy(&address, info + 88, 8);
	memcpy(&stride, info + 96, 4);
	memcpy(&width, info + 100, 4);
	memcpy(&height, info + 104, 4);
	if (!address || !width || !height || width > 4096 || height > 2160 || info[108] != 32 || info[109] != 1 ||
		stride % 4 || stride / 4 < width) {
		return false;
	}

	if (info[111] != 8 || info[112] != 8 || info[113] != 8 || info[115] != 8 ||
		!((info[110] == 16 && info[114] == 0) || (info[110] == 0 && info[114] == 16))) {
		return false;
	}

	uint64_t bytes = (uint64_t)stride * height;

	if (bytes > 64 * 1024 * 1024 || address + bytes < address || address + bytes > 0x100000000ULL) {
		return false;
	}

	uint32_t *shadow = kcalloc((size_t)width * height, sizeof(uint32_t));

	if (!shadow) {
		return false;
	}

	void *mapped = 0;

	if (!pmap_map_mmio(address, bytes, &mapped)) {
		(void)kfree(shadow);
		return false;
	}

	scanout = mapped;
	pitch = stride / 4;
	rgb_order = info[110] == 0;
	display = (display_device_t){.framebuffer = shadow,
								 .width = width,
								 .height = height,
								 .stride = width,
								 .bytes_per_pixel = 4,
								 .present = present};
	if (!display_register(&display)) {
		(void)pmap_unmap_mmio(mapped, bytes);
		(void)kfree(shadow);
		return false;
	}

	kprintf("DriverKitDisplayFamily: firmware framebuffer %ux%u address=0x%llx pitch=%u bytes\n", width, height,
			(unsigned long long)address, stride);
	return true;
}
