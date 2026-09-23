/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/i386/devices_init.c
 *
 * The devices area's boot phases: i386_init_platform() discovers the machine
 * (memory map, PCI, RTC) and i386_init_drivers() brings up the VirtIO-PCI
 * block and input drivers. Each has a self-test hook, run by the boot argument
 * "test=platform" / "test=drivers".
 *
 * Boot arguments read here:
 *
 *   expect-virtio=<n>   platform self-test: at least n VirtIO PCI functions
 *   expect-time=<unix>  platform self-test: RTC within 300 s of this time
 *   expect-block=<n>    drivers self-test: at least n block devices (default 1)
 *   expect-input=<n>    drivers self-test: at least n input devices (default 0)
 *   input-wait=<sec>    drivers self-test: wait up to sec seconds for a
 *                       keyboard and a mouse event (polling)
 *   block-keep=1        drivers self-test: leave the scratch pattern on disk
 *                       (so the host can check the write arrived)
 *   virtio-irq=1        bind the VirtIO drivers to their PCI interrupt line
 *                       through irq_register() instead of polling
 *   expect-display=<n>  drivers self-test: at least n VirtIO-GPU scanouts
 *                       (default 0); when at least 1, a deterministic pattern
 *                       is painted into the framebuffer and presented so the
 *                       host can verify it with a monitor screendump
 */

#include <kern/i386/boot_info.h>
#include <platform/i386/pci.h>
#include <platform/platform.h>
#include <platform/rtc.h>

#include <drivers/block/block_device.h>
#include <drivers/tep/tep_mailbox.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_block.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/virtio/virtio_pci.h>

#include <kern/console/console.h>
#include <kern/machine/barrier.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/timer.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DEVICES_ARG_MAX 24U

#define DEVICES_SELFTEST_SECTORS 16U
#define DEVICES_SELFTEST_MIN_DISK_SECTORS 64U

/* 2020-01-01 and 2100-01-01 in Unix seconds bound a plausible RTC reading. */
#define DEVICES_TIME_MIN 1577836800ULL
#define DEVICES_TIME_MAX 4102444800ULL
#define DEVICES_TIME_SLACK 300ULL

static uint8_t g_selftest_original[DEVICES_SELFTEST_SECTORS * BLOCK_SECTOR_SIZE];
static uint8_t g_selftest_pattern[DEVICES_SELFTEST_SECTORS * BLOCK_SECTOR_SIZE];
static uint8_t g_selftest_readback[DEVICES_SELFTEST_SECTORS * BLOCK_SECTOR_SIZE];

/* Parse a decimal boot argument; false when absent or not a number. */
static bool devices_arg_number(const char *name, uint64_t *value)
{
	char text[DEVICES_ARG_MAX];

	if (!i386_boot_arg(name, text, sizeof(text)) || text[0] == '\0') return false;

	uint64_t number = 0ULL;

	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9') return false;

		number = number * 10ULL + (uint64_t)(text[index] - '0');
	}

	*value = number;
	return true;
}

static uint64_t devices_arg_or(const char *name, uint64_t fallback)
{
	uint64_t value;

	return devices_arg_number(name, &value) ? value : fallback;
}

static void devices_print_time(uint64_t unix_time)
{
	uint64_t days = unix_time / 86400ULL;
	uint32_t seconds_of_day = (uint32_t)(unix_time % 86400ULL);

	/* Civil date from a day count (Hinnant's algorithm). */
	uint64_t shifted = days + 719468ULL;
	uint64_t era = shifted / 146097ULL;
	uint32_t day_of_era = (uint32_t)(shifted - era * 146097ULL);
	uint32_t year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
	uint32_t day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
	uint32_t month_index = (5U * day_of_year + 2U) / 153U;
	uint32_t day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
	uint32_t month = month_index < 10U ? month_index + 3U : month_index - 9U;
	uint32_t year = (uint32_t)(year_of_era + era * 400ULL) + (month <= 2U ? 1U : 0U);

	kprintf(
		"%u-%c%c-%c%c %c%c:%c%c:%c%c UTC",
		year,
		(char)('0' + month / 10U), (char)('0' + month % 10U),
		(char)('0' + day / 10U), (char)('0' + day % 10U),
		(char)('0' + seconds_of_day / 36000U), (char)('0' + seconds_of_day / 3600U % 10U),
		(char)('0' + seconds_of_day / 600U % 6U), (char)('0' + seconds_of_day / 60U % 10U),
		(char)('0' + seconds_of_day % 60U / 10U), (char)('0' + seconds_of_day % 10U)
	);
}

