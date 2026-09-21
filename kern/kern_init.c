#include <kern/console/console.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_chime.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/splash.h>
#include <kern/boot/nvram.h>
#include <kern/loader/elf.h>
#include <kern/arm64/cache.h>
#include <kern/arm64/exception.h>
#include <kern/arm64/gic.h>
#include <kern/arm64/smp.h>
#include <kern/arm64/system.h>
#include <kern/machine/machine_routines.h>
#include <kern/arm64/timer.h>
#include <kern/arm64/transition.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/video/display.h>
#include <drivers/block/block_device.h>
#include <kern/console/bootlog.h>
#include <kern/console/ioregistry.h>
#include <kern/memory/heap.h>
#include <kern/irq/irq.h>
#include <kern/ipc/ipc_init.h>
#include <kern/ipc/ipc_types.h>
#include <kern/tests/boot_test.h>
#include <kern/tests/post.h>
#include <kern/tests/smp_test.h>
#include <platform/driverkit.h>
#include <kern/process/proc.h>
#include <kern/sched_prism/sched.h>
#include <kern/process/task.h>
#include <kern/process/thread.h>
#include <platform/dtb.h>
#include <platform/platform.h>
#include <platform/rtc.h>
#include <platform/uart.h>
#include <vm/address_space.h>
#include <vm/pmm.h>
#include <vm/user_copy.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>
#include <vfs/devfs.h>
#include <vfs/ext4.h>
#include <vfs/ramfs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <crc32c.h>
#include <stdint.h>
#include <string.h>

/* Timer configuration. */
#define KERNEL_TIMER_HZ 100U

/* Live VMM mapping test. */
#define KERNEL_VMM_TEST_ADDRESS 0xFFFFFFE100000000ULL
#define KERNEL_VMM_TEST_VALUE 0x41524D4F53564D4DULL

/* Kernel virtual arena tests. */

/* Kernel heap tests. */

/* Higher-half direct-map test. */
#define KERNEL_HIGHER_HALF_RAM_TEST_VALUE 0x484947484D415000ULL

typedef uint64_t (*kern_higher_half_probe_t)(
	uint64_t left,
	uint64_t right
);

static __attribute__((noinline))
uint64_t kern_higher_half_probe(uint64_t left, uint64_t right);

static const char g_kernel_link_probe_string[] = "higher-half-linked";

static const char *const volatile g_kernel_link_string_table[] = {
	g_kernel_link_probe_string
};

static kern_higher_half_probe_t const volatile g_kernel_link_function_table[] = {
	kern_higher_half_probe
};

/* PID 1 loaded from the mounted system volume. */
static proc_t g_boot_process;


static __attribute__((noreturn, noinline))
void kern_init_higher_half(void);

/* Fatal boot handling. */

static __attribute__((noreturn))
void kern_halt(void)
{
	for (;;) {
		__asm__ volatile("wfe");
	}
}

static __attribute__((noreturn))
void kern_fail(const char *message)
{
	kputln(message);
	kern_halt();
}












/* Early boot diagnostics and physical-memory validation. */

static void kern_dump_boot_dtb(const dtb_t *device_tree)
{
	kputln("dtb: valid flattened Device Tree");

	kputs("dtb: version: ");
	kputu64(device_tree->version);
	kputc('\n');

	kputs("dtb: total size: ");
	kputu64(device_tree->total_size);
	kputln(" bytes");

	if (!kconsole_verbose()) return;

	kprintf("dtb: address: %p\n", (void *)device_tree->base);

	kprintf("dtb: structure address: %p\n", (void *)device_tree->structure);
	kprintf("dtb: structure size: %llu bytes\n", (unsigned long long)device_tree->structure_size);

	kprintf("dtb: strings address: %p\n", (void *)device_tree->strings);
	kprintf("dtb: strings size: %llu bytes\n", (unsigned long long)device_tree->strings_size);
}

static bool kern_test_pmm(void)
{
	uint64_t test_pages[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		if (!pmm_allocate_page(&test_pages[index])) {
			return false;
		}

		kverbosef("kern_test_pmm: allocated test page %u at 0x%llx\n", index, (unsigned long long)test_pages[index]);
	}

	for (uint32_t index = 0U; index < 3U; index++) {
		if (!pmm_free_page(test_pages[index])) {
			return false;
		}
	}

	return true;
}

/* Virtual-memory self-tests. */

