#include <vm/address_space.h>
#include <vm/vmm_internal.h>

#include <kern/console/console.h>
#include <kern/ipi.h>
#include <kern/lock.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/machine/timer.h>
#include <kern/machine/vm_param.h>
#include <kern/sched_prism/processor.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VM_USER_NULL_GUARD_SIZE VMM_L3_SIZE

/* Every level of the 39-bit walk holds 512 descriptors. */
#define VM_TABLE_ENTRIES 512U

/*
 * AP[2:1] values used by EL0 mappings:
 *
 * 0b01: EL1 read-write, EL0 read-write
 * 0b11: EL1 read-only,  EL0 read-only
 */
#define VM_USER_AP_READ_WRITE (1ULL << 6U)
#define VM_USER_AP_READ_ONLY (3ULL << 6U)

/* PAR_EL1.F is bit zero after an AT instruction. */
#define VM_PAR_FAULT (1ULL << 0U)

/*
 * Which space a CPU runs with is that CPU's own state (struct processor's
 * active_space, see kern/sched_prism/processor.h): nothing here is global, so
 * two CPUs can run threads of different processes, or of the same one, at the
 * same time. The accessors below are only meaningful while the caller cannot
 * change CPU (interrupts masked, or code that never migrates: kernel code runs
 * to completion or to an explicit sleep, and the scheduler switches spaces with
 * interrupts masked).
 *
 * There are no ASIDs. Switching TTBR0 invalidates the switching CPU's TLB
 * (vmalle1, local), and every change to a live page-table entry invalidates by
 * address, inner-shareable, on all CPUs (vmm_invalidate_page). That is correct
 * with or without ASIDs and never has to reason about ASID reuse or rollover;
 * the cost is a full local flush per address-space switch, which is what NXU has
 * always paid.
 *
 * Lock order for the paths below: space->lock -> (pmm, via vmm_root_get_l3_entry).
 */
static vm_address_space_t *vm_cpu_space(void)
{
	processor_t processor = current_processor();

	return processor == 0 ? 0 : processor->active_space;
}