static uint32_t devices_count_virtio_pci(void)
{
	uint32_t count = 0U;

	for (uint32_t index = 0U; index < pci_device_count(); index++) {
		if (pci_device_at(index)->vendor_id == VIRTIO_PCI_VENDOR_ID) count++;
	}

	return count;
}

/* ---- platform phase --------------------------------------------------- */

bool i386_init_platform(const i386_boot_info_t *boot)
{
	(void)boot;

	kputln("i386_init_platform: discovering hardware");

	if (!platform_bootstrap(0)) {
		kputln("i386_init_platform: platform_bootstrap failed");
		return false;
	}

	const platform_t *platform = platform_get();

	if (platform == 0) return false;

	platform_dump(platform);

	if (rtc_init()) {
		uint64_t now = rtc_unix_time();

		kprintf("i386_init_platform: RTC %llu (", (unsigned long long)now);
		devices_print_time(now);
		kputln(")");
	} else {
		kputln("i386_init_platform: RTC not readable");
	}

	kprintf("i386_init_platform: %u PCI function(s), %u VirtIO\n", pci_device_count(), devices_count_virtio_pci());
	return true;
}

static bool devices_check(bool condition, const char *what)
{
	if (!condition) kprintf("i386_init_platform_selftest: FAIL %s\n", what);

	return condition;
}

bool i386_init_platform_selftest(const i386_boot_info_t *boot)
{
	(void)boot;

	bool ok = true;
	const platform_t *platform = platform_get();

	ok &= devices_check(platform != 0, "platform_get() returned null");

	if (platform == 0) return false;

	ok &= devices_check(pci_present(), "PCI configuration mechanism 1 not present");
	ok &= devices_check(platform->pci_device_count == pci_device_count() && pci_device_count() != 0U, "PCI enumeration found nothing");
	ok &= devices_check(pci_find_class(PCI_CLASS_BRIDGE, 0x00U, 0U) != 0, "no PCI host bridge");
	ok &= devices_check(pci_find_class(PCI_CLASS_BRIDGE, 0x01U, 0U) != 0, "no PCI-ISA bridge");

	uint32_t virtio_functions = devices_count_virtio_pci();
	uint64_t expected_virtio = devices_arg_or("expect-virtio", 0ULL);

	kprintf("i386_init_platform_selftest: %u VirtIO function(s), expecting at least %llu\n", virtio_functions, (unsigned long long)expected_virtio);
	ok &= devices_check(virtio_functions >= expected_virtio, "fewer VirtIO PCI functions than expected");

	/* Every sized BAR must be a power of two, or the sizing logic is wrong. */
	for (uint32_t index = 0U; index < pci_device_count(); index++) {
		const pci_device_t *device = pci_device_at(index);

		for (uint32_t bar = 0U; bar < PCI_MAX_BARS; bar++) {
			const pci_bar_t *region = &device->bars[bar];

			if (region->valid && (region->size & (region->size - 1ULL)) != 0ULL) {
				kprintf(
					"i386_init_platform_selftest: FAIL BAR%u of %x:%x has size 0x%llx\n",
					bar,
					(unsigned int)device->vendor_id,
					(unsigned int)device->device_id,
					(unsigned long long)region->size
				);
				ok = false;
			}
		}
	}

	ok &= devices_check(rtc_init(), "rtc_init() failed");

	uint64_t first = rtc_unix_time();

	kprintf("i386_init_platform_selftest: RTC %llu (", (unsigned long long)first);
	devices_print_time(first);
	kputln(")");

	ok &= devices_check(first >= DEVICES_TIME_MIN && first < DEVICES_TIME_MAX, "RTC time implausible");

	uint64_t expected_time;

	if (devices_arg_number("expect-time", &expected_time)) {
		uint64_t difference = first > expected_time ? first - expected_time : expected_time - first;

		kprintf("i386_init_platform_selftest: RTC differs from host by %llu s\n", (unsigned long long)difference);
		ok &= devices_check(difference <= DEVICES_TIME_SLACK, "RTC not within 300 s of the host clock");
	}

	/* The clock must not run backwards, nor jump, between two reads. */
	uint64_t second = rtc_unix_time();

	ok &= devices_check(second >= first && second - first < 5ULL, "RTC not monotonic across two reads");

	if (ok) kputln("i386_init_platform_selftest: PCI enumeration and RTC ok");

	return ok;
}

