#include <kern/console/console.h>
#include <kern/boot/boot_args.h>
#include <kern/boot/boot_mode.h>
#include <kern/boot/splash.h>
#include <kern/boot/nvram.h>
#include <kern/loader/elf.h>
#include <mach/arm64/cache.h>
#include <mach/arm64/exception.h>
#include <mach/arm64/gic.h>
#include <mach/arm64/system.h>
#include <mach/machine/machine_routines.h>
#include <mach/arm64/timer.h>
#include <mach/arm64/transition.h>
#include <drivers/input/input.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <drivers/virtio/virtio.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/video/display.h>
#include <drivers/video/ramfb_console.h>
#include <drivers/video/ui_service_host.h>
#include <drivers/block/block_device.h>
#include <kern/aqua/window_server.h>
#include <kern/console/bootlog.h>
#include <kern/console/ioregistry.h>
#include <kern/memory/heap.h>
#include <kern/irq/irq.h>
#include <kern/ipc/ipc_init.h>
#include <kern/tests/ipc_test.h>
#include <kern/tests/vm_shm_test.h>
#include <kern/tests/vm_map_test.h>
#include <kern/tests/ipc_process_test.h>
#include <kern/tests/thread_process_test.h>
#include <kern/tests/socket_process_test.h>
#include <kern/tests/xamethyst_process_test.h>
#include <kern/tests/windowserver_process_test.h>
#include <kern/tests/about_sevos_process_test.h>
#include <kern/ipc/ipc_types.h>
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
#include <vfs/ext4.h>
#include <vfs/ramfs.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <crc32c.h>
#include <stdint.h>
#include <string.h>

/* Timer configuration. */
#define KERNEL_TIMER_HZ 100U
#define PHYSICAL_TIMER_INTID 30U

/* Live VMM mapping test. */
#define KERNEL_VMM_TEST_ADDRESS 0xFFFFFFE100000000ULL
#define KERNEL_VMM_TEST_VALUE 0x41524D4F53564D4DULL

/* Kernel virtual arena tests. */
#define KERNEL_VM_KERN_TEST_A_SIZE (PMM_PAGE_SIZE * 2ULL + 128ULL)
#define KERNEL_VM_KERN_TEST_B_SIZE (PMM_PAGE_SIZE + 256ULL)
#define KERNEL_VM_KERN_TEST_VALUE 0x564D4B45524E0000ULL

/* Kernel heap tests. */
#define KERNEL_HEAP_LARGE_TEST_SIZE (PMM_PAGE_SIZE * 3ULL + 333ULL)
#define KERNEL_HEAP_LARGE_TEST_VALUE 0xA5U

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

static volatile uint32_t g_sched_context_test_stage;

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

/*
 * Poll recovery startup input while the splash owns the scanout. IRQ delivery
 * is not required here; the VirtIO input service drains the transport directly.
 */
static void kern_boot_splash_service(void)
{
	virtio_input_service();
	if (boot_mode_poll() && !boot_splash_set_status("Loading startup options...")) {
		kern_fail("kern_boot_splash_service: recovery status update failed");
	}
}

static bool g_boot_splash_shown;

/*
 * Shows the splash on whatever display is currently registered, if it has
 * not already been shown. Passed to virtio_init() so it fires the instant a
 * GPU attaches, mid-scan, before the remaining MMIO slots (input, block) are
 * probed -- the splash then paints its first frame as soon as a display
 * physically exists instead of waiting for the whole bus scan (and the
 * black, unpresented scanout in between) to finish. Also called again after
 * the ramfb-fallback check further down, in case no GPU was found and the
 * ramfb emergency console registered a display instead.
 */
static void kern_boot_splash_show_if_needed(void)
{
	if (g_boot_splash_shown) return;

	display_device_t *display = display_primary();
	if (display == 0) return;

	if (!boot_splash_show(display)) kern_fail("kern_boot_splash_show_if_needed: initialization failed");
	g_boot_splash_shown = true;
}

/*
 * kern_sched_context_test_continue
 *
 * Run once on a freshly constructed kernel-thread stack, then voluntarily
 * yield back to the bootstrap thread. The bootstrap side terminates and
 * reaps this thread while it is queued, validating both directions of the
 * AArch64 context switch before userspace is dispatched.
 */
static void kern_sched_context_test_continue(void *parameter)
{
	volatile uint32_t *stage = (volatile uint32_t *)parameter;

	*stage = 1U;

	if (!sched_yield()) {
		*stage = UINT32_MAX;
		return;
	}

	*stage = 2U;
}

static const char g_vfs_test_message[] = "NXU VFS ramfs self-test\n";

/*
 * kern_test_vfs:
 *
 * Validate pathname traversal, vnode creation, per-process descriptors and
 * file offsets against the bootstrap ramfs root.
 */
