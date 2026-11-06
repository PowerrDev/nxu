/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/vm/pmap.h
 *
 * The architecture-neutral pmap backend contract: the ONLY header the neutral
 * VM core includes to touch a translation. Each architecture implements these
 * functions under kern/vm/arch/<arch>/pmap.c against its own (opaque) pmap_t.
 *
 * Ownership boundary (see doc/vm/unified-pmap.md):
 *   - The core owns physical-page ownership, reference counting, COW policy and
 *     all address-space bookkeeping; it calls the pmm, never the backend, for
 *     those.
 *   - The backend owns page-table formats, descriptor manipulation, the
 *     translation root, activation, and TLB invalidation.
 *
 * This header depends only on neutral types (<kern/vm/vm_types.h>) and the
 * opaque pmap_t (<kern/vm/machine/pmap_types.h>). It deliberately does NOT
 * include <vm/address_space.h>: the contract is independent of the
 * address-space struct.
 */
#ifndef NXU_KERN_VM_PMAP_H
#define NXU_KERN_VM_PMAP_H

#include <kern/vm/machine/pmap_types.h> /* opaque pmap_t */
#include <kern/vm/vm_types.h>           /* vm_prot_t, vm_user_page_mapping_t */
#include <kern/cpuset.h>                /* nxu_cpuset_t */

#include <stdbool.h>
#include <stdint.h>

/* ---- lifecycle ---------------------------------------------------------- */

/*
 * Allocate an empty translation root for `pmap`; no user mappings installed.
 * On failure `pmap` is left zeroed and nothing is allocated.
 */
bool pmap_create(pmap_t *pmap);

/*
 * Free every translation table and the root of `pmap`, then zero it.
 *
 * Precondition: no CPU may have this pmap active -- the core guarantees this
 * via vm_address_space_quiesce() before calling -- and every mapped page must
 * already have been released by the core. Frees only structure the pmap owns
 * exclusively, so it cannot partially fail.
 */
bool pmap_destroy(pmap_t *pmap);

/* ---- activation (root load lives here: TTBR0_EL1 / mov cr3) -------------- */
/*
 * The core performs the active_cpus bookkeeping and the memory ordering around
 * these per the activation protocol; the backend performs only the root load /
 * unload plus its own local TLB maintenance.
 */

/* Make `pmap` the translation root on the calling CPU. */
bool pmap_activate(pmap_t *pmap);

/*
 * Return the calling CPU to the kernel-only regime (disable TTBR0 / load the
 * kernel root), including the local TLB flush the deactivation protocol
 * requires before the CPU is removed from active_cpus.
 */
bool pmap_deactivate(void);

/* ---- single-page operations --------------------------------------------- */
/*
 * `va` and `pa` are page-aligned. `active` is the core-owned set of CPUs on
 * which `pmap` may currently be loaded, passed in so the backend can target
 * cross-CPU invalidation (see the synchronization section of the design doc).
 * Each of these touches exactly one leaf descriptor: it either fully succeeds
 * or makes no observable change.
 */

/*
 * Install one mapping into an EMPTY slot. If the slot is already present this
 * MUST fail (return false) and change nothing -- it never silently overwrites;
 * use pmap_replace for an existing mapping. On table-allocation failure no
 * descriptor is installed (any intermediate empty tables already linked are
 * harmless and reclaimed by pmap_destroy). Installing into a previously empty
 * slot carries no cross-CPU shootdown obligation (no stale entry existed).
 */
bool pmap_enter(pmap_t *pmap, uint64_t va, uint64_t pa, vm_prot_t prot, bool cow,
				const nxu_cpuset_t *active);

/*
 * Remove one present mapping. Does NOT free the physical page: the core owns
 * the refcount and calls pmm_free_page(). Globally coherent on return. Fails
 * (changing nothing) only if the slot is absent.
 */
bool pmap_remove(pmap_t *pmap, uint64_t va, const nxu_cpuset_t *active);

/*
 * Narrow the permissions of an existing mapping (never widens, never changes
 * the physical page or ownership). Uses break-before-make where the
 * architecture requires it. Used by loader finalization.
 */
bool pmap_protect(pmap_t *pmap, uint64_t va, vm_prot_t prot, const nxu_cpuset_t *active);

/*
 * Replace an EXISTING mapping in place: repoint `va` at `new_pa` with `prot`
 * and the given `cow` state. The only call permitted to change a present slot.
 * Used (a) by the core after copying a COW page, and (b) during fork to
 * downgrade a parent's private writable page to RO+COW and, on rollback, to
 * restore it. Globally coherent on return; fails and changes nothing if the
 * slot is absent.
 */
bool pmap_replace(pmap_t *pmap, uint64_t va, uint64_t new_pa, vm_prot_t prot, bool cow,
				  const nxu_cpuset_t *active);

/* ---- read-only inspection (no invalidation) ----------------------------- */

bool pmap_query(const pmap_t *pmap, uint64_t va, vm_user_page_mapping_t *out);
bool pmap_translate(const pmap_t *pmap, uint64_t va, bool write, uint64_t *pa);

/* ---- COW / fork helpers ------------------------------------------------- */

/*
 * Copy one physical page's contents, src and dst given as physical addresses.
 * The backend chooses how it reaches the bytes: a permanent direct map where
 * it has one (arm64), or a transient temporary mapping it installs and tears
 * down internally (no permanent direct map required). A transient mapping must
 * be torn down on every exit path. Neutral code cannot do this portably, hence
 * it lives in the backend.
 */
bool pmap_copy_page(uint64_t dst_pa, uint64_t src_pa);

/* One present leaf of an address space, as produced by enumeration. */
typedef struct {
	uint64_t  va;
	uint64_t  pa;
	vm_prot_t prot;
	bool      cow; /* true if the parent entry is already copy-on-write */
} pmap_leaf_t;

/*
 * Fork phase 1 (read-only): write up to `cap` of `pmap`'s present user leaves
 * into `out` in ascending VA order and store the total count in `*count`.
 * Returns false (touching nothing) if `cap` is too small for the whole set;
 * the core sizes `out` from the address space's resident-page count under the
 * lock, so that is an assertion rather than an expected path. Performs no
 * mutation and no allocation.
 *
 * Fork phase 2 is performed by the core using pmap_enter / pmap_replace /
 * pmap_copy_page plus pmm reference counting -- there is intentionally no
 * pmap_fork(), so every refcount decision stays in the core and rollback is a
 * deterministic walk over this leaf array (see the Fork protocol in the doc).
 */
bool pmap_enumerate(const pmap_t *pmap, pmap_leaf_t *out, uint64_t cap, uint64_t *count);

/*
 * Teardown: unmap every present user leaf and report each one's physical page
 * to `free_page` (the core's pmm free), then invalidate in bulk. Returns the
 * number of pages released. The core owns the frees (via the callback); the
 * backend owns the table walk and the single bulk TLB flush, which is why this
 * is one backend call rather than a per-page loop (it preserves the one-flush
 * teardown the pre-split code did). Leaves the translation tables in place for
 * a subsequent pmap_destroy.
 */
uint64_t pmap_release_user_pages(pmap_t *pmap, void (*free_page)(uint64_t physical_address));

/* ---- accessors the core exposes but does not interpret ------------------- */

uint64_t pmap_root_physical(const pmap_t *pmap); /* e.g. debug / IPI payloads */
uint64_t pmap_table_count(const pmap_t *pmap);   /* accounting / stats        */

#endif /* NXU_KERN_VM_PMAP_H */