/* ---- drivers phase ---------------------------------------------------- */

bool i386_init_drivers(const i386_boot_info_t *boot)
{
	(void)boot;

	const platform_t *platform = platform_get();

	if (platform == 0) {
		kputln("i386_init_drivers: no platform");
		return false;
	}

	if (!input_init() || !keyboard_init() || !mouse_init()) {
		kputln("i386_init_drivers: input core initialization failed");
		return false;
	}

	if (!block_device_init()) {
		kputln("i386_init_drivers: block core initialization failed");
		return false;
	}

	if (!display_init()) {
		kputln("i386_init_drivers: display core initialization failed");
		return false;
	}

	if (devices_arg_or("virtio-irq", 0ULL) != 0ULL) {
		virtio_pci_set_irq_mode(true);
		kputln("i386_init_drivers: VirtIO interrupts enabled (PCI INTx via irq_register)");
	} else {
		kputln("i386_init_drivers: VirtIO polling (no interrupts)");
	}

	virtio_probe_policy_t policy = {
		.input = true,
		.block = true,
		.gpu = true,
		.sound = true,
		.on_gpu_ready = 0
	};

	if (!virtio_pci_register()) {
		kputln("i386_init_drivers: virtio_pci_register failed");
		return false;
	}

	if (!virtio_init(platform, &policy)) {
		kputln("i386_init_drivers: virtio_init failed");
		return false;
	}

	kprintf(
		"i386_init_drivers: %u block device(s), %u input device(s), keyboard %s, mouse %s, %u display(s)\n",
		block_device_count(),
		virtio_input_count(),
		keyboard_is_present() ? "present" : "absent",
		mouse_is_present() ? "present" : "absent",
		virtio_gpu_device_count()
	);

	/* The Trusted Enclave link (COM2 when QEMU has a second -serial). Absent, tepOS requests fail closed. */
	(void)tep_mailbox_start();

	return true;
}

/*
 * devices_selftest_block:
 *
 * Raw block-layer round trip on the first device: read sector 0, write a
 * pattern across a scratch region at the end of the disk (wider than the
 * driver's 8-sector request limit so the transfer is split), flush, read it
 * back and compare, prove an out-of-range request is refused, then restore the
 * sectors that were overwritten.
 */