static bool kern_validate_identity_mappings(const platform_t *platform)
{
	uint64_t translated_address;
	uint64_t stack_probe = (uint64_t)&translated_address;

	if (
		!vmm_translate(stack_probe, &translated_address) ||
		translated_address != stack_probe
	) {
		return false;
	}

	if (
		!vmm_translate(platform->uart.base, &translated_address) ||
		translated_address != platform->uart.base
	) {
		return false;
	}

	return true;
}

static bool kern_test_live_vmm(void)
{
	uint64_t physical_address;

	if (!pmm_allocate_page(&physical_address)) {
		return false;
	}

	bool mapped = false;
	bool passed = false;

	uint64_t translated_address;
	uint64_t unmapped_address;
	vmm_page_mapping_t mapping;

	if (!vmm_map_page(
		KERNEL_VMM_TEST_ADDRESS,
		physical_address,
		VMM_MEMORY_NORMAL,
		VMM_PROTECTION_READ_WRITE
	)) {
		goto cleanup;
	}

	mapped = true;

	if (!vmm_query_page(KERNEL_VMM_TEST_ADDRESS, &mapping)) {
		goto cleanup;
	}

	if (
		mapping.physical_address != physical_address ||
		mapping.memory_type != VMM_MEMORY_NORMAL ||
		mapping.protection != VMM_PROTECTION_READ_WRITE
	) {
		goto cleanup;
	}

	if (!vmm_translate(KERNEL_VMM_TEST_ADDRESS, &translated_address)) {
		goto cleanup;
	}

	if (translated_address != physical_address) {
		goto cleanup;
	}

	volatile uint64_t *virtual_word = (volatile uint64_t *)KERNEL_VMM_TEST_ADDRESS;

	volatile uint64_t *physical_word = (volatile uint64_t *)physical_address;

	*virtual_word = KERNEL_VMM_TEST_VALUE;

	if (*physical_word != KERNEL_VMM_TEST_VALUE) {
		goto cleanup;
	}

	if (!vmm_protect_page(
		KERNEL_VMM_TEST_ADDRESS,
		VMM_PROTECTION_READ_ONLY
	)) {
		goto cleanup;
	}

	if (vmm_translate_write(
		KERNEL_VMM_TEST_ADDRESS,
		&translated_address
	)) {
		goto cleanup;
	}

	if (!vmm_translate(
		KERNEL_VMM_TEST_ADDRESS,
		&translated_address
	)) {
		goto cleanup;
	}

	if (translated_address != physical_address) {
		goto cleanup;
	}

	if (!vmm_query_page(KERNEL_VMM_TEST_ADDRESS, &mapping)) {
		goto cleanup;
	}

	if (mapping.protection != VMM_PROTECTION_READ_ONLY) {
		goto cleanup;
	}

	if (!vmm_unmap_page(
		KERNEL_VMM_TEST_ADDRESS,
		&unmapped_address
	)) {
		goto cleanup;
	}

	mapped = false;

	if (unmapped_address != physical_address) {
		goto cleanup;
	}

	if (vmm_translate(
		KERNEL_VMM_TEST_ADDRESS,
		&translated_address
	)) {
		goto cleanup;
	}

	passed = true;

cleanup:
	if (mapped) {
		uint64_t ignored_address;

		if (!vmm_unmap_page(
			KERNEL_VMM_TEST_ADDRESS,
			&ignored_address
		)) {
			return false;
		}
	}

	if (!pmm_free_page(physical_address)) {
		return false;
	}

	return passed;
}

static __attribute__((noinline))
uint64_t kern_higher_half_probe(uint64_t left, uint64_t right)
{
	return (left ^ 0x41524D4F53000000ULL) + (right << 7U);
}

static bool kern_test_higher_half_alias(void)
{
	uint64_t physical_address;

	if (!vmm_kernel_address_to_physical(
		(uint64_t)&kern_higher_half_probe,
		&physical_address
	)) {
		return false;
	}

	uint64_t higher_half_address;

	if (!vmm_physical_to_higher_half(
		physical_address,
		&higher_half_address
	)) {
		return false;
	}

	uint64_t translated_address;

	if (!vmm_translate(
		higher_half_address,
		&translated_address
	)) {
		return false;
	}

	if (translated_address != physical_address) {
		return false;
	}

	const uint64_t left = 0x123456789ABCDEF0ULL;
	const uint64_t right = 0x0000000000000042ULL;

	uint64_t expected = kern_higher_half_probe(left, right);

	kern_higher_half_probe_t volatile probe = (kern_higher_half_probe_t)higher_half_address;

	uint64_t actual = probe(left, right);

	return actual == expected;
}