static bool kern_test_vfs(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) return false;

	filedesc_t filedesc = &kernel_proc->p_fd;

	if (vfs_mkdir("/tmp") != VFS_STATUS_OK) return false;

	uint32_t descriptor;
	vfs_status_t status = vfs_open(
		filedesc,
		"/tmp/hello",
		VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	uint64_t message_size = sizeof(g_vfs_test_message) - 1ULL;
	uint64_t written_size;
	status = vfs_write(filedesc, descriptor, g_vfs_test_message, message_size, &written_size);
	if (status != VFS_STATUS_OK || written_size != message_size) goto fail_close;

	if (vfs_seek(filedesc, descriptor, 0ULL) != VFS_STATUS_OK) goto fail_close;

	char buffer[sizeof(g_vfs_test_message)];
	memset(buffer, 0, sizeof(buffer));

	uint64_t read_size;
	status = vfs_read(filedesc, descriptor, buffer, message_size, &read_size);
	if (status != VFS_STATUS_OK || read_size != message_size) goto fail_close;

	for (uint64_t index = 0ULL; index < message_size; index++) {
		if (buffer[index] != g_vfs_test_message[index]) goto fail_close;
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	vnode_t vnode;
	status = vfs_lookup("/tmp/./../tmp/hello", &vnode);
	if (status != VFS_STATUS_OK) return false;

	bool vnode_valid = vnode->v_type == VNODE_TYPE_REGULAR && vnode->v_size == message_size;
	vnode_rele(vnode);
	if (!vnode_valid) return false;

	status = vfs_read(filedesc, descriptor, buffer, sizeof(buffer), &read_size);
	if (status != VFS_STATUS_BAD_FD) return false;

	return filedesc->fd_open_count == 0U;

fail_close:
	(void)vfs_close(filedesc, descriptor);
	return false;
}

/*
 * kern_test_block_device:
 *
 * Read sector zero through the transport-independent block layer. The test is
 * intentionally read-only so the development disk can later be formatted as
 * ext4 without a kernel bootstrap test modifying filesystem metadata.
 */
static bool kern_test_block_device(void)
{
	block_device_t device = block_device_first();

	if (device == 0) return false;

	uint8_t sector[BLOCK_SECTOR_SIZE];

	memset(
		sector,
		0,
		sizeof(sector)
	);

	if (!block_device_read(
		device,
		0ULL,
		1U,
		sector
	)) {
		return false;
	}

	kputs(
		"VirtIOBlockFamily: sector 0 first bytes:"
	);

	for (
		uint32_t index = 0U;
		index < 16U;
		index++
	) {
		kputc(' ');
		kputhex_byte(
			sector[index]
		);
	}

	kputc('\n');

	return true;
}


static const char g_ext4_test_message[] = "Hello from the NXU ext4 driver!\n";
static const char g_ext4_system_message[] = "NXU System volume\n";
static const char g_ext4_write_message[] = "NXU writable ext4 foundation\n";
static const char g_ext4_persist_message[] = "NXU persistent ext4 marker\n";
static const char g_ext4_tail_message[] = "tail-after-sparse-growth";
static const char g_jbd2_recovery_message[] = "NXU JBD2 committed metadata survived the crash.\n";

/*
 * kern_test_crc32c:
 *
 * Verify the software Castagnoli implementation against the canonical
 * 123456789 check value using the running-checksum convention used by ext4.
 */
static bool kern_test_crc32c(void)
{
	static const char vector[] = "123456789";
	return crc32c(~0U, vector, sizeof(vector) - 1U) == 0x1CF96D7CU;
}

static bool kern_ext4_read_exact(
	filedesc_t filedesc,
	const char *path,
	const char *expected,
	uint64_t size
)
{
	uint32_t descriptor;
	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) return false;

	char buffer[128];
	if (size > sizeof(buffer)) {
		(void)vfs_close(filedesc, descriptor);
		return false;
	}
	memset(buffer, 0, sizeof(buffer));

	uint64_t read_size;
	vfs_status_t status = vfs_read(filedesc, descriptor, buffer, size, &read_size);
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	if (status != VFS_STATUS_OK || read_size != size) return false;
	for (uint64_t index = 0ULL; index < size; index++) {
		if (buffer[index] != expected[index]) return false;
	}
	return true;
}

/*
 * kern_test_ext4:
 *
 * Exercise the complete writable ext4 foundation through VFS. The test covers
 * existing-file reads, inode and block allocation, extent growth, directory
 * insertion, truncate shrink/grow semantics, sparse reads, unlink and a
 * persistent marker which is observed on subsequent boots.
 */