static bool devices_selftest_block(void)
{
	block_device_t device = block_device_first();

	if (device == 0) {
		kputln("i386_init_drivers_selftest: FAIL no block device");
		return false;
	}

	kprintf(
		"i386_init_drivers_selftest: block device %s, %llu sectors of %u bytes, %s, flush %s\n",
		device->name,
		(unsigned long long)device->sector_count,
		device->sector_size,
		device->read_only ? "read-only" : "read-write",
		device->flush_supported ? "supported" : "absent"
	);

	if (!block_device_read(device, 0ULL, 1U, g_selftest_readback)) {
		kputln("i386_init_drivers_selftest: FAIL read of sector 0");
		return false;
	}

	kputs("i386_init_drivers_selftest: sector 0 begins");

	for (uint32_t index = 0U; index < 8U; index++) {
		kputc(' ');
		kputhex_byte(g_selftest_readback[index]);
	}

	kputc('\n');

	if (device->sector_count < DEVICES_SELFTEST_MIN_DISK_SECTORS) {
		kputln("i386_init_drivers_selftest: disk too small for the write test, skipped");
		return true;
	}

	if (device->read_only) {
		kputln("i386_init_drivers_selftest: read-only device, write test skipped");
		return true;
	}

	uint64_t scratch = device->sector_count - DEVICES_SELFTEST_SECTORS;

	if (!block_device_read(device, scratch, DEVICES_SELFTEST_SECTORS, g_selftest_original)) {
		kputln("i386_init_drivers_selftest: FAIL read of scratch sectors");
		return false;
	}

	for (uint32_t index = 0U; index < sizeof(g_selftest_pattern); index++) {
		g_selftest_pattern[index] = (uint8_t)((index * 131U + index / 512U * 17U + 0xA5U) & 0xFFU);
	}

	bool ok = true;

	if (!block_device_write(device, scratch, DEVICES_SELFTEST_SECTORS, g_selftest_pattern)) {
		kputln("i386_init_drivers_selftest: FAIL write of scratch sectors");
		ok = false;
	} else if (!block_device_flush(device)) {
		kputln("i386_init_drivers_selftest: FAIL flush");
		ok = false;
	} else {
		memset(g_selftest_readback, 0, sizeof(g_selftest_readback));

		if (!block_device_read(device, scratch, DEVICES_SELFTEST_SECTORS, g_selftest_readback)) {
			kputln("i386_init_drivers_selftest: FAIL read-back of scratch sectors");
			ok = false;
		} else if (memcmp(g_selftest_pattern, g_selftest_readback, sizeof(g_selftest_pattern)) != 0) {
			kputln("i386_init_drivers_selftest: FAIL read-back differs from the pattern written");
			ok = false;
		} else {
			kprintf("i386_init_drivers_selftest: wrote, flushed and read back %u sectors at %llu\n", DEVICES_SELFTEST_SECTORS, (unsigned long long)scratch);
		}
	}

	if (block_device_read(device, device->sector_count, 1U, g_selftest_readback)) {
		kputln("i386_init_drivers_selftest: FAIL read past the end of the disk was accepted");
		ok = false;
	}

	if (devices_arg_or("block-keep", 0ULL) != 0ULL) {
		kputln("i386_init_drivers_selftest: block-keep set, scratch sectors left as written");
		return ok;
	}

	if (!block_device_write(device, scratch, DEVICES_SELFTEST_SECTORS, g_selftest_original) || !block_device_flush(device)) {
		kputln("i386_init_drivers_selftest: FAIL restoring the scratch sectors");
		ok = false;
	}

	return ok;
}

static uint64_t devices_input_interrupts(void)
{
	uint64_t total = 0ULL;

	for (uint32_t index = 0U; index < virtio_input_device_count(); index++) {
		const virtio_input_device_t *device = virtio_input_device(index);

		if (device != 0) total += device->irq_count;
	}

	return total;
}

static uint64_t devices_input_events(input_device_class_t device_class)
{
	uint64_t total = 0ULL;

	for (uint32_t index = 0U; index < virtio_input_device_count(); index++) {
		const virtio_input_device_t *device = virtio_input_device(index);

		if (device != 0 && device->device_class == device_class) total += device->event_count;
	}

	return total;
}

/*
 * devices_selftest_input:
 *
 * Check the input devices attached and were classified; with input-wait=<s>,
 * poll the event queues until a keyboard and a mouse event have both arrived
 * (the test harness injects them through the QEMU monitor).
 */
