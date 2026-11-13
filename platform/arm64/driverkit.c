/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/arm64/driverkit.c
 *
 * See platform/driverkit.h.
 */

#include <platform/driverkit.h>

#include <drivers/block/block_device.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <drivers/video/display.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_input.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_chime.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/splash.h>
#include <kern/console/bootlog.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <kern/irq/irq.h>
#include <kern/arm64/gic.h>
#include <kern/arm64/system.h>
#include <platform/platform.h>
#include <platform/rtc.h>

#include <stdbool.h>
#include <stdint.h>

static bool g_boot_splash_shown;

/*
 * driverkit_fail
 *
 * Log which family could not start and report failure to the caller.
 */
static bool
driverkit_fail(const char *message)
{
	kputln(message);
	return false;
}

/*
 * driverkit_fatal
 *
 * For the callbacks that have no way to report failure: log and stop.
 */
static __attribute__((noreturn)) void
driverkit_fatal(const char *message)
{
	kputln(message);

	for (;;) {
		__asm__ volatile("wfe");
	}
}

void
driverkit_service_boot_input(void)
{
	virtio_input_service();

	(void)boot_mode_poll();
}

/*
 * driverkit_show_splash_if_needed
 *
 * Shows the splash on whatever display is currently registered, if it has
 * not already been shown. Passed to virtio_init() so it fires the instant a
 * GPU attaches, mid-scan, before the remaining MMIO slots (input, block) are
 * probed -- the splash then paints its first frame as soon as a display
 * physically exists instead of waiting for the whole bus scan (and the
 * black, unpresented scanout in between) to finish. Also called again after
 * the ramfb-fallback check, in case no GPU was found and the ramfb
 * emergency console registered a display instead.
 */
static void
driverkit_show_splash_if_needed(void)
{
	if (g_boot_splash_shown) return;

	display_device_t *display = display_primary();
	if (display == 0) return;

	if (!boot_splash_show(display)) driverkit_fatal("driverkit_show_splash_if_needed: initialization failed");
	g_boot_splash_shown = true;

	/* The first frame is on its way to the screen: the boot chime is due as soon as the sound device and its file are
	 * there. */
	boot_chime_arm();
}

bool
driverkit_init(driverkit_config_t *config)
{
	if (config == 0) return false;

	gic_init();
	kprintf("GICv3 initialized\n");
	kprintf("cpu0 MPIDR 0x%llx, GICv3 redistributor 0, enabled\n",
			(unsigned long long)arm64_read_mpidr_el1());
	(void)ioreg_add(ioreg_family_platform(), "IOPlatformCPU", "IOPlatformCPU");

	if (!irq_init()) return driverkit_fail("irq: initialization failed");
	kprintf("GICv3 vector table online\n");
	(void)ioreg_add(ioreg_family_platform(), "IOInterruptController", "IOInterruptController");

	if (!input_init()) return driverkit_fail("input: core initialization failed");
	kprintf("input core online\n");

  kprintf("initializating keyboard.\n");
	if (!keyboard_init()) return driverkit_fail("keyboard: initialization failed");
	kprintf("keyboard driver matched\n");

	boot_mode_init();
	kprintf("recovery key watch armed\n");
	(void)ioreg_add(ioreg_family_platform(), "IOBootMode", "IOBootMode");

  kprintf("initializating mouse.\n");
	if (!mouse_init()) return driverkit_fail("mouse_init: initialization failed");
	kprintf("mouse driver matched\n");

  kprintf("initializating block device.\n");
	if (!block_device_init()) return driverkit_fail("block_device_init: initialization failed");
	kprintf("block device core online\n");

  kprintf("initializating display.\n");
	if (!display_init()) return driverkit_fail("display_init: initialization failed");
	kprintf("display core online\n");

  kprintf("initializating real-time clock.\n");
	if (!rtc_init()) return driverkit_fail("rtc_init: initialization failed");
	kprintf("real-time clock online\n");
	(void)ioreg_add(ioreg_family_platform(), "IORTC", "IORTC");

	nxu_boot_log_driver_handoff();
	kprintf("probing MMIO transports\n");

	const platform_t *runtime_platform = platform_get();

	config->input_enabled = !boot_args_component_disabled(BOOT_COMPONENT_INPUT);
	config->block_enabled = !boot_args_component_disabled(BOOT_COMPONENT_BLOCK);
	config->gpu_enabled = !boot_args_component_disabled(BOOT_COMPONENT_GPU);
	config->sound_enabled = !boot_args_component_disabled(BOOT_COMPONENT_SOUND);

	virtio_probe_policy_t virtio_policy = {.input = config->input_enabled,
										   .block = config->block_enabled,
										   .gpu = config->gpu_enabled,
										   .network = true,
										   .sound = config->sound_enabled,
										   .on_gpu_ready = config->gpu_enabled ? driverkit_show_splash_if_needed : 0};

	if (runtime_platform == 0 || !virtio_init(runtime_platform, &virtio_policy)) {
		return driverkit_fail("virtio_init: initialization failed");
	}

	if (config->input_enabled && !keyboard_is_present()) return driverkit_fail("keyboard: VirtIO keyboard not found");
	if (config->input_enabled && !mouse_is_present()) return driverkit_fail("mouse: VirtIO mouse not found");
	if (config->block_enabled && block_device_count() == 0U)
		return driverkit_fail("driverkit_start: VirtIO block device not found");

	if (!config->input_enabled) kprintf("disabled by boot-args\n");
	if (!config->block_enabled) kprintf("disabled by boot-args\n");
	if (!config->gpu_enabled) kprintf("VirtIO GPU disabled by boot-args\n");
	if (!config->sound_enabled) kprintf("VirtIO Sound disabled by boot-args\n");

	if (config->gpu_enabled && (display_primary() == 0 || virtio_gpu_count() == 0U)) {
		kprintf("VirtIO GPU not found\n");

		if (ramfb_console_init(runtime_platform)) {
			kprintf("emergency ramfb console active\n");
		} else {
			kprintf("emergency ramfb console unavailable\n");
		}
	}

	/*
	 * Covers the ramfb-fallback path above: the splash already showed via
	 * the on_gpu_ready callback if a real GPU attached, so this is a no-op
	 * in the common case.
	 */
	driverkit_show_splash_if_needed();

	return true;
}