static bool kern_test_ext4(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) return false;
	filedesc_t filedesc = &kernel_proc->p_fd;

	if (!kern_ext4_read_exact(
		filedesc,
		"/disk/hello.txt",
		g_ext4_test_message,
		sizeof(g_ext4_test_message) - 1ULL
	)) return false;

	if (!kern_ext4_read_exact(
		filedesc,
		"/disk/System/README.txt",
		g_ext4_system_message,
		sizeof(g_ext4_system_message) - 1ULL
	)) return false;

	vfs_status_t status = vfs_mkdir("/disk/NXU");
	if (status != VFS_STATUS_OK && status != VFS_STATUS_EXISTS) return false;

	uint32_t descriptor;
	status = vfs_open(
		filedesc,
		"/disk/NXU/write-test.bin",
		VFS_OPEN_READ | VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	uint8_t block[1024];
	for (uint32_t chunk = 0U; chunk < 12U; chunk++) {
		for (uint32_t index = 0U; index < sizeof(block); index++) {
			block[index] = (uint8_t)(chunk ^ index);
		}
		uint64_t written;
		status = vfs_write(filedesc, descriptor, block, sizeof(block), &written);
		if (status != VFS_STATUS_OK || written != sizeof(block)) goto fail_close;
	}

	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	vnode_t vnode;
	status = vfs_lookup("/disk/NXU/write-test.bin", &vnode);
	if (status != VFS_STATUS_OK) return false;
	status = vnode_truncate(vnode, 5000ULL);
	if (status == VFS_STATUS_OK) status = vnode_truncate(vnode, 9000ULL);
	vnode_rele(vnode);
	if (status != VFS_STATUS_OK) return false;

	status = vfs_open(
		filedesc,
		"/disk/NXU/write-test.bin",
		VFS_OPEN_READ | VFS_OPEN_WRITE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;

	if (vfs_seek(filedesc, descriptor, 5000ULL) != VFS_STATUS_OK) goto fail_close;

	uint8_t zeros[128];
	memset(zeros, 0xA5, sizeof(zeros));
	uint64_t read_size;
	status = vfs_read(filedesc, descriptor, zeros, sizeof(zeros), &read_size);
	if (status != VFS_STATUS_OK || read_size != sizeof(zeros)) goto fail_close;
	for (uint32_t index = 0U; index < sizeof(zeros); index++) {
		if (zeros[index] != 0U) goto fail_close;
	}

	if (vfs_seek(filedesc, descriptor, 8192ULL) != VFS_STATUS_OK) goto fail_close;
	uint64_t written;
	status = vfs_write(
		filedesc,
		descriptor,
		g_ext4_tail_message,
		sizeof(g_ext4_tail_message) - 1ULL,
		&written
	);
	if (status != VFS_STATUS_OK || written != sizeof(g_ext4_tail_message) - 1ULL) goto fail_close;
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	status = vfs_open(
		filedesc,
		"/disk/NXU/delete-me.txt",
		VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) return false;
	status = vfs_write(
		filedesc,
		descriptor,
		g_ext4_write_message,
		sizeof(g_ext4_write_message) - 1ULL,
		&written
	);
	if (status != VFS_STATUS_OK) goto fail_close;
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

	if (vfs_unlink("/disk/NXU/delete-me.txt") != VFS_STATUS_OK) return false;
	status = vfs_lookup("/disk/NXU/delete-me.txt", &vnode);
	if (status != VFS_STATUS_NOT_FOUND) {
		if (status == VFS_STATUS_OK) vnode_rele(vnode);
		return false;
	}

	status = vfs_lookup("/disk/NXU/journal-recovery.txt", &vnode);
	if (status == VFS_STATUS_OK) {
		vnode_rele(vnode);
		if (!kern_ext4_read_exact(
			filedesc,
			"/disk/NXU/journal-recovery.txt",
			g_jbd2_recovery_message,
			sizeof(g_jbd2_recovery_message) - 1ULL
		)) return false;
		kputln("jbd2: committed crash-test transaction recovered");
	} else if (status != VFS_STATUS_NOT_FOUND) {
		return false;
	}

	status = vfs_lookup("/disk/NXU/persistent.txt", &vnode);
	if (status == VFS_STATUS_OK) {
		vnode_rele(vnode);
		if (!kern_ext4_read_exact(
			filedesc,
			"/disk/NXU/persistent.txt",
			g_ext4_persist_message,
			sizeof(g_ext4_persist_message) - 1ULL
		)) return false;
		kputln("IOFilesystemFamily: persistence marker recovered from previous boot");
	} else if (status == VFS_STATUS_NOT_FOUND) {
		status = vfs_open(
			filedesc,
			"/disk/NXU/persistent.txt",
			VFS_OPEN_WRITE | VFS_OPEN_CREATE,
			&descriptor
		);
		if (status != VFS_STATUS_OK) return false;
		status = vfs_write(
			filedesc,
			descriptor,
			g_ext4_persist_message,
			sizeof(g_ext4_persist_message) - 1ULL,
			&written
		);
		if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) return false;

		if (status != VFS_STATUS_OK || written != sizeof(g_ext4_persist_message) - 1ULL) return false;
		kputln("IOFilesystemFamily: persistence marker created; reboot to verify persistence");
	} else {
		return false;
	}

	kputs("IOFilesystemFamily: /hello.txt: ");
	kputs(g_ext4_test_message);
	return filedesc->fd_open_count == 0U;

fail_close:
	(void)vfs_close(filedesc, descriptor);
	return false;
}

#if defined(NXU_JOURNAL_CRASH_TEST)
/*
 * kern_run_jbd2_crash_test:
 *
 * Create a stable empty file, arm the journal crash point, then perform one
 * write whose metadata reaches a durable JBD2 commit record but is not
 * checkpointed home. The kernel halts immediately afterward. A normal reboot
 * must replay the transaction and recover the file size/block mapping.
 */
static void kern_run_jbd2_crash_test(void)
{
	proc_t kernel_proc = proc_kernel();
	if (kernel_proc == 0) kern_fail("jbd2: crash-test kernel proc missing");
	filedesc_t filedesc = &kernel_proc->p_fd;
	uint32_t descriptor;

	vfs_status_t status = vfs_open(
		filedesc,
		"/disk/NXU/journal-recovery.txt",
		VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) kern_fail("jbd2: crash-test file preparation failed");
	if (vfs_close(filedesc, descriptor) != VFS_STATUS_OK) kern_fail("jbd2: crash-test close failed");
	if (!ext4_debug_arm_journal_crash()) kern_fail("jbd2: failed to arm crash point");

	status = vfs_open(
		filedesc,
		"/disk/NXU/journal-recovery.txt",
		VFS_OPEN_WRITE,
		&descriptor
	);
	if (status != VFS_STATUS_OK) kern_fail("jbd2: crash-test reopen failed");

	uint64_t written = 0ULL;
	status = vfs_write(
		filedesc,
		descriptor,
		g_jbd2_recovery_message,
		sizeof(g_jbd2_recovery_message) - 1ULL,
		&written
	);
	(void)vfs_close(filedesc, descriptor);
	if (status != VFS_STATUS_IO_ERROR) kern_fail("jbd2: crash point did not stop checkpoint");
	if (!ext4_debug_journal_crash_reached()) kern_fail("jbd2: durable crash point was not reached");

	kputln("jbd2: crash-test transaction committed");
	kputln("jbd2: durable commit verified before simulated power loss");
	kputln("jbd2: home metadata intentionally not checkpointed");
	kputln("jbd2: halt complete; close QEMU and run make run");
	ml_irq_disable();
	for (;;) __asm__ volatile("wfe");
}
#endif