static bool kern_test_linked_kernel_pointers(void)
{
	const char *string_pointer = g_kernel_link_string_table[0];
	kern_higher_half_probe_t function_pointer = g_kernel_link_function_table[0];

	if (
		!vmm_is_higher_half_address((uint64_t)string_pointer) ||
		!vmm_is_higher_half_address((uint64_t)function_pointer)
	) {
		return false;
	}

	if (
		string_pointer[0] != 'h' ||
		string_pointer[6] != '-' ||
		string_pointer[11] != '-' ||
		string_pointer[18] != '\0'
	) {
		return false;
	}

	const uint64_t left = 0x1020304050607080ULL;
	const uint64_t right = 0x0000000000000011ULL;

	return function_pointer(left, right) == kern_higher_half_probe(left, right);
}

static bool kern_test_higher_half_ram_alias(void)
{
	uint64_t physical_address;

	if (!pmm_allocate_page(&physical_address)) {
		return false;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		physical_address,
		&virtual_address
	)) {
		(void)pmm_free_page(physical_address);
		return false;
	}

	volatile uint64_t *physical_word = (volatile uint64_t *)physical_address;

	volatile uint64_t *virtual_word = (volatile uint64_t *)virtual_address;

	*physical_word = KERNEL_HIGHER_HALF_RAM_TEST_VALUE;

	if (*virtual_word != KERNEL_HIGHER_HALF_RAM_TEST_VALUE) {
		(void)pmm_free_page(physical_address);
		return false;
	}

	*virtual_word = KERNEL_HIGHER_HALF_RAM_TEST_VALUE + 1ULL;

	if (
		*physical_word !=
		KERNEL_HIGHER_HALF_RAM_TEST_VALUE + 1ULL
	) {
		(void)pmm_free_page(physical_address);
		return false;
	}

	return pmm_free_page(physical_address);
}

/* Kernel virtual arena and heap self-tests. */




/* Higher-half transition validation and entry. */

static bool kern_validate_higher_half_state(void)
{
	uint64_t pc = arm64_read_program_counter();
	uint64_t sp = arm64_read_stack_pointer();
	uint64_t vbar = arm64_read_vector_base();

	if (
		!vmm_is_higher_half_address(pc) ||
		!vmm_is_higher_half_address(sp) ||
		!vmm_is_higher_half_address(vbar)
	) {
		return false;
	}

	const platform_t *platform = platform_get();
	const dtb_t *device_tree = dtb_get_boot();

	if (
		platform == 0 ||
		device_tree == 0
	) {
		return false;
	}

	if (
		!vmm_is_higher_half_address(
			(uint64_t)platform
		) ||
		!vmm_is_higher_half_address(
			(uint64_t)device_tree
		)
	) {
		return false;
	}

	uint64_t physical_address;

	if (!vmm_translate(pc, &physical_address)) {
		return false;
	}

	if (!vmm_translate_write(
		sp - 16ULL,
		&physical_address
	)) {
		return false;
	}

	if (!vmm_translate(vbar, &physical_address)) {
		return false;
	}

	if (!vmm_translate_write(
		(uint64_t)platform,
		&physical_address
	)) {
		return false;
	}

	if (!vmm_translate_write(
		(uint64_t)device_tree,
		&physical_address
	)) {
		return false;
	}

	return true;
}

static void kern_dump_higher_half_state(void)
{
	if (!kconsole_verbose()) return;

	kprintf("kern_init: higher-half PC: %p\n", (void *)arm64_read_program_counter());
	kprintf("kern_init: higher-half SP: %p\n", (void *)arm64_read_stack_pointer());
	kprintf("kern_init: higher-half VBAR_EL1: %p\n", (void *)arm64_read_vector_base());
	kprintf("kern_init: platform state: %p\n", (void *)platform_get());
	kprintf("kern_init: Device Tree state: %p\n", (void *)dtb_get_boot());
}

/* Rebase every long-lived kernel pointer before TTBR0 is removed. */

static bool kern_prepare_higher_half_runtime(void)
{
	if (!vmm_rebase_kernel_pointers()) {
		return false;
	}

	if (!pmm_enter_higher_half()) {
		return false;
	}

	if (!dtb_enter_higher_half()) {
		return false;
	}

	if (!gic_enter_higher_half()) {
		return false;
	}

	/* Switch the console last so earlier failures still print safely. */
	if (!uart_enter_higher_half()) {
		return false;
	}

	return true;
}

