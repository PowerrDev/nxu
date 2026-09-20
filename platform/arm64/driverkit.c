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
#include <kern/boot/boot_mode.h>
#include <kern/boot/splash.h>
#include <kern/console/bootlog.h>
#include <kern/console/console.h>
#include <kern/console/ioregistry.h>
#include <kern/irq/irq.h>
#include <mach/arm64/gic.h>
#include <mach/arm64/system.h>
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
static bool driverkit_fail(const char *message)
{
	kputln(message);
	return false;
}

/*
 * driverkit_fatal
 *
 * For the callbacks that have no way to report failure: log and stop.
 */
static __attribute__((noreturn))
void driverkit_fatal(const char *message)
{
	kputln(message);

	for (;;) {
		__asm__ volatile("wfe");
	}
}

void driverkit_service_boot_input(void)
{
	virtio_input_service();

	if (boot_mode_poll() && !boot_splash_set_status("Loading startup options...")) {
		driverkit_fatal("driverkit_service_boot_input: recovery status update failed");
	}
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
static void driverkit_show_splash_if_needed(void)
{
	if (g_boot_splash_shown) return;

	display_device_t *display = display_primary();
	if (display == 0) return;

	if (!boot_splash_show(display)) driverkit_fatal("driverkit_show_splash_if_needed: initialization failed");
	g_boot_splash_shown = true;
}

bool driverkit_init(driverkit_config_t *config)
{
	if (config == 0) return false;

	gic_init();
	kputln("kern_init: GICv3 initialized");
	kprintf("IOPlatformCPU: cpu0 MPIDR 0x%llx, GICv3 redistributor 0, enabled\n", (unsigned long long)arm64_read_mpidr_el1());
	(void)ioreg_add(ioreg_family_platform(), "IOPlatformCPU", "IOPlatformCPU");

	if (!irq_init()) return driverkit_fail("irq: initialization failed");
	kputln("IOInterruptController: GICv3 vector table online");
	(void)ioreg_add(ioreg_family_platform(), "IOInterruptController", "IOInterruptController");

	if (!input_init()) return driverkit_fail("input: core initialization failed");
	kputln("IOHIDSystem: input core online");

	if (!keyboard_init()) return driverkit_fail("keyboard: initialization failed");
	kputln("IOHIDSystem: keyboard driver matched");

	boot_mode_init();
	kputln("IOBootMode: recovery key watch armed");
	(void)ioreg_add(ioreg_family_platform(), "IOBootMode", "IOBootMode");

	if (!mouse_init()) return driverkit_fail("VirtIOMouseFamily: initialization failed");
	kputln("IOHIDSystem: mouse driver matched");

	if (!block_device_init()) return driverkit_fail("VirtIOBlockFamily: initialization failed");
	kputln("IOStorageFamily: block device core online");

	if (!display_init()) return driverkit_fail("IODisplayFamily: core initialization failed");
	kputln("IOGraphicsFamily: display core online");

	if (!rtc_init()) return driverkit_fail("rtc_init: initialization failed");
	kputln("IORTC: real-time clock online");
	(void)ioreg_add(ioreg_family_platform(), "IORTC", "IORTC");

	nxu_boot_log_driver_handoff();
	kputln("VirtIOFamily: probing MMIO transports");

	const platform_t *runtime_platform = platform_get();

	config->input_enabled = !boot_args_component_disabled(BOOT_COMPONENT_INPUT);
	config->block_enabled = !boot_args_component_disabled(BOOT_COMPONENT_BLOCK);
	config->gpu_enabled = !boot_args_component_disabled(BOOT_COMPONENT_GPU);

	virtio_probe_policy_t virtio_policy = {
		.input = config->input_enabled,
		.block = config->block_enabled,
		.gpu = config->gpu_enabled,
		.on_gpu_ready = config->gpu_enabled ? driverkit_show_splash_if_needed : 0
	};

	if (runtime_platform == 0 || !virtio_init(runtime_platform, &virtio_policy)) {
		return driverkit_fail("VirtIOFamily: initialization failed");
	}

	if (config->input_enabled && !keyboard_is_present()) return driverkit_fail("keyboard: VirtIO keyboard not found");
	if (config->input_enabled && !mouse_is_present()) return driverkit_fail("mouse: VirtIO mouse not found");
	if (config->block_enabled && block_device_count() == 0U) return driverkit_fail("VirtIOBlockFamily: VirtIO block device not found");

	if (!config->input_enabled) kputln("input: disabled by boot-args");
	if (!config->block_enabled) kputln("VirtIOBlockFamily: disabled by boot-args");
	if (!config->gpu_enabled) kputln("DriverKitDisplayFamily: VirtIO GPU disabled by boot-args");

	if (config->gpu_enabled && (display_primary() == 0 || virtio_gpu_count() == 0U)) {
		kputln("DriverKitDisplayFamily: VirtIO GPU not found");

		if (ramfb_console_init(runtime_platform)) {
			kputln("DriverKitDisplayFamily: emergency ramfb console active");
		} else {
			kputln("DriverKitDisplayFamily: emergency ramfb console unavailable");
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