/* Early boot diagnostics and physical-memory validation. */

static void kern_dump_boot_dtb(const dtb_t *device_tree)
{
	kputln("dtb: valid flattened Device Tree");

	kputs("dtb: address: ");
	kputhex64((uint64_t)device_tree->base);
	kputc('\n');

	kputs("dtb: version: ");
	kputu64(device_tree->version);
	kputc('\n');

	kputs("dtb: total size: ");
	kputu64(device_tree->total_size);
	kputln(" bytes");

	kputs("dtb: structure address: ");
	kputhex64((uint64_t)device_tree->structure);
	kputc('\n');

	kputs("dtb: structure size: ");
	kputu64(device_tree->structure_size);
	kputln(" bytes");

	kputs("dtb: strings address: ");
	kputhex64((uint64_t)device_tree->strings);
	kputc('\n');

	kputs("dtb: strings size: ");
	kputu64(device_tree->strings_size);
	kputln(" bytes");
}

static bool kern_test_pmm(void)
{
	uint64_t test_pages[3];

	for (uint32_t index = 0U; index < 3U; index++) {
		if (!pmm_allocate_page(&test_pages[index])) {
			return false;
		}

		kputs("pmm: allocated test page ");
		kputu64(index);
		kputs(" at ");
		kputhex64(test_pages[index]);
		kputc('\n');
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

static bool kern_test_vm_kern(void)
{
	void *allocation_a = 0;
	void *allocation_b = 0;
	void *reused_allocation = 0;

	bool allocation_a_active = false;
	bool allocation_b_active = false;
	bool reused_allocation_active = false;

	bool passed = false;
	bool cleanup_passed = true;

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_A_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&allocation_a
	)) {
		goto cleanup;
	}

	allocation_a_active = true;

	for (uint64_t index = 0ULL; index < 3ULL; index++) {
		uint64_t virtual_address =
			(uint64_t)allocation_a +
			index * PMM_PAGE_SIZE;

		vmm_page_mapping_t mapping;

		if (!vmm_query_page(
			virtual_address,
			&mapping
		)) {
			goto cleanup;
		}

		if (
			mapping.memory_type != VMM_MEMORY_NORMAL ||
			mapping.protection !=
			VMM_PROTECTION_READ_WRITE
		) {
			goto cleanup;
		}

		uint64_t value = KERNEL_VM_KERN_TEST_VALUE + index;

		uint64_t direct_map_address;

		if (!vmm_physical_to_higher_half(
			mapping.physical_address,
			&direct_map_address
		)) {
			goto cleanup;
		}

		volatile uint64_t *virtual_word = (volatile uint64_t *)virtual_address;

		volatile uint64_t *physical_word = (volatile uint64_t *)direct_map_address;

		*virtual_word = value;

		if (*physical_word != value) {
			goto cleanup;
		}
	}

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_B_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&allocation_b
	)) {
		goto cleanup;
	}

	allocation_b_active = true;

	if (!vm_kern_free(
		allocation_a,
		KERNEL_VM_KERN_TEST_A_SIZE
	)) {
		goto cleanup;
	}

	allocation_a_active = false;

	if (!vm_kern_allocate(
		KERNEL_VM_KERN_TEST_A_SIZE,
		VMM_PROTECTION_READ_WRITE,
		&reused_allocation
	)) {
		goto cleanup;
	}

	reused_allocation_active = true;

	if (reused_allocation != allocation_a) {
		goto cleanup;
	}

	passed = true;

cleanup:
	if (
		allocation_a_active &&
		!vm_kern_free(
			allocation_a,
			KERNEL_VM_KERN_TEST_A_SIZE
		)
	) {
		cleanup_passed = false;
	}

	if (
		allocation_b_active &&
		!vm_kern_free(
			allocation_b,
			KERNEL_VM_KERN_TEST_B_SIZE
		)
	) {
		cleanup_passed = false;
	}

	if (
		reused_allocation_active &&
		!vm_kern_free(
			reused_allocation,
			KERNEL_VM_KERN_TEST_A_SIZE
		)
	) {
		cleanup_passed = false;
	}

	return passed && cleanup_passed;
}

static bool kern_test_small_heap(void)
{
	void *allocation_a = kmalloc(24U);
	void *allocation_b = kmalloc(200U);
	void *allocation_c = kcalloc(32U, 8U);

	if (
		allocation_a == 0 ||
		allocation_b == 0 ||
		allocation_c == 0
	) {
		return false;
	}

	kputs("heap: allocation A: ");
	kputhex64((uint64_t)allocation_a);
	kputc('\n');

	kputs("heap: allocation B: ");
	kputhex64((uint64_t)allocation_b);
	kputc('\n');

	kputs("heap: allocation C: ");
	kputhex64((uint64_t)allocation_c);
	kputc('\n');

	if (!kfree(allocation_b)) {
		return false;
	}

	void *reused_allocation = kmalloc(128U);

	if (
		reused_allocation == 0 ||
		reused_allocation != allocation_b
	) {
		return false;
	}

	if (
		!kfree(allocation_a) ||
		!kfree(allocation_c) ||
		!kfree(reused_allocation)
	) {
		return false;
	}

	return true;
}