static bool kern_validate_ttbr0_shutdown(void)
{
	if (
		!vmm_ttbr0_disabled() ||
		!pmm_higher_half_enabled() ||
		!dtb_higher_half_enabled() ||
		!gic_higher_half_enabled() ||
		!uart_higher_half_enabled()
	) {
		return false;
	}

	uint64_t higher_pc = arm64_read_program_counter();

	uint64_t lower_pc;

	if (!vmm_higher_half_to_physical(
		higher_pc,
		&lower_pc
	)) {
		return false;
	}

	uint64_t translated_address;

	/* The old TTBR0 alias must now fault. */
	if (vmm_translate(
		lower_pc,
		&translated_address
	)) {
		return false;
	}

	/* The same instruction must remain reachable through TTBR1. */
	if (
		!vmm_translate(
			higher_pc,
			&translated_address
		) ||
		translated_address != lower_pc
	) {
		return false;
	}

	const dtb_t *device_tree = dtb_get_boot();

	if (
		device_tree == 0 ||
		!vmm_is_higher_half_address(
			(uint64_t)device_tree->base
		) ||
		!vmm_is_higher_half_address(
			vmm_get_ttbr1_root()
		)
	) {
		return false;
	}

	return true;
}

static __attribute__((noreturn))
void kern_enter_higher_half(void)
{
	uint64_t lower_vbar = arm64_read_vector_base();

	uint64_t higher_vbar;
	uint64_t entry_physical;
	uint64_t higher_entry;
	uint64_t translated_address;

	if ((lower_vbar & 0x7FFULL) != 0ULL) {
		kern_fail(
			"kern_init: lower exception vector is misaligned"
		);
	}

	if (!vmm_physical_to_higher_half(
		lower_vbar,
		&higher_vbar
	)) {
		kern_fail(
			"kern_init: higher VBAR conversion failed"
		);
	}

	if (!vmm_kernel_address_to_physical(
		(uint64_t)&kern_init_higher_half,
		&entry_physical
	)) {
		kern_fail("kern_init: entry physical conversion failed");
	}

	if (!vmm_physical_to_higher_half(entry_physical, &higher_entry)) {
		kern_fail("kern_init: higher entry conversion failed");
	}

	if (
		!vmm_translate(
			higher_vbar,
			&translated_address
		) ||
		translated_address != lower_vbar
	) {
		kern_fail(
			"kern_init: higher VBAR translation mismatch"
		);
	}

	if (
		!vmm_translate(higher_entry, &translated_address) ||
		translated_address != entry_physical
	) {
		kern_fail(
			"kern_init: higher entry translation mismatch"
		);
	}

	kputln("kern_init: entering higher-half kernel");

	arm64_enter_higher_half(
		VMM_HIGHER_HALF_BASE,
		higher_vbar,
		higher_entry
	);
}

/*
 * This continuation executes through TTBR1 after the assembly trampoline
 * converts SP_EL1 and VBAR_EL1 to their higher-half aliases.
 */
/*
 * Higher-half runtime bring-up. kern_init_higher_half() reads as the boot
 * sequence; each stage below does one thing and calls kernel_do_post()
 * where its self-tests become meaningful.
 */

/*
 * kern_finish_higher_half_transition
 *
 * Validate the TTBR1 state the transition left behind, rebase the kernel's
 * pointers onto it and shut TTBR0 off for userspace.
 */
static void kern_finish_higher_half_transition(void)
{
	if (!kern_validate_higher_half_state()) {
		kern_fail("kern_init: higher-half state validation failed");
	}

	kputln("kern_init: higher-half transition complete");
	kern_dump_higher_half_state();
	nxu_boot_log_higher_half();

	if (!vmm_validate_linked_kernel_layout()) {
		kern_fail("vmm_validate_linked_kernel_layout: higher-half linked layout validation failed");
	}

	kputln("vmm_validate_linked_kernel_layout: higher-half linked layout validated");

	if (!kern_test_linked_kernel_pointers()) {
		kern_fail("kern_test_linked_kernel_pointers: static higher-half pointer validation failed");
	}

	kputln("kern_test_linked_kernel_pointers: static kernel pointers are higher-half linked");

	/* Permanent TTBR1 runtime state. */

	if (!kern_prepare_higher_half_runtime()) {
		kern_fail("kern_init: higher-half pointer rebasing failed");
	}

	kputln("kern_init: kernel pointers rebased to TTBR1");

	if (!vmm_disable_ttbr0()) {
		kern_fail("vmm_disable_ttbr0: TTBR0 shutdown failed");
	}

	kputln("vmm_disable_ttbr0: TTBR0 disabled");

	if (!kern_validate_ttbr0_shutdown()) {
		kern_fail("kern_validate_ttbr0_shutdown: TTBR0 shutdown validation failed");
	}

	kputln("kern_validate_ttbr0_shutdown: lower address translation rejected");
}

