/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vm/address_space.c
 *
 * Architecture-neutral user address space: ownership, reference counting, COW
 * policy and bookkeeping. Every translation-table operation is delegated to
 * the per-arch pmap backend (kern/vm/pmap.h); this file never touches a
 * page-table format or a pmap_t field.
 *
 * Which space a CPU runs with is that CPU's own state (struct processor's
 * active_space): nothing here is global, so two CPUs can run threads of
 * different processes, or of the same one, at the same time. The accessors
 * below are only meaningful while the caller cannot change CPU (interrupts
 * masked, or code that never migrates).
 *
 * active_cpus names every CPU that may have this space loaded as its
 * translation root; it is maintained with release/acquire ordering around
 * activation and is what vm_address_space_quiesce() waits on before pages or
 * tables may be freed (see the synchronization section of
 * doc/vm/unified-pmap.md).
 *
 * Lock order: space->lock -> (pmm / heap, reached through the backend).
 */

#include <vm/address_space.h>
#include <kern/vm/pmap.h>
#include <vm/vmm_internal.h> /* g_vmm + vmm_disable_ttbr0: boot identity-map teardown only */

#include <kern/console/console.h>
#include <kern/ipi.h>
#include <kern/lock.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>
#include <kern/machine/timer.h>
#include <kern/machine/vm_param.h>
#include <kern/memory/heap.h>
#include <kern/sched_prism/processor.h>
#include <vm/pmm.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool vm_address_in_shm_window(uint64_t address);

static vm_address_space_t *
vm_cpu_space(void)
{
	processor_t processor = current_processor();

	return processor == 0 ? 0 : processor->active_space;
}

static bool
vm_address_in_shm_window(uint64_t virtual_address)
{
	return virtual_address >= VM_SHM_BASE && virtual_address < VM_SHM_BASE + VM_SHM_WINDOW_SIZE;
}

const vm_address_space_t *
vm_address_space_current(void)
{
	return vm_cpu_space();
}