static bool vm_user_descriptor(
	uint64_t physical_address,
	vm_user_protection_t protection,
	uint64_t *descriptor
)
{
	if (
		descriptor == 0 ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t attributes =
		VMM_DESC_ATTR(VMM_MAIR_NORMAL_INDEX) |
		VMM_DESC_SH_INNER |
		VMM_DESC_AF |
		VMM_DESC_PXN;

	switch (protection) {
	case VM_USER_PROTECTION_READ_WRITE:
		attributes |=
			VM_USER_AP_READ_WRITE |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_ONLY:
		attributes |=
			VM_USER_AP_READ_ONLY |
			VMM_DESC_UXN;
		break;

	case VM_USER_PROTECTION_READ_EXECUTE:
		/*
		 * EL1 may read the page but cannot execute it. EL0 may read and
		 * execute it, while writes are rejected by AP[2:1] = 0b11.
		 */
		attributes |= VM_USER_AP_READ_ONLY;
		break;

	default:
		return false;
	}

	*descriptor =
		(physical_address & VMM_DESC_ADDRESS_MASK) |
		attributes |
		VMM_DESC_TABLE_PAGE;

	return true;
}

static bool vm_user_descriptor_protection(
	uint64_t descriptor,
	vm_user_protection_t *protection
)
{
	if (protection == 0) {
		return false;
	}

	uint64_t access_permissions = descriptor & VMM_DESC_AP_MASK;

	bool privileged_execute_never = (descriptor & VMM_DESC_PXN) != 0ULL;

	bool user_execute_never = (descriptor & VMM_DESC_UXN) != 0ULL;

	if (!privileged_execute_never) {
		return false;
	}

	if (
		access_permissions == VM_USER_AP_READ_WRITE &&
		user_execute_never
	) {
		*protection = VM_USER_PROTECTION_READ_WRITE;
		return true;
	}

	if (access_permissions == VM_USER_AP_READ_ONLY) {
		*protection = user_execute_never
			? VM_USER_PROTECTION_READ_ONLY
			: VM_USER_PROTECTION_READ_EXECUTE;

		return true;
	}

	return false;
}

static bool vm_address_space_translate(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	bool write,
	uint64_t *physical_address
)
{
	if (
		!vm_address_space_is_active(space) ||
		physical_address == 0
	) {
		return false;
	}

	uint64_t par;

	if (write) {
		__asm__ volatile(
			"at s1e0w, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	} else {
		__asm__ volatile(
			"at s1e0r, %1\n"
			"isb\n"
			"mrs %0, par_el1\n"
			: "=r"(par)
			: "r"(virtual_address)
			: "memory"
		);
	}

	if ((par & VM_PAR_FAULT) != 0ULL) {
		return false;
	}

	*physical_address =
		(par & VMM_DESC_ADDRESS_MASK) |
		(virtual_address & (VMM_L3_SIZE - 1ULL));

	return true;
}

const vm_address_space_t *vm_address_space_current(void)
{
	return vm_cpu_space();
}

bool vm_address_space_create(vm_address_space_t *space)
{
	if (
		space == 0 ||
		!g_vmm.enabled ||
		!g_vmm.higher_half_enabled ||
		!g_vmm.higher_half_direct_map_enabled ||
		!g_vmm.kernel_pointers_rebased ||
		space->root != 0 ||
		space->root_physical != 0ULL ||
		space->table_count != 0ULL ||
		space->active
	) {
		return false;
	}

	memset(space, 0, sizeof(*space));

	if (!vmm_allocate_table(
		&space->root,
		&space->root_physical,
		&space->table_count
	)) {
		memset(space, 0, sizeof(*space));
		return false;
	}

	return true;
}

/*
 * Take this CPU out of a space's membership. Called once the CPU has installed
 * another root (or disabled TTBR0) and flushed its TLB: from then on it can no
 * longer walk the space's tables.
 */
static void vm_space_leave(vm_address_space_t *space, processor_t cpu)
{
	cpuset_remove_atomic(&space->active_cpus, cpu->cpu_id);
	__atomic_store_n(&space->active, !cpuset_empty(&space->active_cpus), __ATOMIC_RELEASE);
}

/* Stop this CPU's TTBR0 walks and drop what it cached (local: nobody else's TTBR0 changed). */
static void vm_ttbr0_disable_local(void)
{
	uint64_t tcr = vmm_read_tcr() | VMM_TCR_EPD0;

	__asm__ volatile(
		"msr ttbr0_el1, xzr\n"
		"msr tcr_el1, %0\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb nsh\n"
		"isb\n"
		:
		: "r"(tcr)
		: "memory"
	);
}

/*
 * vm_address_space_activate
 *
 * Load `space` into this CPU's TTBR0. Only this CPU's translation state
 * changes; other CPUs keep running whatever they run.
 *
 * Ordering: the CPU joins the space's membership set and publishes that with a
 * full barrier BEFORE it installs the root, so anyone who checks the set after
 * the CPU could have started to walk the tables sees it (this is the store half
 * of the pairing with the load in vm_address_space_quiesce). It leaves the
 * previous space only afterwards.
 */
bool vm_address_space_activate(vm_address_space_t *space)
{
	if (
		space == 0 ||
		space->root == 0 ||
		space->root_physical == 0ULL ||
		space->table_count == 0ULL ||
		!g_vmm.enabled ||
		!g_vmm.kernel_pointers_rebased
	) {
		return false;
	}

	uint64_t irq_state = ml_irq_save();
	processor_t cpu = current_processor();
	vm_address_space_t *previous = cpu->active_space;

	if (previous == space) {
		ml_irq_restore(irq_state);
		return true;
	}

	cpuset_add_atomic(&space->active_cpus, cpu->cpu_id);
	__atomic_store_n(&space->active, true, __ATOMIC_RELEASE);
	__asm__ volatile("dsb ish" ::: "memory");

	uint64_t tcr = vmm_read_tcr() & ~VMM_TCR_EPD0;
	uint64_t ttbr0 = space->root_physical & VMM_DESC_ADDRESS_MASK;

	/*
	 * Install the new root, allow TTBR0 walks, and remove translations of the
	 * previous lower address space before any user mapping can be observed. The
	 * flush is this CPU's own: only its TTBR0 changed, so only it can hold the
	 * previous space's translations that must go. What other CPUs hold of this
	 * space is dealt with when its tables change (every unmap and permission
	 * change invalidates by address on all CPUs).
	 */
	__asm__ volatile(
		"dsb ishst\n"
		"msr ttbr0_el1, %0\n"
		"msr tcr_el1, %1\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb nsh\n"
		"isb\n"
		:
		: "r"(ttbr0), "r"(tcr)
		: "memory"
	);

	cpu->active_space = space;

	if (previous != 0) vm_space_leave(previous, cpu);

	ml_irq_restore(irq_state);
	return true;
}

bool vm_address_space_deactivate(void)
{
	if (!g_vmm.enabled || !g_vmm.kernel_pointers_rebased) return false;

	uint64_t irq_state = ml_irq_save();
	processor_t cpu = current_processor();
	vm_address_space_t *previous = cpu->active_space;

	if (previous == 0) {
		/* Nothing loaded. The boot CPU's boot-time identity map is torn down once, here or in kern_init. */
		bool ok = true;

		if (cpu->cpu_id == 0U && !g_vmm.ttbr0_disabled) ok = vmm_disable_ttbr0();

		ml_irq_restore(irq_state);
		return ok;
	}

	vm_ttbr0_disable_local();
	cpu->active_space = 0;
	vm_space_leave(previous, cpu);

	ml_irq_restore(irq_state);
	return true;
}

/*
 * vm_address_space_quiesce
 *
 * Wait until no CPU has `space` in TTBR0 (its members set is empty), so its page
 * tables and pages can be freed. The caller has already made sure no thread of the
 * space will run again (every thread terminated); the CPUs still in the set are
 * finishing their last switch away from it, or are running a terminated thread up to
 * its next exception, and are pushed with a reschedule interrupt to get there
 * promptly. This CPU leaves the space itself if it is in it. Holds no lock, and
 * waits with interrupts as they were: nothing it waits for needs this CPU.
 *
 * Pairing: the load of the set here is preceded by a full barrier, and a CPU adds
 * itself to the set and executes a full barrier before it can walk the tables, so
 * either this sees the CPU or the CPU sees every change made to the tables so far.
 *
 * Returns false (without freeing anything: the caller must leak rather than free)
 * if the set has not emptied after about two seconds.
 */
bool vm_address_space_quiesce(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return false;

	processor_t self = current_processor();

	if (self->active_space == space) (void)vm_address_space_deactivate();

	uint64_t deadline = timer_get_microseconds() + 2000000ULL;
	uint64_t next_kick = 0ULL;

	for (;;) {
		__asm__ volatile("dsb ish" ::: "memory");

		if (cpuset_empty(&space->active_cpus)) return true;

		uint64_t now = timer_get_microseconds();

		if (now > deadline) return false;

		if (now >= next_kick) {
			next_kick = now + 500ULL;

			for (uint32_t cpu = 0U; cpu < NXU_MAX_CPUS; cpu++) {
				if (cpu != self->cpu_id && cpuset_contains_atomic(&space->active_cpus, cpu)) {
					ipi_send(cpu, IPI_RESCHEDULE);
				}
			}
		}

		cpu_relax();
	}
}

/*
 * Install one leaf descriptor. The caller holds space->lock. Fails if the slot
 * is already mapped (existing mappings are never replaced implicitly). The new
 * mapping needs no TLB maintenance (an unmapped page leaves no TLB entry), only
 * the barrier that makes the descriptor visible to the table walkers of every
 * CPU before the mapping is used.
 */
static bool vm_space_install_locked(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t descriptor
)
{
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		true,
		&space->table_count,
		&entry
	)) {
		return false;
	}

	if ((*entry & VMM_DESC_VALID) != 0ULL) return false;

	*entry = descriptor;
	vmm_publish_new_mapping();
	return true;
}