/*
 * kern_init_memory
 *
 * Kernel virtual arena, then the heap, then the allocator self-tests while
 * both are still untouched (they assert exact address reuse).
 */
static void kern_init_memory(void)
{
	kputln("vm_kern_init: initializing kernel virtual arena");

	if (!vm_kern_init()) {
		kern_fail("vm_kern_init: initialization failed");
	}

	kputln("vm_kern_init: kernel virtual arena initialized");

	kputln("heap_init: initializing kernel heap");

	if (!heap_init()) {
		kern_fail("heap_init: initialization failed");
	}

	kputln("heap_init: kernel heap initialized");

	if (!kernel_do_post(KERNEL_POST_MEMORY)) {
		kern_fail("kernel_do_post: memory self-tests failed");
	}
}

/*
 * kern_hold_boot_splash
 *
 * Recovery startup selection is deliberately independent from UIService.framework.
 * Shift+R is sampled against the raw input queue while this kernel-owned
 * splash is visible, before either sevOS or triageOS userspace starts.
 *
 * Returns the primary display (or 0 when there is none) for the later
 * stages that hand the framebuffer over.
 */
static display_device_t *kern_hold_boot_splash(const driverkit_config_t *drivers)
{
	display_device_t *boot_display = display_primary();

	if (boot_display != 0) {
		uint64_t hold_ms = drivers->input_enabled ? 3000ULL : 500ULL;

		if (!boot_splash_wait(hold_ms, drivers->input_enabled ? driverkit_service_boot_input : 0)) {
			kern_fail("boot_splash_wait: hold failed");
		}
	}

	return boot_display;
}

/*
 * kern_init_processes
 *
 * NXPC transport and the process manager, with the kernel process up.
 */
static void kern_init_processes(void)
{
	ipc_init();

	kputln("NXU_ProcessManager: initializing process manager");

	if (!proc_bootstrap()) {
		kern_fail("NXU_ProcessManager: process manager initialization failed");
	}

	if (!proc_validate()) {
		kern_fail("NXU_ProcessManager: process manager validation failed");
	}

	kputln("NXU_ProcessManager: process manager initialized");

	if (proc_kernel() == 0) {
		kern_fail("NXU_ProcessManager: kernel process unavailable");
	}

	kputs("NXU_ProcessManager: kernel process PID: ");
	kputu64(proc_kernel()->p_ident.pid);
	kputc('\n');
}

/*
 * kern_init_filesystem
 *
 * VFS with the ramfs root and the /disk mountpoint. The system volume
 * itself is mounted later, once the block device is known to exist.
 */