bool
vm_address_space_create(vm_address_space_t *space)
{
	if (space == 0 || space->active || pmap_root_physical(&space->pmap) != 0ULL) {
		return false;
	}

	memset(space, 0, sizeof(*space));

	if (!pmap_create(&space->pmap)) {
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
static void
vm_space_leave(vm_address_space_t *space, processor_t cpu)
{
	cpuset_remove_atomic(&space->active_cpus, cpu->cpu_id);
	__atomic_store_n(&space->active, !cpuset_empty(&space->active_cpus), __ATOMIC_RELEASE);
}

/*
 * vm_address_space_activate
 *
 * Load `space` into this CPU's TTBR0. The CPU joins the space's membership set
 * and publishes that (the backend's leading barrier) BEFORE the root is
 * installed, so anyone who checks the set afterwards sees it (the store half of
 * the pairing with the load in vm_address_space_quiesce). The previous space is
 * left only afterwards.
 */
bool
vm_address_space_activate(vm_address_space_t *space)
{
	if (space == 0 || pmap_root_physical(&space->pmap) == 0ULL) {
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

	if (!pmap_activate(&space->pmap)) {
		/* Precondition failed (VMM not ready): undo the membership join. */
		cpuset_remove_atomic(&space->active_cpus, cpu->cpu_id);
		__atomic_store_n(&space->active, !cpuset_empty(&space->active_cpus), __ATOMIC_RELEASE);
		ml_irq_restore(irq_state);
		return false;
	}

	cpu->active_space = space;

	if (previous != 0) vm_space_leave(previous, cpu);

	ml_irq_restore(irq_state);
	return true;
}

bool
vm_address_space_deactivate(void)
{
	if (!g_vmm.enabled || !g_vmm.kernel_pointers_rebased) return false;

	uint64_t irq_state = ml_irq_save();
	processor_t cpu = current_processor();
	vm_address_space_t *previous = cpu->active_space;

	if (previous == 0) {
		/*
		 * Nothing loaded. The boot CPU's boot-time identity map is torn down
		 * once, here or in kern_init. This is a kernel/boot (higher-half) path
		 * concern, out of the user-pmap scope, so it stays a direct VMM call.
		 */
		bool ok = true;

		if (cpu->cpu_id == 0U && !g_vmm.ttbr0_disabled) ok = vmm_disable_ttbr0();

		ml_irq_restore(irq_state);
		return ok;
	}

	(void)pmap_deactivate();
	cpu->active_space = 0;
	vm_space_leave(previous, cpu);

	ml_irq_restore(irq_state);
	return true;
}

/*
 * vm_address_space_quiesce
 *
 * Wait until no CPU has `space` in TTBR0 (its member set is empty), so its page
 * tables and pages can be freed. The caller has already ensured no thread of the
 * space will run again; CPUs still in the set are finishing their last switch
 * away from it and are kicked with a reschedule IPI to get there promptly. This
 * CPU leaves the space itself if it is in it.
 *
 * Pairing: the load of the set here is preceded by a full barrier, and a CPU
 * adds itself to the set and executes a full barrier before it can walk the
 * tables, so either this sees the CPU or the CPU sees every change made so far.
 *
 * Returns false (freeing nothing: the caller must leak rather than free) if the
 * set has not emptied after about two seconds.
 */
bool
vm_address_space_quiesce(vm_address_space_t *space)
{
	if (space == 0 || pmap_root_physical(&space->pmap) == 0ULL) return false;

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

bool
vm_address_space_map_page_locked(vm_address_space_t *space, uint64_t virtual_address, uint64_t physical_address,
								 vm_user_protection_t protection)
{
	if (space == 0) return false;

	if (!pmap_enter(&space->pmap, virtual_address, physical_address, protection, false, &space->active_cpus)) {
		return false;
	}

	space->resident_pages++;
	if (vm_address_in_shm_window(virtual_address)) space->shared_pages++;

	return true;
}

bool
vm_address_space_map_page(vm_address_space_t *space, uint64_t virtual_address, uint64_t physical_address,
						  vm_user_protection_t protection)
{
	if (space == 0) return false;

	nxu_spin_lock(&space->lock);
	bool installed = vm_address_space_map_page_locked(space, virtual_address, physical_address, protection);
	nxu_spin_unlock(&space->lock);

	return installed;
}

bool
vm_address_space_unmap_page(vm_address_space_t *space, uint64_t virtual_address)
{
	if (space == 0) return false;

	nxu_spin_lock(&space->lock);

	bool removed = pmap_remove(&space->pmap, virtual_address, &space->active_cpus);

	if (removed) {
		space->resident_pages--;
		if (vm_address_in_shm_window(virtual_address)) space->shared_pages--;
	}

	nxu_spin_unlock(&space->lock);

	return removed;
}

bool
vm_address_space_restrict_page(vm_address_space_t *space, uint64_t address)
{
	if (space == 0) return false;

	nxu_spin_lock(&space->lock);
	bool ok = pmap_protect(&space->pmap, address, VM_PROT_RO, &space->active_cpus);
	nxu_spin_unlock(&space->lock);

	return ok;
}

bool
vm_address_space_query_page(const vm_address_space_t *space, uint64_t virtual_address, vm_user_page_mapping_t *mapping)
{
	if (space == 0 || mapping == 0) return false;

	/* space is logically const to callers (this only inspects a mapping), but
	 * the lock still has to be taken to avoid reading a page-table entry that a
	 * concurrent map/unmap on another thread of the same task is publishing. */
	nxu_spinlock_t *lock = (nxu_spinlock_t *)&space->lock;

	nxu_spin_lock(lock);
	bool ok = pmap_query(&space->pmap, virtual_address, mapping);
	nxu_spin_unlock(lock);

	return ok;
}

bool
vm_address_space_translate_read(const vm_address_space_t *space, uint64_t virtual_address, uint64_t *physical_address)
{
	if (!vm_address_space_is_active(space) || physical_address == 0) return false;

	return pmap_translate(&space->pmap, virtual_address, false, physical_address);
}

bool
vm_address_space_translate_write(const vm_address_space_t *space, uint64_t virtual_address, uint64_t *physical_address)
{
	if (!vm_address_space_is_active(space) || physical_address == 0) return false;

	return pmap_translate(&space->pmap, virtual_address, true, physical_address);
}

bool
vm_address_space_is_active(const vm_address_space_t *space)
{
	/* "Is it the space this CPU walks?" -- the calling CPU's TTBR0, no one else's. */
	return space != 0 && space == vm_cpu_space();
}

uint64_t
vm_address_space_root_physical(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : pmap_root_physical(&space->pmap);
}

uint64_t
vm_address_space_table_count(const vm_address_space_t *space)
{
	return space == 0 ? 0ULL : pmap_table_count(&space->pmap);
}

/*
 * vm_address_space_cow_break
 *
 * Give the caller a private, writable copy of a page fork left shared. Two
 * threads of the process (on two CPUs) can break the same page at once, and
 * another process holding the other reference can drop it at any moment, so
 * nothing read under the lock is trusted after it is released: the copy is made
 * outside the lock (it allocates) and installed only if the mapping is still the
 * copy-on-write mapping of the same physical page. If it is not, the copy is
 * discarded and the page is looked at again. If somebody else has already made
 * it writable, that is success: the faulting instruction can simply run again.
 */
bool
vm_address_space_cow_break(vm_address_space_t *space, uint64_t virtual_address)
{
	if (space == 0) return false;

	uint64_t page_va = virtual_address & ~(PMM_PAGE_SIZE - 1ULL);

	for (uint32_t attempt = 0U; attempt < 8U; attempt++) {
		nxu_spin_lock(&space->lock);

		vm_user_page_mapping_t mapping;

		if (!pmap_query(&space->pmap, page_va, &mapping)) {
			nxu_spin_unlock(&space->lock);
			return false;
		}

		if (!mapping.cow) {
			/* Broken by another thread already (or never shared): writable now? */
			bool writable = mapping.protection == VM_PROT_RW;

			nxu_spin_unlock(&space->lock);
			return writable;
		}

		uint64_t old_physical = mapping.physical_address;

		/* The other owner is gone: nothing to copy, just take the page back. */
		if (pmm_page_refcount(old_physical) == 1U) {
			bool ok = pmap_replace(&space->pmap, page_va, old_physical, VM_PROT_RW, false, &space->active_cpus);

			nxu_spin_unlock(&space->lock);
			return ok;
		}

		nxu_spin_unlock(&space->lock);

		uint64_t new_physical;

		if (!pmm_allocate_page(&new_physical)) return false;

		if (!pmap_copy_page(new_physical, old_physical)) {
			(void)pmm_free_page(new_physical);
			return false;
		}

		nxu_spin_lock(&space->lock);

		vm_user_page_mapping_t again;

		if (!pmap_query(&space->pmap, page_va, &again) || !again.cow || again.physical_address != old_physical) {
			/* The page changed while it was being copied: throw the copy away and look again. */
			nxu_spin_unlock(&space->lock);
			(void)pmm_free_page(new_physical);
			continue;
		}

		bool ok = pmap_replace(&space->pmap, page_va, new_physical, VM_PROT_RW, false, &space->active_cpus);

		nxu_spin_unlock(&space->lock);

		if (!ok) {
			(void)pmm_free_page(new_physical);
			return false;
		}

		/* This space's mapping no longer refers to the shared page. */
		(void)pmm_free_page(old_physical);

		return true;
	}

	return false;
}

static void
vm_pmm_free_page(uint64_t physical_address)
{
	(void)pmm_free_page(physical_address);
}

uint64_t
vm_address_space_release_pages(vm_address_space_t *space)
{
	if (space == 0 || pmap_root_physical(&space->pmap) == 0ULL) return 0ULL;

	nxu_spin_lock(&space->lock);

	uint64_t released = pmap_release_user_pages(&space->pmap, vm_pmm_free_page);

	space->resident_pages = 0ULL;
	space->shared_pages = 0ULL;

	nxu_spin_unlock(&space->lock);

	return released;
}

bool
vm_address_space_destroy(vm_address_space_t *space)
{
	if (space == 0 || pmap_root_physical(&space->pmap) == 0ULL) return false;

	/*
	 * Never free tables a CPU can still walk: this CPU leaves the space, and
	 * every other CPU that has it loaded is waited for. If one never leaves
	 * (a thread of the space is still running), the tables are leaked, not
	 * freed: a leak is a bug report, a use-after-free is corruption.
	 */
	if (!vm_address_space_quiesce(space)) {
		kprintf("vm_address_space_destroy: a CPU still has the space loaded; its page tables are leaked\n");
		return false;
	}

	if (!pmap_destroy(&space->pmap)) return false;

	memset(space, 0, sizeof(*space));

	return true;
}

/*
 * vm_address_space_fork
 *
 * Two-phase, transactional fork (see the Fork protocol in
 * doc/vm/unified-pmap.md). Phase 1 enumerates the parent's leaves (read-only,
 * backend). Phase 2 applies them to the child -- retaining each page, marking
 * private writable memory copy-on-write in both spaces -- with per-leaf
 * progress so that any partial failure (including a failure midway through the
 * failing leaf) is rolled back exactly. Both space locks are held for the whole
 * operation, acquired in ascending address order; freezing the parent is what
 * makes a COW downgrade safe to undo.
 */

typedef enum {
	FORK_NONE = 0,
	FORK_RETAINED,
	FORK_CHILD_ENTERED,
	FORK_DONE,
} fork_progress_t;

static void
vm_fork_undo_leaf(vm_address_space_t *parent, vm_address_space_t *child, const pmap_leaf_t *leaf,
				  fork_progress_t progress, bool private_writable)
{
	if (progress >= FORK_CHILD_ENTERED) {
		(void)pmap_remove(&child->pmap, leaf->va, &child->active_cpus);
	}

	/* Parent is only downgraded once a leaf reaches FORK_DONE. */
	if (progress >= FORK_DONE && private_writable) {
		(void)pmap_replace(&parent->pmap, leaf->va, leaf->pa, VM_PROT_RW, false, &parent->active_cpus);
	}

	if (progress >= FORK_RETAINED) {
		(void)pmm_free_page(leaf->pa);
	}
}

static bool
vm_fork_leaf_private_writable(const pmap_leaf_t *leaf)
{
	return leaf->prot == VM_PROT_RW && !vm_address_in_shm_window(leaf->va);
}

bool
vm_address_space_fork(vm_address_space_t *parent, vm_address_space_t *child)
{
	if (parent == 0 || child == 0 || pmap_root_physical(&parent->pmap) == 0ULL) return false;
	if (!vm_address_space_create(child)) return false;

	/* Deterministic global lock order: ascending by address-space address. */
	vm_address_space_t *first = (uintptr_t)parent < (uintptr_t)child ? parent : child;
	vm_address_space_t *second = (uintptr_t)parent < (uintptr_t)child ? child : parent;

	nxu_spin_lock(&first->lock);
	nxu_spin_lock(&second->lock);

	bool ok = true;
	uint64_t cap = parent->resident_pages;
	pmap_leaf_t *leaves = 0;
	uint64_t count = 0ULL;

	if (cap > 0ULL) {
		leaves = (pmap_leaf_t *)kmalloc((size_t)cap * sizeof(pmap_leaf_t));
		if (leaves == 0) ok = false;

		/* Phase 1: read-only enumeration of the parent. */
		if (ok && !pmap_enumerate(&parent->pmap, leaves, cap, &count)) ok = false;
	}

	/* Phase 2: apply, tracking how far the failing leaf progressed. */
	uint64_t failed_at = count;
	fork_progress_t failed_progress = FORK_NONE;

	for (uint64_t i = 0ULL; ok && i < count; i++) {
		bool private_writable = vm_fork_leaf_private_writable(&leaves[i]);
		fork_progress_t progress = FORK_NONE;

		if (!pmm_page_retain(leaves[i].pa)) {
			ok = false;
		} else {
			progress = FORK_RETAINED;

			vm_prot_t child_prot = private_writable ? VM_PROT_RO : leaves[i].prot;
			bool child_cow = private_writable ? true : leaves[i].cow;

			if (!pmap_enter(&child->pmap, leaves[i].va, leaves[i].pa, child_prot, child_cow, &child->active_cpus)) {
				ok = false;
			} else {
				progress = FORK_CHILD_ENTERED;

				if (private_writable &&
					!pmap_replace(&parent->pmap, leaves[i].va, leaves[i].pa, VM_PROT_RO, true, &parent->active_cpus)) {
					ok = false;
				} else {
					progress = FORK_DONE;
					child->resident_pages++;
					if (vm_address_in_shm_window(leaves[i].va)) child->shared_pages++;
				}
			}
		}

		if (!ok) {
			failed_at = i;
			failed_progress = progress;
			break;
		}
	}

	if (!ok) {
		/* Undo the failing leaf by its own progress, then every DONE leaf before it. */
		if (failed_at < count) {
			vm_fork_undo_leaf(parent, child, &leaves[failed_at], failed_progress,
							  vm_fork_leaf_private_writable(&leaves[failed_at]));
		}

		for (uint64_t j = failed_at; j-- > 0ULL;) {
			vm_fork_undo_leaf(parent, child, &leaves[j], FORK_DONE, vm_fork_leaf_private_writable(&leaves[j]));
		}

		child->resident_pages = 0ULL;
		child->shared_pages = 0ULL;
	}

	nxu_spin_unlock(&second->lock);
	nxu_spin_unlock(&first->lock);

	if (leaves != 0) kfree(leaves);

	if (!ok) {
		(void)vm_address_space_destroy(child);
		return false;
	}

	return true;
}