bool vm_address_space_map_page_locked(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address) ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t descriptor;

	if (!vm_user_descriptor(physical_address, protection, &descriptor)) return false;

	return vm_space_install_locked(space, virtual_address, descriptor);
}

bool vm_address_space_map_page(
	vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t physical_address,
	vm_user_protection_t protection
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address) ||
		!vmm_physical_page_valid(physical_address)
	) {
		return false;
	}

	uint64_t descriptor;

	if (!vm_user_descriptor(
		physical_address,
		protection,
		&descriptor
	)) {
		return false;
	}

	nxu_spin_lock(&space->lock);

	bool installed = vm_space_install_locked(space, virtual_address, descriptor);

	nxu_spin_unlock(&space->lock);

	return installed;
}

bool vm_address_space_unmap_page(
	vm_address_space_t *space,
	uint64_t virtual_address
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	nxu_spin_lock(&space->lock);

	uint64_t table_count = space->table_count;

	uint64_t *entry;

	/* create=false: this only ever clears an existing leaf entry, never
	 * allocates intermediate tables, so table_count is never modified here
	 * -- see vm_address_space_query_page for the same read-only usage. */
	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		false,
		&table_count,
		&entry
	)) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	if ((*entry & VMM_DESC_VALID) == 0ULL) {
		nxu_spin_unlock(&space->lock);
		return false;
	}

	/* Break the mapping before invalidating its cached translation, exactly
	 * like vmm_unmap_page does for the kernel TTBR1 side. */
	*entry = 0ULL;

	/*
	 * Inner-shareable, by address, whether or not the space is loaded anywhere:
	 * a thread of this process may be running on any CPU, and the physical page
	 * must not be reused while some CPU can still translate through the old
	 * entry. The lock is held across the invalidation so a concurrent fault
	 * cannot install a new mapping before the old one is gone everywhere.
	 */
	vmm_invalidate_page(virtual_address);

	nxu_spin_unlock(&space->lock);

	return true;
}