static bool kern_test_large_heap(void)
{
	void *large_allocation = kmalloc(KERNEL_HEAP_LARGE_TEST_SIZE);

	if (
		large_allocation == 0 ||
		!vm_kern_contains(large_allocation)
	) {
		return false;
	}

	uint8_t *large_bytes = (uint8_t *)large_allocation;

	uint64_t page_count =
		(KERNEL_HEAP_LARGE_TEST_SIZE +
		PMM_PAGE_SIZE - 1ULL) /
		PMM_PAGE_SIZE;

	for (
		uint64_t page = 0ULL;
		page < page_count;
		page++
	) {
		uint64_t first_offset = page * PMM_PAGE_SIZE;

		uint64_t last_offset = first_offset + PMM_PAGE_SIZE - 1ULL;

		if (
			first_offset <
			KERNEL_HEAP_LARGE_TEST_SIZE
		) {
			large_bytes[first_offset] =
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page
				);
		}

		if (
			last_offset <
			KERNEL_HEAP_LARGE_TEST_SIZE
		) {
			large_bytes[last_offset] =
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page +
					16ULL
				);
		}
	}

	for (
		uint64_t page = 0ULL;
		page < page_count;
		page++
	) {
		uint64_t first_offset = page * PMM_PAGE_SIZE;

		uint64_t last_offset = first_offset + PMM_PAGE_SIZE - 1ULL;

		if (
			first_offset <
				KERNEL_HEAP_LARGE_TEST_SIZE &&
			large_bytes[first_offset] !=
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page
				)
		) {
			return false;
		}

		if (
			last_offset <
				KERNEL_HEAP_LARGE_TEST_SIZE &&
			large_bytes[last_offset] !=
				(uint8_t)(
					KERNEL_HEAP_LARGE_TEST_VALUE +
					page +
					16ULL
				)
		) {
			return false;
		}
	}

	kputs("heap: large allocation: ");
	kputhex64((uint64_t)large_allocation);
	kputc('\n');

	heap_dump();

	if (!kfree(large_allocation)) {
		return false;
	}

	return !kfree(large_allocation);
}

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
	kputs("kern_init: higher-half PC: ");
	kputhex64(arm64_read_program_counter());
	kputc('\n');

	kputs("kern_init: higher-half SP: ");
	kputhex64(arm64_read_stack_pointer());
	kputc('\n');

	kputs("kern_init: higher-half VBAR_EL1: ");
	kputhex64(arm64_read_vector_base());
	kputc('\n');

	kputs("kern_init: platform state: ");
	kputhex64((uint64_t)platform_get());
	kputc('\n');

	kputs("kern_init: Device Tree state: ");
	kputhex64((uint64_t)dtb_get_boot());
	kputc('\n');
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
static __attribute__((noreturn, noinline))
void kern_init_higher_half(void)
{
	if (!kern_validate_higher_half_state()) {
		kern_fail("kern_init: higher-half state validation failed");
	}

	kputln("kern_init: higher-half transition complete");
	kern_dump_higher_half_state();
	nxu_boot_log_higher_half();

	if (!vmm_validate_linked_kernel_layout()) {
		kern_fail("vmm: higher-half linked layout validation failed");
	}

	kputln("vmm: higher-half linked layout validated");

	if (!kern_test_linked_kernel_pointers()) {
		kern_fail("vmm: static higher-half pointer validation failed");
	}

	kputln("vmm: static kernel pointers are higher-half linked");

	/* Permanent TTBR1 runtime state. */

	if (!kern_prepare_higher_half_runtime()) {
		kern_fail("kern_init: higher-half pointer rebasing failed");
	}

	kputln("kern_init: kernel pointers rebased to TTBR1");

	if (!vmm_disable_ttbr0()) {
		kern_fail("vmm: TTBR0 shutdown failed");
	}

	kputln("vmm: TTBR0 disabled");

	if (!kern_validate_ttbr0_shutdown()) {
		kern_fail("vmm: TTBR0 shutdown validation failed");
	}

	kputln("vmm: lower address translation rejected");

	/* Kernel virtual arena. */

	kputln("vm_kern: initializing kernel virtual arena");

	if (!vm_kern_init()) {
		kern_fail("vm_kern: initialization failed");
	}

	kputln("vm_kern: kernel virtual arena initialized");
	kputln("vm_kern: testing virtual allocations");

	if (!kern_test_vm_kern()) {
		kern_fail("vm_kern: allocation test failed");
	}

	kputln("vm_kern: allocation test passed");
	vm_kern_dump();

	/* Kernel heap. */

	kputln("heap: initializing kernel heap");

	if (!heap_init()) {
		kern_fail("heap_init: initialization failed");
	}

	kputln("heap_init: kernel heap initialized");

	if (!kern_test_small_heap()) {
		kern_fail("kern_test_small_heap: allocation, reuse or free test failed");
	}

	kputln("kern_test_small_heap: allocation, reuse and free tests passed");
	heap_dump();

	kputln("kern_test_large_heap: testing multi-page allocation");

	if (!kern_test_large_heap()) {
		kern_fail("kern_test_large_heap: multi-page allocation/free test failed");
	}

	kputln("heap: multi-page allocation/free test passed");
	heap_dump();

	/*
	 * Interrupt controller and boot-policy-controlled device probing.
	 *
	 * This runs immediately after the heap, ahead of IPC, process, VFS,
	 * thread and scheduler bring-up, so the boot splash can appear the
	 * moment a display is available instead of after nearly the entire
	 * kernel init sequence -- the rest of boot then proceeds with the
	 * splash already on screen and the console log scrolling behind it.
	 */

	gic_init();
	kputln("kern_init: GICv3 initialized");
	kprintf("IOPlatformCPU: cpu0 MPIDR 0x%llx, GICv3 redistributor 0, enabled\n", (unsigned long long)arm64_read_mpidr_el1());
	(void)ioreg_add(ioreg_family_platform(), "IOPlatformCPU", "IOPlatformCPU");

	if (!irq_init()) kern_fail("irq: initialization failed");
	kputln("IOInterruptController: GICv3 vector table online");
	(void)ioreg_add(ioreg_family_platform(), "IOInterruptController", "IOInterruptController");

	if (!input_init()) kern_fail("input: core initialization failed");
	kputln("IOHIDSystem: input core online");

	if (!keyboard_init()) kern_fail("keyboard: initialization failed");
	kputln("IOHIDSystem: keyboard driver matched");

	boot_mode_init();
	kputln("IOBootMode: recovery key watch armed");
	(void)ioreg_add(ioreg_family_platform(), "IOBootMode", "IOBootMode");

	if (!mouse_init()) kern_fail("VirtIOMouseFamily: initialization failed");
	kputln("IOHIDSystem: mouse driver matched");

	if (!block_device_init()) kern_fail("VirtIOBlockFamily: initialization failed");
	kputln("IOStorageFamily: block device core online");

	if (!display_init()) kern_fail("IODisplayFamily: core initialization failed");
	kputln("IOGraphicsFamily: display core online");

	if (!rtc_init()) kern_fail("rtc_init: initialization failed");
	kputln("IORTC: real-time clock online");
	(void)ioreg_add(ioreg_family_platform(), "IORTC", "IORTC");

	nxu_boot_log_driver_handoff();
	kputln("VirtIOFamily: probing MMIO transports");

	const platform_t *runtime_platform = platform_get();
	bool input_enabled = !boot_args_component_disabled(BOOT_COMPONENT_INPUT);
	bool block_enabled = !boot_args_component_disabled(BOOT_COMPONENT_BLOCK);
	bool gpu_enabled = !boot_args_component_disabled(BOOT_COMPONENT_GPU);
	virtio_probe_policy_t virtio_policy = {
		.input = input_enabled,
		.block = block_enabled,
		.gpu = gpu_enabled,
		.on_gpu_ready = gpu_enabled ? kern_boot_splash_show_if_needed : 0
	};

	if (runtime_platform == 0 || !virtio_init(runtime_platform, &virtio_policy)) {
		kern_fail("VirtIOFamily: initialization failed");
	}

	if (input_enabled && !keyboard_is_present()) kern_fail("keyboard: VirtIO keyboard not found");
	if (input_enabled && !mouse_is_present()) kern_fail("mouse: VirtIO mouse not found");
	if (block_enabled && block_device_count() == 0U) kern_fail("VirtIOBlockFamily: VirtIO block device not found");

	if (!input_enabled) kputln("input: disabled by boot-args");
	if (!block_enabled) kputln("VirtIOBlockFamily: disabled by boot-args");
	if (!gpu_enabled) kputln("NXUDisplayDriverFamily: VirtIO GPU disabled by boot-args");

	if (gpu_enabled && (display_primary() == 0 || virtio_gpu_count() == 0U)) {
		kputln("NXUDisplayDriverFamily: VirtIO GPU not found");

		if (ramfb_console_init(runtime_platform)) {
			kputln("NXUDisplayDriverFamily: emergency ramfb console active");
		} else {
			kputln("NXUDisplayDriverFamily: emergency ramfb console unavailable");
		}
	}

	/*
	 * Covers the ramfb-fallback path above: the splash already showed via
	 * the on_gpu_ready callback if a real GPU attached, so this is a no-op
	 * in the common case.
	 */
	kern_boot_splash_show_if_needed();

	/*
	 * Recovery startup selection is deliberately independent from UIService.framework.
	 * Shift+R is sampled against the raw input queue while this kernel-owned
	 * splash is visible, before either sevOS or triageOS userspace starts.
	 */
	display_device_t *boot_display = display_primary();
	if (boot_display != 0) {
		uint64_t hold_ms = input_enabled ? 3000ULL : 500ULL;
		if (!boot_splash_wait(hold_ms, input_enabled ? kern_boot_splash_service : 0)) kern_fail("boot_splash_wait: hold failed");
	}

	/* NXPC kernel message transport. */

	ipc_init();

	if (!ipc_self_test()) {
		kern_fail(NXPC_LOG_PREFIX "kernel transport self-test failed");
	}

	if (!ipc_space_self_test()) {
		kern_fail(NXPC_LOG_PREFIX "per-process name table self-test failed");
	}

	kputln(NXPC_LOG_PREFIX "kernel transport ready");

	if (!vm_shm_self_test()) {
		kern_fail("vm_shm: self-test failed");
	}

	if (!vm_map_self_test()) {
		kern_fail("vm_map: self-test failed");
	}

	/* Process and thread core. */

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

	/* Virtual filesystem bootstrap. */

	if (!kern_test_crc32c()) kern_fail("NXU_CRC32C_Checksum: self-test failed");
	kputln("NXU_CRC32C_Checksum: self-test passed");
	kputln("IOVirtualFSDriver initializing virtual filesystem");

	if (!vfs_init()) kern_fail("IOVirtualFSDriver initialization failed");
	if (!ramfs_register()) kern_fail("IOVirtualFSDriver ramfs registration failed");
	if (!ext4_register()) kern_fail("IOVirtualFSDriver ext4 registration failed");

	vfs_status_t mount_status = vfs_mount("ramfs", 0, "/");
	if (mount_status != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver root ramfs mount failed");

	kputln("IOVirtualFSDriver mounted ramfs at /");

	if (vfs_mkdir("/disk") != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver: /disk mountpoint creation failed");

	if (!kern_test_vfs()) kern_fail("IOVirtualFSDriver pathname/file-descriptor self-test failed");

	kputln("IOVirtualFSDriver pathname/file-descriptor self-test passed");
	vfs_dump();

	kputln("thread_bootstrap: initializing thread subsystem");

	if (!thread_bootstrap()) {
		kern_fail("thread_bootstrap: subsystem initialization failed");
	}

	kputln("sched_bootstrap: initializing processor scheduler");

	if (!sched_bootstrap(proc_task(proc_kernel()))) {
		kern_fail("sched_bootstrap: scheduler initialization failed");
	}

	if (!sched_run_queue_self_test()) {
		kern_fail("sched_run_queue_self_test: run queue self-test failed");
	}

	if (!sched_mlfq_self_test()) {
		kern_fail("sched_mlfq_self_test: MLFQ feedback self-test failed");
	}

	if (!sched_validate() || !thread_validate()) {
		kern_fail("sched_validate: bootstrap validation failed");
	}

	kputln("thread: thread subsystem initialized");
	kputln("sched_validate: fixed-priority run queue self-test passed");
	kputln("sched_validate: MLFQ feedback self-test passed");
	sched_dump();
	thread_dump();

	/* AArch64 scheduler context-switch validation. */

	kputln("kernel_thread_create: testing AArch64 context switching");

	g_sched_context_test_stage = 0U;
	thread_t context_test_thread;

	if (!kernel_thread_create(
		proc_task(proc_kernel()),
		kern_sched_context_test_continue,
		(void *)&g_sched_context_test_stage,
		&context_test_thread
	)) {
		kern_fail("kernel_thread_create: context-switch test thread creation failed");
	}

	if (!sched_thread_start(context_test_thread)) {
		kern_fail("sched_thread_start: context-switch test thread start failed");
	}

	if (!sched_yield()) {
		kern_fail("sched_yield: context-switch test dispatch failed");
	}

	if (
		g_sched_context_test_stage != 1U ||
		current_thread() != sched_bootstrap_thread()
	) {
		kern_fail("sched_bootstrap_thread: AArch64 context-switch test failed");
	}

	if (!sched_thread_terminate(context_test_thread)) {
		kern_fail("sched_thread_terminate: context-switch test termination failed");
	}

	if (!thread_reap(context_test_thread)) {
		kern_fail("thread_reap: context-switch test reap failed");
	}

	thread_deallocate(context_test_thread);

	if (!sched_validate() || !thread_validate()) {
		kern_fail("sched_validate: post-switch validation failed");
	}

	kputln("sched_validate: AArch64 context-switch test passed");

	/* EL0 exception-vector classification. */
	kputln("ARM64ExceptionHandler(): validating EL0 vector classification");

	if (!exception_validate_vector_classification()) {
		kern_fail("ARM64ExceptionHandler(): EL0 vector classification failed");
	}

	kputln("ARM64ExceptionHandler(): current EL synchronous vector classified");
	kputln("ARM64ExceptionHandler(): current EL IRQ vector classified");
	kputln("ARM64ExceptionHandler(): EL0 synchronous vector classified");
	kputln("ARM64ExceptionHandler(): EL0 IRQ vector classified");
	kputln("ARM64ExceptionHandler(): EL0 return state is recoverable");

#if defined(NXU_WINDOWSERVER_BOOT_TEST) && !defined(NXU_AQUA_BOOT_TEST)
	/* WindowServer-only bring-up remains available as a compositor smoke test. */
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

	if (!windowserver_bootstrap()) {
		kern_fail("panic: WindowServer bootstrap failed");
	}

	for (;;) {
		__asm__ volatile("wfe");
	}
#endif

#if defined(NXU_AQUA_BOOT_TEST)
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

	if (!windowserver_bootstrap()) {
		kern_fail("panic: WindowServer bootstrap failed");
	}
	if (!ui_service_bootstrap()) {
		kern_fail("panic: interactive session failed");
	}

	for (;;) {
		__asm__ volatile("wfe");
	}
#endif

#if defined(NXU_UI_SERVICE_BOOT_TEST)
	/* UIService requires WindowServer for surface submission. */
	if (boot_display != 0) ioreg_dump();
	if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");
	nxu_boot_log_ui_handoff();
	arm64_enable_irqs();

#if defined(NXU_WINDOWSERVER)
	if (!windowserver_bootstrap()) kern_fail("panic: WindowServer bootstrap failed");
#endif
	if (!ui_service_bootstrap()) kern_fail("panic: interactive session failed");

	for (;;) __asm__ volatile("wfe");
#endif

	virtio_dump();
	block_device_dump();
	display_dump();

	if (block_enabled) {
		if (!kern_test_block_device()) kern_fail("VirtIOBlockFamily: sector read test failed");

		kputln("IOFilesystemFamily: mounting disk0 at /disk");
		vfs_status_t ext4_mount_status = vfs_mount("ext4", block_device_first(), "/disk");
		if (ext4_mount_status != VFS_STATUS_OK) {
			kputs("IOFilesystemFamily: mount failed: ");
			kputln(vfs_status_name(ext4_mount_status));
			kern_fail("IOFilesystemFamily: mount failed");
		}

		ext4_dump();
		if (boot_mode_is_triage_os()) {
			kputln("IOFilesystemFamily: recovery boot; writable filesystem self-test skipped");
		} else {
			if (!kern_test_ext4()) kern_fail("IOFilesystemFamily: writable filesystem self-test failed");
			kputln("IOFilesystemFamily: writable filesystem self-test passed");
			if (vfs_sync_all() != VFS_STATUS_OK) kern_fail("IOVirtualFSDriver filesystem sync failed");
			kputln("IOVirtualFSDriver mounted filesystems synchronized");
		}

		ext4_dump();
		vfs_dump();

#if defined(NXU_JOURNAL_CRASH_TEST)
		kern_run_jbd2_crash_test();
#endif

#if defined(NXU_IPC_PROCESS_TEST)
		if (!ipc_process_test()) kern_fail("ipc_process_test: failed");
		kputln("ipc_process_test: passed; halting (test build, no bootd)");
		for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_THREAD_PROCESS_TEST)
		if (!thread_process_test()) kern_fail("thread_process_test: failed");
		kputln("thread_process_test: passed; halting (test build, no bootd)");
		for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_SOCKET_PROCESS_TEST)
		if (!socket_process_test()) kern_fail("socket_process_test: failed");
		kputln("socket_process_test: passed; halting (test build, no bootd)");
		for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_XAMETHYST_PROCESS_TEST)
		if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");

		if (!xamethyst_process_test()) kern_fail("amethyst_test_process: failed");
		kputln("amethyst_test_process: passed; halting (test build)");
		for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_WINDOWSERVER_PROCESS_TEST)
		/*
		 * Hand the framebuffer over to WindowServer completely, exactly
		 * like the normal boot path does right before spawning bootd
		 * (see the boot_splash_finish() call a few lines below this
		 * block): once unregistered, no further kprintf/kputln output
		 * touches the graphical console, leaving WindowServer's own
		 * present() calls as the only thing drawing to the screen.
		 */
		if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");

		if (!windowserver_process_test()) kern_fail("windowserver_process_test: failed");
		kputln("windowserver_process_test: passed; halting (test build)");
		for (;;) __asm__ volatile("wfe");
