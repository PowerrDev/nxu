#include <kern/console/bootlog.h>

#include <kern/arm64/system.h>
#include <kern/arm64/timer.h>
#include <kern/console/console.h>
#include <kern/logging/version.h>
#include <vm/vmm.h>

#include <stdint.h>

/*
 * Keep the early boot log factual. These lines describe values read from the
 * machine or completed transitions; policy explanations belong in comments.
 */
void nxu_boot_log_early(const void *dtb_address, uint64_t current_el_raw, uint32_t current_el)
{
	kputln(NXU_KERNEL_VERSION);
	kprintf("NXU: %s, clang %u.%u.%u\n", NXU_BUILD, __clang_major__, __clang_minor__, __clang_patchlevel__);
	kprintf("cpu0: EL%u (CurrentEL 0x%llx), MIDR 0x%llx, MPIDR 0x%llx\n", current_el, (unsigned long long)current_el_raw, (unsigned long long)arm64_read_midr_el1(), (unsigned long long)arm64_read_mpidr_el1());
	kprintf("timer: CNTFRQ_EL0 %llu Hz\n", (unsigned long long)timer_get_frequency());
	kprintf("PE: boot FDT at %p\n", (void *)dtb_address);
}

/*
 * Summarize only resources that were actually discovered. Individual VirtIO
 * devices are reported later by their class drivers when they attach.
 */
void nxu_boot_log_platform(const dtb_t *dtb, const platform_t *platform)
{
	if (dtb == 0 || platform == 0) return;

	kprintf("PE: FDT v%u, %u bytes, %u RAM region(s), %u VirtIO-MMIO slot(s)\n", dtb->version, dtb->total_size, platform->memory_region_count, platform->virtio_mmio_count);

	for (uint32_t index = 0U; index < platform->memory_region_count; index++) {
		const platform_region_t *region = &platform->memory_regions[index];
		kprintf("PE: RAM[%u] 0x%llx-0x%llx (%llu MiB)\n", index, (unsigned long long)region->base, (unsigned long long)(region->base + region->size), (unsigned long long)(region->size / (1024ULL * 1024ULL)));
	}

	kprintf("PE: UART 0x%llx, GICD 0x%llx, GICR 0x%llx\n", (unsigned long long)platform->uart.base, (unsigned long long)platform->gic_distributor.base, (unsigned long long)platform->gic_redistributor.base);
}

/*
 * Record the single architectural transition callers care about during VM
 * bring-up. Detailed page-table validation already logs its own results.
 */
void nxu_boot_log_higher_half(void)
{
	kprintf("kernel_bootstrap: higher-half kernel active at 0x%llx\n", (unsigned long long)VMM_HIGHER_HALF_BASE);
}

/*
 * Driver startup logs the point at which the transport scan can begin. Device
 * classes report their own successful or failed attachments.
 */
void nxu_boot_log_driver_handoff(void)
{
	kputln("IODriverFamily: GICv3 and IRQ routing online; probing devices");
}

void nxu_boot_log_ui_handoff(void)
{
	kputln("kernel_bootstrap: device discovery complete");
}