bool vm_address_space_query_page(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	vm_user_page_mapping_t *mapping
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		mapping == 0 ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	/* space is logically const to callers (this only inspects a mapping),
	 * but the lock still has to be taken to avoid reading a page-table
	 * entry that vm_address_space_map_page/unmap_page is concurrently
	 * publishing on another thread of the same task. */
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;

	nxu_spin_lock(lock);

	uint64_t table_count = space->table_count;
	uint64_t *entry;

	if (!vmm_root_get_l3_entry(
		space->root,
		virtual_address,
		false,
		&table_count,
		&entry
	)) {
		nxu_spin_unlock(lock);
		return false;
	}

	uint64_t descriptor = *entry;

	nxu_spin_unlock(lock);

	if (
		(descriptor & VMM_DESC_TYPE_MASK) !=
		VMM_DESC_TABLE_PAGE ||
		((descriptor & VMM_DESC_ATTR_MASK) >> 2U) !=
		VMM_MAIR_NORMAL_INDEX
	) {
		return false;
	}

	if (!vm_user_descriptor_protection(
		descriptor,
		&mapping->protection
	)) {
		return false;
	}

	mapping->physical_address = descriptor & VMM_DESC_ADDRESS_MASK;
	mapping->cow = (descriptor & VMM_DESC_SW_COW) != 0ULL;

	return true;
}

bool vm_address_space_translate_read(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		false,
		physical_address
	);
}

bool vm_address_space_translate_write(
	const vm_address_space_t *space,
	uint64_t virtual_address,
	uint64_t *physical_address
)
{
	return vm_address_space_translate(
		space,
		virtual_address,
		true,
		physical_address
	);
}

bool vm_address_space_is_active(const vm_address_space_t *space)
{
	/* "Is it the space this CPU walks?" -- the calling CPU's TTBR0, no one else's. */
	return space != 0 && space == vm_cpu_space();
}

uint64_t vm_address_space_root_physical(
	const vm_address_space_t *space
)
{
	return space == 0 ? 0ULL : space->root_physical;
}

uint64_t vm_address_space_table_count(
	const vm_address_space_t *space
)
{
	return space == 0 ? 0ULL : space->table_count;
}

/*
 * Page-table walking for fork and teardown. A user address space is a
 * three-level tree (L1 root, L2, L3), every table one page, every user
 * mapping a 4 KiB leaf.
 */

static bool vm_table_from_descriptor(uint64_t descriptor, uint64_t **table)
{
	if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) return false;

	return vmm_table_pointer_from_physical(descriptor & VMM_DESC_ADDRESS_MASK, table);
}

static bool vm_address_in_shm_window(uint64_t virtual_address)
{
	return
		virtual_address >= VM_SHM_BASE &&
		virtual_address < VM_SHM_BASE + VM_SHM_WINDOW_SIZE;
}