static void kern_init_filesystem(void)
{
	kputln("IOVirtualFSDriver initializing virtual filesystem");

	if (!vfs_init()) kern_fail("IOVirtualFSDriver initialization failed");
	if (!ramfs_register()) kern_fail("IOVirtualFSDriver ramfs registration failed");
	if (!ext4_register()) kern_fail("IOVirtualFSDriver ext4 registration failed");
	if (!devfs_register()) kern_fail("IOVirtualFSDriver devfs registration failed");

	vfs_status_t mount_status = vfs_mount("ramfs", 0, "/");
	if (mount_status != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver root ramfs mount failed");

	kputln("IOVirtualFSDriver mounted ramfs at /");

	if (vfs_mkdir("/disk") != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver: /disk mountpoint creation failed");

	if (vfs_mkdir("/dev") != VFS_STATUS_OK || vfs_mount("devfs", 0, "/dev") != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver: devfs mount at /dev failed");

	kputln("IOVirtualFSDriver mounted devfs at /dev");
}

/*
 * kern_init_scheduler
 *
 * Thread subsystem and the processor scheduler, with the bootstrap thread
 * running. The scheduler and context-switch tests run in POST.
 */
static void kern_init_scheduler(void)
{
	kputln("thread_bootstrap: initializing thread subsystem");

	if (!thread_bootstrap()) {
		kern_fail("thread_bootstrap: subsystem initialization failed");
	}

	kputln("sched_bootstrap: initializing processor scheduler");

	if (!sched_bootstrap(proc_task(proc_kernel()))) {
		kern_fail("sched_bootstrap: scheduler initialization failed");
	}

	kputln("thread: thread subsystem initialized");
}

/*
 * kern_mount_system_volume
 *
 * Mount the ext4 system volume from disk0 at /disk.
 */
static void kern_mount_system_volume(void)
{
	kputln("IOFilesystemFamily: mounting disk0 at /disk");

	vfs_status_t ext4_mount_status = vfs_mount("ext4", block_device_first(), "/disk");

	if (ext4_mount_status != VFS_STATUS_OK) {
		kputs("IOFilesystemFamily: mount failed: ");
		kputln(vfs_status_name(ext4_mount_status));
		kern_fail("IOFilesystemFamily: mount failed");
	}

	ext4_dump();
}

/*
 * kern_launch_init_process
 *
 * Pick PID 1 for this boot (bootd, its recovery image, or triageOS), hand
 * the framebuffer over and spawn it.
 */
static void kern_launch_init_process(display_device_t *boot_display)
{
	bool recovery_boot = boot_mode_is_triage_os();
	const char *bootd_recovery_path = "/disk/System/Library/CoreServices/bootd.recovery";
	const char *init_path = recovery_boot
		? "/disk/System/Recovery/triageOS"
		: "/disk/System/Library/CoreServices/bootd";
	const char *init_name = recovery_boot ? "triageOS" : "bootd";
	bool init_uses_fallback = false;

	if (!recovery_boot) {
		bool bootd_matches_recovery = false;
		loader_status_t compare_status = loader_images_equal(init_path, bootd_recovery_path, &bootd_matches_recovery);
		if (compare_status == LOADER_STATUS_OK && !bootd_matches_recovery) {
			kputln("loader_images_equal: bootd primary differs from recovery image");
			init_path = bootd_recovery_path;
			init_uses_fallback = true;
		}
	}

	kprintf("boot: selected environment: %s\n", boot_mode_name());
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");
	kputs("loader_spawn: loading ");
	kputs(init_path);
	kputln(" as PID 1");

	loader_status_t init_status = loader_spawn(proc_kernel(), init_path, init_name, &g_boot_process);
	if (init_status != LOADER_STATUS_OK && !recovery_boot && !init_uses_fallback) {
		kputs("loader_spawn: primary bootd unavailable: ");
		kputln(loader_status_name(init_status));
		kputln("loader_spawn: trying bootd recovery image");
		init_status = loader_spawn(proc_kernel(), bootd_recovery_path, init_name, &g_boot_process);
	}

	if (init_status != LOADER_STATUS_OK || g_boot_process == 0) {
		kputs("loader_spawn: selected environment unavailable: ");
		kputln(loader_status_name(init_status));
		kern_fail("loader_spawn: PID 1 launch failed");
	}

	if (g_boot_process->p_ident.pid != 1U) kern_fail("loader_spawn: selected environment did not receive PID 1");
	kprintf("%s: PID %u ready for scheduler dispatch\n", init_name, g_boot_process->p_ident.pid);
	kputln("VirtIOBlockFamily: VirtIO block device initialized");
}

/*
 * kern_start_scheduler
 *
 * Start the periodic timer, enable IRQs and hand the CPU to userspace. Only
 * returns control to the idle loop when no PID 1 was launched.
 */
static __attribute__((noreturn))
void kern_start_scheduler(const driverkit_config_t *drivers)
{
	if (drivers->input_enabled) kputln("input: VirtIO keyboard and mouse initialized");

	gic_enable_ppi(PHYSICAL_TIMER_INTID, 0x80U);
	kputln("kern_init: physical timer PPI enabled");

	timer_start_periodic(KERNEL_TIMER_HZ);

	kprintf("kern_init: periodic timer started at %u Hz\n", KERNEL_TIMER_HZ);

	/* After the boot CPU's timer (the secondaries copy its rate) and before it takes its first interrupt. */
	(void)smp_boot_secondaries();

	arm64_enable_irqs();
	kputln("kern_init: IRQs enabled");

#if defined(NXU_SMP_TEST)
	/* The secondary CPUs are up and the boot CPU takes interrupts: the SMP suite runs on the boot thread. */
	if (!smp_test_run()) kern_fail("smp_test: failed");
	kputln("smp_test: passed; halting (test build)");
	for (;;) __asm__ volatile("wfe");
#endif

	if (g_boot_process != 0) {
		kprintf("sched: dispatching %s PID %u\n", boot_mode_is_triage_os() ? "triageOS" : "bootd", g_boot_process->p_ident.pid);
		kputln(boot_mode_is_triage_os() ? "kern_init: recovery userspace active" : "kern_init: root userspace services active");

		/* The unified boot runs its UI session here, on this thread, yielding to everything else. */
		boot_test_unified();

		for (;;) {
			if (!sched_yield()) kern_fail("sched: userspace dispatch failed");
		}
	}

	kputln("kern_init: remaining in kernel-only mode");

	uint64_t last_second = 0ULL;
		
	for (;;) {
		uint64_t interrupt_count = timer_get_interrupt_count();
		uint64_t seconds = interrupt_count / KERNEL_TIMER_HZ;

		if (seconds != last_second) {
			last_second = seconds;

			kputs("timer: uptime ");
			kputu64(seconds);
			kputln(" second(s)");
			input_dump_activity();
		}

	}
}

/*
 * kern_init_higher_half
 *
 * The boot sequence after the TTBR1 transition, top to bottom.
 */
static __attribute__((noreturn, noinline))
void kern_init_higher_half(void)
{
	kern_finish_higher_half_transition();
	kern_init_memory();

	driverkit_config_t drivers;

	if (!driverkit_init(&drivers)) {
		kern_fail("driverkit_init: driver bring-up failed");
	}

	display_device_t *boot_display = kern_hold_boot_splash(&drivers);

	kern_init_processes();
	kern_init_filesystem();
	kern_init_scheduler();

	if (!kernel_do_post(KERNEL_POST_CORE)) {
		kern_fail("kernel_do_post: core self-tests failed");
	}

	boot_test_graphical(boot_display);

	virtio_dump();
	block_device_dump();
	display_dump();

	if (drivers.block_enabled) {
		kern_mount_system_volume();

		if (!kernel_do_post(KERNEL_POST_STORAGE)) {
			kern_fail("kernel_do_post: storage self-tests failed");
		}

		ext4_dump();
		vfs_dump();

		boot_test_storage(boot_display);

		/* The file the chime plays is readable now; the thread starts playing once the scheduler runs. */
		boot_chime_start();
		kern_launch_init_process(boot_display);
	}

	kern_start_scheduler(&drivers);
}


/*
 * The lower-half phase builds and validates both translation regimes.
 * It never initializes long-lived kernel services after the transition.
 */
void kern_init(const void *dtb_address)
{
	uint64_t current_el_raw = arm_read_current_el_raw();
	uint32_t current_el = arm_read_current_el();

	kputln("kern_init: uart initialized");
	nxu_boot_log_early(dtb_address, current_el_raw, current_el);

	/* Catch bootstrap faults before touching firmware-provided memory. */
	exception_init();
	kputln("kern_init: early exception vectors installed");

	/* Permanent Device Tree metadata. */

	const volatile uint8_t *dtb_bytes = (const volatile uint8_t *)dtb_address;

	kputs("dtb: first bytes: ");

	for (
		uint32_t index = 0U;
		index < 4U;
		index++
	) {
		kputhex_byte(
			dtb_bytes[index]
		);

		kputc(' ');
	}

	kputc('\n');

	kputln("dtb: bootstrap begin");

	if (!dtb_bootstrap(dtb_address)) {
		kern_fail("dtb_bootstrap: invalid Device Tree Blob");
	}

	kputln("dtb_bootstrap: bootstrap complete");

	const dtb_t *device_tree = dtb_get_boot();

	if (device_tree == 0) {
		kern_fail(
			"dtb_t device_tree: boot Device Tree unavailable"
		);
	}

	kputln("nvram_bootstrap: bootstrap begin");
	if (!nvram_bootstrap(device_tree)) kern_fail("nvram_bootstrap: bootstrap failed");
	kputln("nvram_bootstrap: bootstrap complete");

	kputln("boot_args: initialization begin");
	if (!boot_args_init()) kern_fail("boot_args_init: initialization failed");
	kputln("boot_args_init: initialization complete");
	boot_args_dump();

	/*
	 * New threads may run on the boot CPU only until the subsystems they run in
	 * are SMP-safe (doc/kern/smp.md lists what is not). `sched.affinity=all` lifts
	 * that for every thread created from here on, to find out what breaks.
	 */
	char affinity_policy[8];

	if (boot_arg_value("sched.affinity", affinity_policy, sizeof(affinity_policy)) && strcmp(affinity_policy, "all") == 0) {
		nxu_cpuset_t everywhere;

		cpuset_fill(&everywhere, NXU_MAX_CPUS);
		processor_set_default_affinity(&everywhere);
		kputln("sched: new threads may run on any CPU (sched.affinity=all)");
	}

	kern_dump_boot_dtb(device_tree);

	kputln("dtb: walking structure block");

	// if (!dtb_dump(device_tree)) {
	// 	kern_fail(
	// 		"dtb_dump: malformed structure block"
	// 	);
	// }

	/* Permanent platform description. */

	kputln("platform: discovering hardware");

	if (!platform_bootstrap(device_tree)) {
		kern_fail("platform: hardware discovery failed");
	}

	const platform_t *platform = platform_get();

	if (platform == 0) {
		kern_fail("platform: permanent platform state unavailable");
	}

	kputln("platform: hardware discovery complete");

	platform_dump(platform);
	nxu_boot_log_platform(device_tree, platform);

	/* Physical memory manager. */
	kputln("pmm_init: initializing physical memory manager");

	if (!pmm_init(
		platform,
		device_tree
	)) {
		kern_fail("pmm_init: initialization failed");
	}

	kputln("pmm_init: physical memory manager initialized");

	pmm_dump();

	if (!kern_test_pmm()) {
		kern_fail("kern_test_pmm: allocation/free test failed");
	}

	kputln("kern_test_pmm: allocation/free test passed");

	/* Exception level and vector table. */

	kputs("kern_init: current raw exception level: 0x");
	kputhex_byte((uint8_t)current_el_raw);
	kputc('\n');
	kputs("kern_init: current exception level: EL");

	kputc((char)('0' + current_el));

	kputc('\n');

	/* Note: Exception vectors were installed during early bootstrap. */

	/* TTBR0 identity address space. */
	kputln("vmm_init: building identity address space");

	if (!vmm_init(platform)) {
		kern_fail("vmm_init: initialization failed");
	}

	kputln("vmm_init: stage 1 MMU enabled");

	vmm_dump();

	if (!vmm_validate_kernel_permissions()) {
		kern_fail("vmm_validate_kernel_permissions: kernel permission validation failed");
	}

	kputln("vmm_validate_kernel_permissions: kernel permissions validated");

	if (!kern_validate_identity_mappings(
		platform
	)) {
		kern_fail("kern_validate_identity_mappings: identity translation test failed");
	}

	kputln("kern_validate_identity_mappings: stack identity translation passed");
	kputln("kern_validate_identity_mappings: UART identity translation passed");

	/* CPU caches and live table updates. */
	kputln("cache_init: initializing CPU caches");

	if (!cache_init()) {
		kern_fail("cache_init: initialization failed");
	}

	kputln("cache_init: CPU caches initialized");

	cache_dump();

	/* TTBR1 kernel alias and full direct map. */
	kputln("vmm_init_higher_half_alias: installing TTBR1 higher-half alias");

	if (!vmm_init_higher_half_alias()) {
		kern_fail("vmm_init_higher_half_alias: TTBR1 higher-half initialization failed");
	}

	kputln("vmm_init_higher_half_alias: TTBR1 higher-half alias installed");

	if (kconsole_verbose()) vmm_dump_higher_half();

	/* The live mapping test now targets a TTBR1 virtual address */
	kputln("kern_test_live_vmm: testing live page mappings");

	if (!kern_test_live_vmm()) {
		kern_fail("kern_test_live_vmm: live page mapping test failed");
	}

	kputln("kern_test_live_vmm: live page mapping test passed");

	kputln("kern_test_higher_half_alias: testing higher-half execution");

	if (!kern_test_higher_half_alias()) {
		kern_fail("kern_test_higher_half_alias: higher-half execution test failed");
	}

	kputln("kern_test_higher_half_alias: higher-half execution test passed");

	kputln("vmm_map_higher_half_direct_map: building higher-half direct map");

	if (!vmm_map_higher_half_direct_map(
		platform
	)) {
		kern_fail("vmm_map_higher_half_direct_map: higher-half direct map failed");
	}

	kputln("vmm_map_higher_half_direct_map: higher-half direct map installed");

	if (!vmm_validate_higher_half_direct_map(
		platform
	)) {
		kern_fail("vmm_validate_higher_half_direct_map: higher-half direct map validation failed");
	}

	kputln("vmm_validate_higher_half_direct_map: higher-half direct map validated");

	if (!kern_test_higher_half_ram_alias()) {
		kern_fail("kern_test_higher_half_ram_alias: higher-half RAM alias test failed");
	}

	kputln("kern_test_higher_half_ram_alias: higher-half RAM alias test passed");

	vmm_dump_higher_half();

	/* This call never returns to the lower TTBR0 alias. */
	kern_enter_higher_half();
}