#endif

#if defined(NXU_ABOUT_SEVOS_PROCESS_TEST)
		if (boot_display != 0 && !boot_splash_finish()) kern_fail("boot_splash_finish: completion failed");

		if (!about_sevos_process_test()) kern_fail("about_sevos_process_test: failed");
		kputln("about_sevos_process_test: passed; halting (test build)");
		for (;;) __asm__ volatile("wfe");
#endif

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

	if (input_enabled) kputln("input: VirtIO keyboard and mouse initialized");

	gic_enable_ppi(PHYSICAL_TIMER_INTID, 0x80U);
	kputln("kern_init: physical timer PPI enabled");

	timer_start_periodic(KERNEL_TIMER_HZ);

	kprintf("kern_init: periodic timer started at %u Hz\n", KERNEL_TIMER_HZ);

	arm64_enable_irqs();
	kputln("kern_init: IRQs enabled");

	if (g_boot_process != 0) {
		kprintf("sched: dispatching %s PID %u\n", boot_mode_is_triage_os() ? "triageOS" : "bootd", g_boot_process->p_ident.pid);
		kputln(boot_mode_is_triage_os() ? "kern_init: recovery userspace active" : "kern_init: root userspace services active");

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
	kputln("pmm: initializing physical memory manager");

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

	kputln("pmm: allocation/free test passed");

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

	kputln("vmm: stage 1 MMU enabled");

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

	kputln("vmm: stack identity translation passed");
	kputln("vmm: UART identity translation passed");

	/* CPU caches and live table updates. */
	kputln("cache_init: initializing CPU caches");

	if (!cache_init()) {
		kern_fail("cache_init: initialization failed");
	}

	kputln("cache_init: CPU caches initialized");

	cache_dump();

	/* TTBR1 kernel alias and full direct map. */
	kputln("vmm: installing TTBR1 higher-half alias");

	if (!vmm_init_higher_half_alias()) {
		kern_fail("vmm: TTBR1 higher-half initialization failed");
	}

	kputln("vmm: TTBR1 higher-half alias installed");

	vmm_dump_higher_half();

	/* The live mapping test now targets a TTBR1 virtual address */
	kputln("vmm: testing live page mappings");

	if (!kern_test_live_vmm()) {
		kern_fail("vmm: live page mapping test failed");
	}

	kputln("vmm: live page mapping test passed");

	kputln("vmm: testing higher-half execution");

	if (!kern_test_higher_half_alias()) {
		kern_fail("vmm: higher-half execution test failed");
	}

	kputln("vmm: higher-half execution test passed");

	kputln("vmm: building higher-half direct map");

	if (!vmm_map_higher_half_direct_map(
		platform
	)) {
		kern_fail("vmm: higher-half direct map failed");
	}

	kputln("vmm: higher-half direct map installed");

	if (!vmm_validate_higher_half_direct_map(
		platform
	)) {
		kern_fail("vmm: higher-half direct map validation failed");
	}

	kputln("vmm: higher-half direct map validated");

	if (!kern_test_higher_half_ram_alias()) {
		kern_fail("vmm: higher-half RAM alias test failed");
	}

	kputln("vmm: higher-half RAM alias test passed");

	vmm_dump_higher_half();

	/* This call never returns to the lower TTBR0 alias. */
	kern_enter_higher_half();
}