/*
 * Make every CPU forget every cached translation, after a bulk change to a
 * space's live entries (fork write-protecting the parent, teardown). Always
 * inner-shareable: threads of the space may be on any CPU, and a CPU that is not
 * running it holds nothing that matters and loses nothing it cannot refill.
 */
static void vm_address_space_flush_if_active(const vm_address_space_t *space)
{
	(void)space;

	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vmalle1is\n"
		"dsb ish\n"
		"isb\n"
		::: "memory"
	);
}

/*
 * vm_address_space_cow_break
 *
 * Give the caller a private, writable copy of a page fork left shared. Two
 * threads of the process (on two CPUs) can break the same page at once, and
 * another process holding the other reference can drop it at any moment, so
 * nothing read under the lock is trusted after it is released: the copy is made
 * outside the lock (it allocates) and installed only if the entry is still the
 * one that was copied from. If it is not, the copy is discarded and the page is
 * looked at again. If somebody else has already made it writable, that is success:
 * the faulting instruction can simply run again.
 */
bool vm_address_space_cow_break(
	vm_address_space_t *space,
	uint64_t virtual_address
)
{
	if (
		space == 0 ||
		space->root == 0 ||
		virtual_address < VM_USER_NULL_GUARD_SIZE ||
		!vmm_lower_page_valid(virtual_address)
	) {
		return false;
	}

	uint64_t page_va = virtual_address & ~(VMM_L3_SIZE - 1ULL);

	for (uint32_t attempt = 0U; attempt < 8U; attempt++) {
		nxu_spin_lock(&space->lock);

		uint64_t table_count = space->table_count;
		uint64_t *entry;

		if (!vmm_root_get_l3_entry(space->root, page_va, false, &table_count, &entry)) {
			nxu_spin_unlock(&space->lock);
			return false;
		}

		uint64_t descriptor = *entry;

		if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) {
			nxu_spin_unlock(&space->lock);
			return false;
		}

		if ((descriptor & VMM_DESC_SW_COW) == 0ULL) {
			/* Broken by another thread already (or never shared): writable now? */
			bool writable = (descriptor & VMM_DESC_AP_MASK) == VM_USER_AP_READ_WRITE;

			nxu_spin_unlock(&space->lock);
			return writable;
		}

		uint64_t old_physical = descriptor & VMM_DESC_ADDRESS_MASK;
		uint64_t writable_attributes =
			(descriptor & ~(VMM_DESC_AP_MASK | VMM_DESC_SW_COW)) |
			VM_USER_AP_READ_WRITE;

		/* The other owner is gone: nothing to copy, just take the page back. */
		if (pmm_page_refcount(old_physical) == 1U) {
			*entry = writable_attributes;
			vmm_invalidate_page(page_va);

			nxu_spin_unlock(&space->lock);
			return true;
		}

		nxu_spin_unlock(&space->lock);

		uint64_t new_physical;

		if (!pmm_allocate_page(&new_physical)) return false;

		uint64_t old_kernel;
		uint64_t new_kernel;

		if (
			!vmm_physical_to_higher_half(old_physical, &old_kernel) ||
			!vmm_physical_to_higher_half(new_physical, &new_kernel)
		) {
			(void)pmm_free_page(new_physical);
			return false;
		}

		memcpy((void *)new_kernel, (const void *)old_kernel, VMM_L3_SIZE);

		nxu_spin_lock(&space->lock);

		table_count = space->table_count;

		if (
			!vmm_root_get_l3_entry(space->root, page_va, false, &table_count, &entry) ||
			*entry != descriptor
		) {
			/* The page changed while it was being copied: throw the copy away and look again. */
			nxu_spin_unlock(&space->lock);
			(void)pmm_free_page(new_physical);
			continue;
		}

		*entry = (writable_attributes & ~VMM_DESC_ADDRESS_MASK) | (new_physical & VMM_DESC_ADDRESS_MASK);
		vmm_invalidate_page(page_va);

		nxu_spin_unlock(&space->lock);

		/* This space's mapping no longer refers to the shared page. */
		(void)pmm_free_page(old_physical);

		return true;
	}

	return false;
}