static bool devices_selftest_input(void)
{
	uint32_t count = virtio_input_count();
	uint64_t expected = devices_arg_or("expect-input", 0ULL);

	kprintf(
		"i386_init_drivers_selftest: %u input device(s), keyboard %s, mouse %s\n",
		count,
		keyboard_is_present() ? "present" : "absent",
		mouse_is_present() ? "present" : "absent"
	);

	if (count < expected) {
		kprintf("i386_init_drivers_selftest: FAIL expected %llu input device(s)\n", (unsigned long long)expected);
		return false;
	}

	if (expected >= 2ULL && (!keyboard_is_present() || !mouse_is_present())) {
		kputln("i386_init_drivers_selftest: FAIL keyboard or mouse missing");
		return false;
	}

	uint64_t wait_seconds = devices_arg_or("input-wait", 0ULL);

	if (wait_seconds == 0ULL) return true;

	kprintf("i386_init_drivers_selftest: waiting for input events (%llu s)\n", (unsigned long long)wait_seconds);

	/*
	 * With handlers chained on the PIC lines the events must arrive by
	 * interrupt: unmask the CPU and do not poll. Otherwise poll.
	 */
	bool by_interrupt = virtio_pci_irq_bound_count() != 0U;
	uint64_t irq_state = ml_irq_save();

	if (by_interrupt) {
		ml_irq_enable();
		kputln("i386_init_drivers_selftest: input delivered by interrupt");
	} else {
		kputln("i386_init_drivers_selftest: input delivered by polling");
	}

	uint64_t deadline = timer_get_microseconds() + wait_seconds * 1000000ULL;

	while (timer_get_microseconds() < deadline) {
		if (!by_interrupt) virtio_input_service();

		if (devices_input_events(INPUT_DEVICE_KEYBOARD) != 0ULL && devices_input_events(INPUT_DEVICE_MOUSE) != 0ULL) {
			ml_irq_restore(irq_state);
			kprintf(
				"i386_init_drivers_selftest: received %llu keyboard and %llu mouse event(s), %llu interrupt(s), %u queued for userspace\n",
				(unsigned long long)devices_input_events(INPUT_DEVICE_KEYBOARD),
				(unsigned long long)devices_input_events(INPUT_DEVICE_MOUSE),
				(unsigned long long)devices_input_interrupts(),
				input_pending_count()
			);
			return true;
		}

		ml_cpu_relax();
	}

	ml_irq_restore(irq_state);
	kputln("i386_init_drivers_selftest: FAIL timed out waiting for input events");
	return false;
}

/*
 * devices_selftest_display:
 *
 * Paint a deterministic four-quadrant pattern (red, green, blue, white) into
 * the primary display's framebuffer and present it, so a host-side monitor
 * screendump can confirm the pixels VirtIO-GPU actually scanned out, not just
 * that the driver's own command round trips said "ok". Nothing here can
 * check the scanout from inside the guest; a missing display only fails when
 * expect-display=<n> asked for at least one.
 */
static bool devices_selftest_display(void)
{
	uint64_t expected = devices_arg_or("expect-display", 0ULL);
	uint32_t count = virtio_gpu_device_count();

	kprintf("i386_init_drivers_selftest: %u display(s), expecting at least %llu\n", count, (unsigned long long)expected);

	if (count < expected) {
		kprintf("i386_init_drivers_selftest: FAIL expected %llu display(s)\n", (unsigned long long)expected);
		return false;
	}

	if (expected == 0ULL) return true;

	display_device_t *display = display_primary();

	if (display == 0) {
		kputln("i386_init_drivers_selftest: FAIL no primary display registered");
		return false;
	}

	kprintf(
		"i386_init_drivers_selftest: painting test pattern, %ux%u, stride %u\n",
		display->width,
		display->height,
		display->stride
	);

	uint32_t half_width = display->width / 2U;
	uint32_t half_height = display->height / 2U;

	for (uint32_t y = 0U; y < display->height; y++) {
		uint32_t color_top = y < half_height ? 0x00FF0000U /* red */ : 0x000000FFU /* blue */;
		uint32_t color_bottom = y < half_height ? 0x0000FF00U /* green */ : 0x00FFFFFFU /* white */;

		for (uint32_t x = 0U; x < display->width; x++) {
			display->framebuffer[y * display->stride + x] = x < half_width ? color_top : color_bottom;
		}
	}

	if (!display_present_full(display)) {
		kputln("i386_init_drivers_selftest: FAIL display_present_full failed");
		return false;
	}

	kputln("i386_init_drivers_selftest: test pattern presented");
	return true;
}

bool i386_init_drivers_selftest(const i386_boot_info_t *boot)
{
	(void)boot;

	uint64_t expected_blocks = devices_arg_or("expect-block", 1ULL);

	if (virtio_block_count() < expected_blocks) {
		kprintf("i386_init_drivers_selftest: FAIL %u block device(s), expected %llu\n", virtio_block_count(), (unsigned long long)expected_blocks);
		return false;
	}

	bool ok = true;

	if (expected_blocks != 0ULL) ok &= devices_selftest_block();

	ok &= devices_selftest_input();
	ok &= devices_selftest_display();

	if (ok) kputln("i386_init_drivers_selftest: block round trip, input and display ok");

	return ok;
}