uint64_t vm_address_space_release_pages(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return 0ULL;

	uint64_t released = 0ULL;

	nxu_spin_lock(&space->lock);

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(space->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!vm_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; l3 < VM_TABLE_ENTRIES; l3++) {
				if ((level3[l3] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				(void)pmm_free_page(level3[l3] & VMM_DESC_ADDRESS_MASK);
				level3[l3] = 0ULL;
				released++;
			}
		}
	}

	vm_address_space_flush_if_active(space);

	nxu_spin_unlock(&space->lock);

	return released;
}

bool vm_address_space_destroy(vm_address_space_t *space)
{
	if (space == 0 || space->root == 0) return false;

	/*
	 * Never free tables a CPU can still walk: this CPU leaves the space, and
	 * every other CPU that has it loaded is waited for. If one never leaves
	 * (which would mean a thread of the space is still running), the tables are
	 * leaked, not freed: a leak is a bug report, a use-after-free is corruption.
	 */
	if (!vm_address_space_quiesce(space)) {
		kprintf("vm_address_space_destroy: a CPU still has the space loaded; its page tables are leaked\n");
		return false;
	}

	for (uint32_t l1 = 0U; l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(space->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; l2 < VM_TABLE_ENTRIES; l2++) {
			if ((level2[l2] & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

			(void)pmm_free_page(level2[l2] & VMM_DESC_ADDRESS_MASK);
		}

		(void)pmm_free_page(space->root[l1] & VMM_DESC_ADDRESS_MASK);
	}

	(void)pmm_free_page(space->root_physical);

	memset(space, 0, sizeof(*space));

	return true;
}

bool vm_address_space_fork(
	vm_address_space_t *parent,
	vm_address_space_t *child
)
{
	if (parent == 0 || child == 0 || parent->root == 0) return false;
	if (!vm_address_space_create(child)) return false;

	bool ok = true;
	bool parent_changed = false;

	nxu_spin_lock(&parent->lock);

	for (uint32_t l1 = 0U; ok && l1 < VM_TABLE_ENTRIES; l1++) {
		uint64_t *level2;
		if (!vm_table_from_descriptor(parent->root[l1], &level2)) continue;

		for (uint32_t l2 = 0U; ok && l2 < VM_TABLE_ENTRIES; l2++) {
			uint64_t *level3;
			if (!vm_table_from_descriptor(level2[l2], &level3)) continue;

			for (uint32_t l3 = 0U; ok && l3 < VM_TABLE_ENTRIES; l3++) {
				uint64_t descriptor = level3[l3];
				if ((descriptor & VMM_DESC_TYPE_MASK) != VMM_DESC_TABLE_PAGE) continue;

				uint64_t virtual_address =
					((uint64_t)l1 << VMM_L1_SHIFT) |
					((uint64_t)l2 << VMM_L2_SHIFT) |
					((uint64_t)l3 << VMM_L3_SHIFT);

				uint64_t physical = descriptor & VMM_DESC_ADDRESS_MASK;
				uint64_t child_descriptor = descriptor;

				/* Private writable memory becomes copy-on-write on both
				 * sides; everything else is shared exactly as it is. */
				if (
					!vm_address_in_shm_window(virtual_address) &&
					(descriptor & VMM_DESC_AP_MASK) == VM_USER_AP_READ_WRITE
				) {
					child_descriptor =
						(descriptor & ~VMM_DESC_AP_MASK) |
						VM_USER_AP_READ_ONLY |
						VMM_DESC_SW_COW;
				}

				if (!pmm_page_retain(physical)) {
					ok = false;
					break;
				}

				uint64_t *entry;

				if (!vmm_root_get_l3_entry(
					child->root,
					virtual_address,
					true,
					&child->table_count,
					&entry
				)) {
					(void)pmm_free_page(physical);
					ok = false;
					break;
				}

				*entry = child_descriptor;

				if (child_descriptor != descriptor) {
					level3[l3] = child_descriptor;
					parent_changed = true;
				}
			}
		}
	}

	if (parent_changed) vm_address_space_flush_if_active(parent);

	nxu_spin_unlock(&parent->lock);

	if (!ok) {
		(void)vm_address_space_release_pages(child);
		(void)vm_address_space_destroy(child);
		return false;
	}

	return true;
}
