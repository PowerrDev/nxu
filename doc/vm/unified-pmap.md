# Unified VM Subsystem and the `pmap` Backend Contract

Status: **design, pre-implementation**. This document is the plan of record for
splitting the VM subsystem into one architecture-neutral core and a set of
per-architecture `pmap` backends. No implementation files are to change until
the open decisions in the final section are resolved.

## Motivation: what exists today

The tree currently contains **three near-complete, parallel VM
implementations**, not one shared subsystem with thin backends:

| Layer | arm64 | i386 | x86_64 |
| --- | --- | --- | --- |
| Upper layer (map / fault) | `vm/vm_map.c`, `vm/vm_fault.c` | own copies: `kern/i386/vm_fault.c`, `vm_init.c` | folded into `vm/x86_64/` |
| Page tables ("pmap") | `vm/vmm.c`, `vmm_ttbr1.c`, `vmm_tables.c`, `address_space.c` | `kern/i386/pmap.c`, `vmm_i386.c`, `address_space_i386.c` | `vm/x86_64/vmm.c`, `address_space.c` |
| Physical allocator | `vm/pmm.c` — **shared by all three** | same | same |

Only the physical memory manager (`vm/pmm.c`) is genuinely shared. The
top-level `vm/` directory is, in practice, the **arm64 backend wearing a
"shared" name**: its public headers (`vm/vmm.h`, `vm/address_space.h`) leak
`TTBR0`/`TTBR1`, `EL0`/`EL1`, `AT S1E1R/W`, break-before-make, ASIDs and
inner-shareable invalidation throughout. `vm/x86_64/` reuses those same headers
but means CR3 / PML4 / NX by them — which is why
`vm_address_space_t.root_physical` is documented as "the value written to
TTBR0_EL1" even on x86.

Two pieces of the intended architecture are, however, **already latent and
correct**:

- `vm/pmm.c` is cleanly shared (physical ownership + reference counting).
- `vm/vm_fault.c` and `vm/vm_map.c` are already architecture-neutral.
  `vm_fault_user()` calls only `vm_address_space_query_page`,
  `vm_map_region_at`, `pmm_allocate_page`, `vm_address_space_map_page_locked`
  and `vm_address_space_cow_break` — it never touches a page-table format.

The seam the new design formalizes therefore **already exists** as the
`vm_address_space_*` API. It is merely triplicated, leaky, and uncontracted.

## Target architecture

```
                         NXU Kernel
                             |
                   Shared VM core  (arch-neutral)
         pmm · vm_map · vm_fault · address-space bookkeeping
         physical ownership · refcounts · COW policy · regions
                             |
                      vm/pmap.h          (the contract)
                             |
         +-------------------+-------------------+
         |                   |                   |
   arch/arm64/pmap.c   arch/i386/pmap.c   arch/x86_64/pmap.c
   TTBR0 · ASID ·      CR3 (32-bit) ·     CR3 · PCID · NX ·
   AArch64 descr. ·    x86 PTE · invlpg   x86-64 PTE · invlpg
   TLBI broadcast      + IPI shootdown    + IPI shootdown
```

This is the Mach `vm_map` / `pmap` division. The naming is not invented: i386
already calls its backend `pmap`, and a neutral `vm_map.c` already exists.

### File layout (per instruction)

- `vm/pmap.h` — architecture-neutral `pmap` interface. **The only header
  neutral code includes to touch a translation.**
- `vm/vm_types.h` — neutral VM types (`vm_prot_t`,
  `vm_user_page_mapping_t`) the contract needs without depending on
  `address_space.h`.
- `vm/machine/pmap_types.h` — compile-time arch dispatch to the opaque
  `pmap_t` definition (clone of the existing `kern/machine/vm_param.h`
  pattern).
- `vm/arch/arm64/` — reference backend (`pmap.c`, `pmap_types.h`),
  populated from today's `vm/address_space.c` + `vm/vmm*.c` arch halves.
- `vm/arch/i386/` — future i386 backend (Phase 1).
- `vm/arch/x86_64/` — future x86_64 backend (Phase 2).

The architecture-neutral core (`pmm.c`, `vm_map.c`, `vm_fault.c`,
`address_space.c`) **stays at its present top-level `vm/` location for Phase 0**
(approved Decision 1); relocating it would be unrelated churn that Requirement 7
forbids. A later consolidation of the core under `vm/` is possible but is
explicitly not part of this work.

## Ownership boundaries (normative)

These boundaries are the contract. Any code that violates them is a bug.

| Concern | Owner | Rationale |
| --- | --- | --- |
| Physical page allocation / free | **Core** (`pmm`) | arch-independent |
| Physical page **reference counting** | **Core** (`pmm_page_retain`, `pmm_page_refcount`) | shared pages (`vm_shm`, COW) span arches identically |
| COW **policy** (when to reclaim vs. copy) | **Core** | a refcount decision, not a hardware one |
| Address-space **bookkeeping** (`resident_pages`, `shared_pages`, region list, `active`/`active_cpus`, the per-space lock) | **Core** | arch-independent accounting |
| Region list / anonymous mmap windows | **Core** (`vm_map.c`) | arch-independent |
| Fault resolution logic | **Core** (`vm_fault.c`) | arch-independent policy |
| Page-table **format** / descriptor bits | **Backend** | AArch64 vs. x86 PTE differ |
| Descriptor manipulation (build / parse / edit) | **Backend** | format-specific |
| Translation **root** management (alloc / free / value) | **Backend** | TTBR0 vs. CR3 |
| Address-space **activation** (loading the root) | **Backend** | TTBR0_EL1 vs. `mov cr3` |
| **TLB invalidation** (local instruction + decision whether a broadcast suffices) | **Backend** | TLBI vs. invlpg; ASID vs. PCID |
| Page-byte **copy** during COW/fork (via the backend's own alias) | **Backend** | needs a mapping of the physical page |

Requirement cross-reference: this table satisfies Requirements **1** (core owns
physical ownership, refcounts, COW policy, bookkeeping) and **2** (backend owns
formats, descriptors, roots, activation, TLB).

## The opaque `pmap_t` rule

`pmap_t` is **opaque to all neutral code** (Requirement 4). Neutral code may
hold a `pmap_t` (embedded in `vm_address_space_t`), pass its address to
`pmap_*` functions, and nothing else. It may never read or write a field, and
no `pmap_t` field name may appear outside `vm/arch/<arch>/`.

### Practical vs. strict opacity (clarification)

Because `pmap_t` is embedded **by value** in `vm_address_space_t`, the neutral
header `vm/address_space.h` must `#include <vm/machine/pmap_types.h>` so
the compiler knows the struct's size and alignment. This means the concrete
layout is *visible* to neutral translation units at the C level — this is
**practical opacity enforced by discipline and CI, not strict C-level
opacity** (which would require an out-of-line pointer and a heap allocation per
address space, a cost we reject for a per-task object).

The discipline is absolute and simple to state:

- **Only files under `vm/arch/<arch>/` may name a `pmap_t` field.**
- **Neutral code never touches `space->pmap` internals.** It treats
  `&space->pmap` purely as a handle to hand to `pmap_*`. The tokens
  `space->pmap.` (field access) and `.root`/`.root_physical`/`.table_count`
  anywhere outside the backend are contract violations.
- The CI opacity guard (see Validation) greps for exactly these patterns, so
  the rule is mechanically enforced rather than merely asserted.

The alternative — a truly opaque pointer `pmap_t *pmap` with backend-side
allocation — remains available if a future arch needs a `pmap_t` too large to
embed comfortably; it is not adopted now.

`vm/pmap.h` forward-declares nothing architecture-specific. The concrete
layout is supplied only through `vm/machine/pmap_types.h`:

```c
/* vm/machine/pmap_types.h */
#if   defined(__aarch64__)
#include <vm/arch/arm64/pmap_types.h>
#elif defined(__i386__)
#include <vm/arch/i386/pmap_types.h>
#elif defined(__x86_64__)
#include <vm/arch/x86_64/pmap_types.h>
#else
#error "vm/machine/pmap_types.h: unsupported target architecture"
#endif
```

```c
/* vm/arch/arm64/pmap_types.h — reference backend */
typedef struct pmap {
    uint64_t *root;           /* TTBR1 direct-map pointer to the L1 root table */
    uint64_t  root_physical;  /* value written to TTBR0_EL1 */
    uint64_t  table_count;    /* translation tables this pmap owns */
} pmap_t;
```

The size of `pmap_t` varies by arch (i386 is 32-bit, x86_64 adds PCID/NX
state). Because the core only ever holds it **by value inside
`vm_address_space_t`** and passes it **by pointer**, the varying size is
contained — no neutral code allocates a bare `pmap_t` or assumes its size.

Requirement **5** is enforced structurally: `TTBR`, `CR3`, `ASID`, `PCID`,
`EL0/EL1` and descriptor bit layouts appear **only** under
`vm/arch/<arch>/`. `vm/pmap.h` and the neutral core must grep clean of
all of them. A CI guard (see Validation) makes this mechanical.

## Neutral VM types header: `vm/vm_types.h`

All types the contract needs that are **not** architecture-specific live in one
genuinely neutral header, `vm/vm_types.h`. This is what lets `pmap.h`
depend only on neutral types — crucially, **`pmap.h` does not include
`address_space.h`** (correction: the contract must not depend on the
address-space struct). The dependency direction is one-way:

```
vm/vm_types.h   (vm_prot_t, vm_user_page_mapping_t)  ← no VM deps
        ↑                         ↑
vm/pmap.h            vm/address_space.h
(+ pmap_types.h, cpuset)  (+ pmap_types.h embeds pmap_t)
```

`vm_types.h` defines:

```c
/* Unifies vmm_protection_t (vm/vmm.h) and vm_user_protection_t
 * (vm/address_space.h) — today two names for the same three states. */
typedef enum {
    VM_PROT_RW,   /* read + write   */
    VM_PROT_RO,   /* read only      */
    VM_PROT_RX,   /* read + execute */
} vm_prot_t;

/* Neutral result of a page query. `cow` set => hardware permission is RO and
 * `protection` reads VM_PROT_RO; a writer must break sharing first. Moved here
 * (out of address_space.h) so pmap.h can use it without depending on the
 * address-space struct. */
typedef struct {
    uint64_t  physical_address;
    vm_prot_t protection;
    bool      cow;
} vm_user_page_mapping_t;
```

**Protection aliases (Decision 6).** Thin `#define` aliases mapping the old
enum names to `vm_prot_t` are kept **throughout Phase 0** so the unmigrated
kernel higher-half code in `vmm.*` keeps compiling unchanged (it is out of
scope — Requirement 6). Their removal is **explicitly deferred to the future
higher-half / direct-map facade migration**, which is the phase that rewrites
`vmm.*`; removing them earlier would pull `vmm.*` into Phase 0's scope and is
therefore forbidden here. The aliases are tracked as a known, time-boxed debt
owned by that later phase.

## The backend contract: `vm/pmap.h`

```c
#include <vm/machine/pmap_types.h>   /* opaque pmap_t */
#include <vm/vm_types.h>             /* vm_prot_t, vm_user_page_mapping_t */
#include <kern/cpuset.h>
/* NB: pmap.h does NOT include address_space.h — the contract is independent
 * of the address-space struct. */

/* ---- lifecycle ---------------------------------------------------------- */

/* Allocate an empty translation root for `pmap`. No user mappings installed.
 * Failure: `pmap` is left zeroed; nothing is allocated. */
bool pmap_create(pmap_t *pmap);

/* Free every translation table and the root of `pmap`, then zero it.
 * Precondition: no CPU may have this pmap active (the core guarantees this via
 * vm_address_space_quiesce before calling), and all mapped pages must already
 * have been released by the core. Cannot partially fail: it frees structure it
 * exclusively owns. */
bool pmap_destroy(pmap_t *pmap);

/* ---- activation (root load lives here: TTBR0_EL1 / mov cr3) -------------- */
/* Caller (the core) performs the active_cpus bookkeeping and barriers around
 * these per the activation protocol; the backend performs only the root load /
 * unload and its own local TLB maintenance. */

/* Make `pmap` the translation root on the calling CPU. */
bool pmap_activate(pmap_t *pmap);

/* Return the calling CPU to the kernel-only regime (disable TTBR0 / load the
 * kernel root), including the local TLB flush the protocol requires before the
 * CPU is removed from active_cpus. */
bool pmap_deactivate(void);

/* ---- single-page operations --------------------------------------------- */
/* `va`/`pa` page-aligned. `active` is the core-owned set of CPUs on which
 * `pmap` may currently be loaded, passed in so the backend can target
 * cross-CPU invalidation (see Synchronization). Every mutation is globally
 * coherent before the call returns. Each of these touches exactly one leaf
 * descriptor: it either fully succeeds or makes no visible change (see
 * Failure and rollback). */

/* Install one mapping into an EMPTY slot. If the slot is already present,
 * pmap_enter MUST fail (return false) and change nothing — it never silently
 * overwrites. Use pmap_replace to change an existing mapping. On table-
 * allocation failure no descriptor is installed (see rollback note). */
bool pmap_enter(pmap_t *pmap, uint64_t va, uint64_t pa,
                vm_prot_t prot, bool cow, const nxu_cpuset_t *active);

/* Remove one present mapping. Does NOT free the physical page (the core owns
 * the refcount and calls pmm_free_page). Fails only if the slot is absent. */
bool pmap_remove(pmap_t *pmap, uint64_t va, const nxu_cpuset_t *active);

/* Narrow permissions on an existing mapping (never widens, never changes the
 * physical page or ownership). Used by loader finalization. */
bool pmap_protect(pmap_t *pmap, uint64_t va, vm_prot_t prot,
                  const nxu_cpuset_t *active);

/* Replace an EXISTING mapping in place: repoint `va` at `new_pa` with `prot`
 * and the given `cow` state. This is the only call that may change a present
 * slot. Used (a) by the core after copying a COW page, and (b) during fork to
 * downgrade a parent's private writable page to RO+COW. Fails and changes
 * nothing if the slot is absent. The `cow` argument is explicit so the two
 * callers express intent directly. */
bool pmap_replace(pmap_t *pmap, uint64_t va, uint64_t new_pa,
                  vm_prot_t prot, bool cow, const nxu_cpuset_t *active);

/* ---- read-only inspection (no invalidation) ----------------------------- */

bool pmap_query(const pmap_t *pmap, uint64_t va, vm_user_page_mapping_t *out);
bool pmap_translate(const pmap_t *pmap, uint64_t va, bool write, uint64_t *pa);

/* ---- COW / fork helpers ------------------------------------------------- */

/* Copy one physical page's contents, src and dst given as physical addresses.
 * The backend chooses how it reaches the bytes: a permanent direct map if it
 * has one (arm64), or a transient per-CPU temporary mapping it installs and
 * tears down internally (no permanent direct map required). Neutral code
 * cannot do this portably, hence it lives in the backend. */
bool pmap_copy_page(uint64_t dst_pa, uint64_t src_pa);

/* ---- fork: two-phase enumerate / apply (see Fork protocol) --------------- */
/* One leaf of a parent address space, as produced by enumeration. */
typedef struct {
    uint64_t  va;
    uint64_t  pa;
    vm_prot_t prot;
    bool      cow;   /* true if the parent entry is already COW */
} pmap_leaf_t;

/* Phase 1 (read-only): write up to `cap` of `pmap`'s present user leaves into
 * `out`, in ascending VA, and store the total count in `*count`. Returns false
 * (touching nothing) if `cap` is too small for the whole set; the core sizes
 * `out` from space->resident_pages under the lock, so this is an assertion,
 * not an expected path. Performs NO mutation and NO allocation. */
bool pmap_enumerate(const pmap_t *pmap, pmap_leaf_t *out, uint64_t cap,
                    uint64_t *count);

/* Phase 2 is performed by the CORE using pmap_enter / pmap_replace /
 * pmap_copy_page plus pmm refcounting — there is no pmap_fork(). This keeps
 * every refcount decision in the core (Req 1) and every descriptor edit in the
 * backend (Req 2), with a single, inspectable rollback list (the leaf array).*/

/* ---- accessors the core exposes but does not interpret ------------------- */

uint64_t pmap_root_physical(const pmap_t *pmap);  /* for debug / IPI payloads */
uint64_t pmap_table_count(const pmap_t *pmap);     /* for accounting / stats   */
```

The fork path is deliberately **two-phase enumerate/apply rather than a
backend callback** (Decision 3). The callback form interleaves backend table
walking with core refcounting under the backend's lock, which makes partial-
failure rollback ad hoc — the core would have to reconstruct, from inside a
half-finished walk, exactly which pages it had retained and which parent
entries it had downgraded. The two-phase form makes the applied set an explicit
array the core already holds, so rollback is a deterministic reverse iteration
over a known list (see Fork protocol). That strictly clearer failure/rollback
semantics is the reason two-phase wins here.

## Synchronization and TLB-shootdown guarantees (Requirement 3)

This section is normative. It must hold identically on every backend even
though the mechanism differs.

### Locks

- **One lock per address space**: `vm_address_space_t.lock`. It guards
  page-table mutation *and* the region list (`mmap_entries`, `mmap_next_va`).
- **Lock ordering**: `space->lock` → `pmm` internal lock. A backend that must
  allocate a table page calls `pmm_allocate_page` **while holding
  `space->lock`**; `pmm` must never call back into a pmap under its own lock.
  No other nesting is permitted. Activation/deactivation run on the scheduler's
  context-switch path under the scheduler's serialization and do **not** take
  `space->lock`.
- **The fault critical section is atomic**: `vm_fault_user` holds `space->lock`
  across "region still covers this VA?" **and** `pmap_enter`, so an munmap that
  drops a region cannot be interleaved with a fault that maps into it. Backends
  must tolerate being called with `space->lock` held and must not acquire it
  themselves (`pmap_enter` is the locked-path primitive; the core provides the
  unlocked `vm_address_space_map_page` wrapper that takes the lock around it).

### The `active_cpus` set: representation and atomicity

`vm_address_space_t.active_cpus` is **core-owned** and names every CPU that may
currently have `space` loaded as its translation root. It is neutral
bookkeeping — a CPU bitmap — and exposes **no** architecture assumption, so
passing it into backend ops does not violate Requirement 4.

Its representation and the atomicity of every access are specified, because the
protocol's correctness depends on them:

- **Storage.** An array of lock-free atomic words, `_Atomic uint64_t
  word[(NCPU + 63) / 64]`; CPU *c* is bit `c & 63` of `word[c >> 6]`.
- **Add** (`cpuset_add`): `atomic_fetch_or(&word[i], 1ull << b,
  memory_order_acq_rel)`.
- **Remove** (`cpuset_remove`): `atomic_fetch_and(&word[i], ~(1ull << b),
  memory_order_acq_rel)`.
- **Snapshot** (`cpuset_snapshot`): per word, `atomic_load(&word[i],
  memory_order_acquire)` copied into a private array; the caller then iterates
  the copy.

**Concurrent updates to different bits of the same word.** Two CPUs joining and
leaving distinct spaces may touch different bits of the *same* storage word at
the same time. Add and remove are therefore **atomic read-modify-write**
(`fetch_or`/`fetch_and`), never a non-atomic load–modify–store, so neither can
clobber the other's bit — the hazard a naïve `word |= mask` would create. Each
word is independently atomic.

**Snapshots are per-word atomic, not globally atomic.** A snapshot that spans
several words may observe a CPU join after an earlier word was already sampled
(a "torn" multi-word view). The protocol **tolerates** this: a CPU that becomes
a member only after a word was sampled is covered by the activation ordering
below (it is guaranteed to observe the new descriptor), and any CPU present in
any sampled word is shot down. No atomic multi-word snapshot is required, and
none is taken.

**Two layers of ordering.** C11 atomics give atomicity and *compiler/other-core
software* ordering for accesses to `active_cpus`. They say nothing about the
**hardware page-table walker and TLB**, which are not C11 observers. Ordering
between a `active_cpus` access and a translation-table walk or TLB fill is
therefore established by **explicit architectural barriers** (ARM64
`DSB`/`DMB`/`ISB`/`TLBI`), layered on top of the atomics. Both layers are
required; neither substitutes for the other.

### Activation / deactivation / invalidation protocol (Requirement 3)

The hard requirement: **a CPU activating an address space must never end up
holding a TLB entry for a VA whose descriptor a concurrent CPU changed or
removed without shooting that CPU down.** The ordering below makes that
impossible without relying on any informal "full barrier" hand-wave; every step
names its atomic order and its architectural barrier.

In the sequences, "A" is a CPU mutating `space` (invalidator) and "B" is a CPU
activating `space`.

**Activator B — `vm_address_space_activate(space)`:**

```
1.  cpuset_add(&space->active_cpus, B)           ; atomic_fetch_or, acq_rel
2.  DSB ISH                                       ; (1) observable to all PEs
                                                  ;     before the root changes
3.  pmap_activate(&space->pmap):
        MSR TTBR0_EL1, <root_physical | ASID>
        ISB                                       ; new regime in effect here;
                                                  ;     only now may the walker
                                                  ;     fill from this space
```

**Invalidator A — inside `pmap_remove` / `pmap_protect` / `pmap_replace`** (a
change to an *existing* valid leaf uses break-before-make; see below). The
architecturally exact arm64 sequence for one VA:

```
1.  STR   <invalid or new descriptor>, [leaf]    ; edit the table
2.  DSB ISHST                                     ; the descriptor store is
                                                  ;     observable to table
                                                  ;     walkers before the TLBI
3.  DSB ISH                                       ; full barrier: order the
                                                  ;     store (1) BEFORE the
                                                  ;     snapshot load (4)
4.  snap = cpuset_snapshot(&space->active_cpus)   ; per-word atomic_load acquire
5.  TLBI VAE1IS, <VA | ASID>                      ; broadcast invalidate, inner
                                                  ;     shareable (VALE1IS for a
                                                  ;     last-level-only change)
6.  DSB ISH                                       ; wait for TLBI completion
                                                  ;     across the IS domain —
                                                  ;     this IS the shootdown ack
7.  ISB                                           ; A observes the new regime
```

On arm64 the broadcast `TLBI ...IS` + `DSB ISH` (steps 5–6) reaches **every**
inner-shareable PE in hardware, so the completion of the `DSB ISH` at step 6 is
itself the global acknowledgment; `snap` (step 4) is computed and accepted but
the broadcast makes targeting unnecessary, which preserves today's behavior
(Requirement 7). The snapshot is load-bearing for the future **x86 backend**,
where steps 5–6 instead become: local `invlpg`/PCID-invalidation, then an IPI to
every *other* CPU in `snap`, then **spin until each target acknowledges** it has
executed its local invalidation — the explicit remote-shootdown-acknowledgment
ordering the contract requires.

**Break-before-make.** When step 1 *changes* an already-valid leaf (as
`pmap_replace` and a permission narrowing do), arm64 requires the full
break-before-make dance to avoid a TLB conflict abort: write the **invalid**
descriptor first, `DSB ISHST`, `TLBI`, `DSB ISH`; then write the **new**
descriptor, `DSB ISHST`, `ISB`. `pmap_enter` into a guaranteed-empty slot is a
make-only transition and needs no TLBI (no prior entry to invalidate) — only
`DSB ISHST; ISB` to publish the new leaf.

**Why B cannot miss A's invalidation (ordering chain, not a hand-wave).**
Assume, for contradiction, that B fills its TLB from the **stale** descriptor
**and** A's snapshot (step 4) does not contain B.

- A's snapshot missing B ⇒ A's `atomic_load` (step 4) was ordered, by A's
  `DSB ISH` (step 3), **after** A's descriptor store (step 1), yet observed
  `active_cpus` without B's bit. Since B's bit, once set, is only cleared by B's
  own later deactivation, A's load preceded B's `fetch_or` (B-step 1) becoming
  observable to A.
- B's `fetch_or` (B-step 1) is ordered **before** B's `MSR TTBR0_EL1` by B's
  `DSB ISH` (B-step 2), and B's walk of the descriptor is ordered **after** the
  `MSR` by the `ISB` (B-step 3).

Composing: `A.store(desc)` observable → `A.load(snap)` → `B.fetch_or`
observable → `B.MSR TTBR0` → `B.walk(desc)`. Hence `B.walk` reads the descriptor
*after* `A.store(desc)` is observable, so B reads the **new** descriptor — not
the stale one. Contradiction. Therefore in every execution at least one holds:
A's snapshot contains B (B is shot down), or B's walker observes the new
descriptor. The two `DSB ISH` full barriers (A-step 3 and B-step 2) are exactly
what forbid both store→load reorderings simultaneously; this is the mechanized
form of the store-buffer impossibility, with the specific instructions named.

**Deactivator B — `vm_address_space_deactivate()`:**

```
1.  pmap_deactivate():
        MSR TTBR0_EL1, <kernel-only / zero root>
        ISB                                       ; stop using space's regime
2.      TLBI <ASID-scoped, or guaranteed by TTBR0 disable>, DSB ISH, ISB
                                                  ; B's TLB no longer holds
                                                  ;     space's entries
3.  DSB ISH                                       ; order the flush BEFORE the
                                                  ;     bit clear
4.  cpuset_remove(&space->active_cpus, B)         ; atomic_fetch_and, acq_rel
```

B's bit is cleared **only after** B can no longer hold or form a translation for
`space` (step 3 orders the local flush before the clear in step 4). So an
invalidator whose snapshot still contains B issues a harmless shootdown (B has
nothing cached); one whose snapshot no longer contains B is guaranteed B has
already flushed. There is no window in which B is absent from the set yet still
holds a live entry — the symmetric property to activation.

**Freeing vs. the set.** No page table or root of `space` may be freed while
`active_cpus` is non-empty; `vm_address_space_quiesce()` blocks until the set
drains (≈2 s bound) via per-word `cpuset_snapshot`, and `pmap_destroy` requires
it. This closes the use-after-free against a CPU whose walker is still reading
the tables, and composes with the activation ordering: a CPU cannot join
`active_cpus` and begin walking after quiesce has observed an empty set, because
joining requires an `active_cpus` store that a subsequent snapshot would see.

### Invalidation contract

Every mutating `pmap_*` call that **changes or removes an existing
translation** — `pmap_remove`, `pmap_protect`, `pmap_replace` — must guarantee
that, **before it returns**, no CPU in `active` can complete a translation
using the stale entry. `pmap_enter` installs into a slot the contract
guarantees was empty, so no stale entry for that VA existed and it carries no
cross-CPU shootdown obligation (it still issues whatever local barrier the
architecture needs to make the new entry usable; a CPU that took a spurious
not-present fault in the meantime simply retries and the core's "another CPU
already fixed it" logic absorbs it). How a mutation achieves global coherence
is the backend's business:

- **arm64 backend**: issue the broadcast `TLBI` for the VA (optionally
  ASID-scoped) followed by the required `DSB`/`ISB`. AArch64 inner-shareable
  broadcast reaches every CPU in hardware, so the `active` set is accepted but
  may be ignored. This matches today's behavior exactly (Requirement 7).
- **x86 backends (future)**: invalidate locally (`invlpg`, or a PCID-scoped
  invalidation) and then perform a **cross-CPU shootdown** — issue an IPI to
  each *other* CPU in `active` and wait for acknowledgment before returning.
  The IPI delivery primitive is generic kernel infrastructure (an
  `smp_call`-style cross-call); the flush instruction it runs is
  arch-specific. The neutral layer provides the cross-call; the backend
  provides the handler.

The core relies only on the **guarantee** ("coherent on return"), never on the
mechanism. It must therefore never assume invalidation is free: on x86 a
`pmap_remove` carries an IPI round-trip that the arm64 version does not, so
batching opportunities (e.g. ranged unmap) are a backend concern the contract
leaves room for but Phase 0 does not implement.

## Symbol migration (arm64 reference backend)

Partition of the 30 functions currently in `vm/address_space.c`. "Core" stays
in the neutral `address_space.c`; "Backend" moves to
`vm/arch/arm64/pmap.c`; "Split" keeps neutral policy in `address_space.c`
and delegates the descriptor/TLB step to a `pmap_*` call.

| Current symbol (`vm/address_space.c`) | Disposition | Lands as |
| --- | --- | --- |
| `vm_user_descriptor`, `vm_user_descriptor_protection` | Backend | descriptor build/parse (internal to `pmap.c`) |
| `vm_table_from_descriptor` | Backend | internal |
| `vm_address_space_translate`, `..._translate_read`, `..._translate_write` | Backend | `pmap_translate` + thin neutral wrappers |
| `vm_space_leave`, `vm_ttbr0_disable_local` | Backend | folded into `pmap_deactivate` |
| `vm_address_space_restrict_page` | Backend | `pmap_protect` |
| `vm_address_space_query_page` | Backend | `pmap_query` + neutral wrapper |
| `vm_address_space_flush_if_active` | Backend | internal to the invalidation path |
| `vm_address_space_create` | Split | neutral zeroes struct → `pmap_create` |
| `vm_address_space_activate` | Split | neutral `active_cpus` bookkeeping → `pmap_activate` |
| `vm_address_space_deactivate` | Split | neutral bookkeeping → `pmap_deactivate` |
| `vm_space_install_locked`, `..._map_page_locked`, `..._map_page` | Split | neutral region check + `resident_pages` → `pmap_enter` |
| `vm_address_space_unmap_page` | Split | `pmap_remove` → neutral `pmm_free_page`/count |
| `vm_address_space_cow_break` | Split | **neutral** refcount decision + `pmap_copy_page`/`pmap_replace` |
| `vm_address_space_release_pages` | Split | neutral iterates → `pmap_remove` per page + `pmm_free_page` |
| `vm_address_space_destroy` | Split | neutral `quiesce` → `pmap_destroy` |
| `vm_address_space_fork` | Split | backend `pmap_enumerate` (read-only walk) → **neutral** two-phase apply: `pmm_page_retain` + COW decision + `pmap_enter`(child) + `pmap_replace`(parent); rollback over the leaf array on failure |
| `vm_address_space_current`, `..._is_active`, `..._quiesce`, `vm_address_in_shm_window`, `vm_cpu_space` | Core | unchanged |
| `vm_address_space_root_physical`, `..._table_count` | Core | wrap `pmap_root_physical`/`pmap_table_count` |

The arm64 page-table internals currently spread across `vm/vmm_tables.c`
(table walk/allocate, descriptor helpers) move into `vm/arch/arm64/`
alongside `pmap.c` as backend-private translation units. `vm/vmm.c`,
`vm/vmm_ttbr1.c` (kernel higher-half / direct map) are **out of scope for Phase
0** (Requirement 6) and stay where they are, still arm64-only.

## Phase 0 implementation plan

Invariant: the build (which compiles the arm64 source set) **boots and passes
the existing VM selftests after every numbered step**. Behavior is unchanged
throughout (Requirement 7); Phase 0 is a pure refactor.

0. **Scope fence.** Only the user address-space path (`vm_address_space_*`) is
   touched. Kernel higher-half/direct-map (`vmm.c`, `vmm_ttbr1.c`) is untouched
   (Requirement 6).

1. **Add `vm/vm_types.h`** (`vm_prot_t`, `vm_user_page_mapping_t`) plus the
   temporary compatibility aliases to the two old enum names (Decision 6).
   Pure addition → green.

2. **Add the contract headers**: `vm/pmap.h`,
   `vm/machine/pmap_types.h`, `vm/arch/arm64/pmap_types.h`. `pmap.h`
   includes `vm_types.h` and `pmap_types.h` only — **not** `address_space.h`.
   Unused by anyone yet → green.

3. **Split the struct** in `vm/address_space.h`: replace `root`,
   `root_physical`, `table_count` with an embedded `pmap_t pmap` (the header now
   `#include`s `pmap_types.h` for the size). Mechanically rewrite `space->root`
   → `space->pmap.root` (etc.) — **this is the only step allowed to name
   `pmap_t` fields in files that will become neutral, and it is transitional**:
   those references all move into the backend in step 4, after which the
   opacity guard must pass. Zero behavior change → green.

4. **Create `vm/arch/arm64/pmap.c`** and move the "Backend" functions and
   the descriptor/table internals (from `vm/vmm_tables.c`) into it, taking every
   `space->pmap.<field>` reference with them; wire it into the build (add the
   new source, drop the moved code from `vm/address_space.c`). After this step
   **no neutral file names a `pmap_t` field** → opacity guard passes. Still the
   same machine code, relocated → green.

5. **Refactor the "Split" functions** to call the `pmap_*` contract. This is
   the commit that proves the seam. Order within the step: the single-leaf ops
   first (`create`/`activate`/`map`/`unmap`/`protect`), then `cow_break`, then
   `destroy`, then `fork` last (it depends on `pmap_enumerate` + `pmap_enter` +
   `pmap_replace` + `pmap_copy_page` all being in place). → green.

6. **Validate** (next section).

### Build wiring

In the arm64 `C_SOURCES`, `vm/address_space.c` stays (now neutral) and
`vm/arch/arm64/pmap.c` is added; the arm64 table internals move with it.
`vm/vmm.c`, `vmm_ttbr1.c`, `vmm_tables.c` stay listed (the last loses only the
functions that moved). The tree resolves through the existing `-I.` (repo root) include path; no new `-I` is needed
covered by the existing `-I` roots. The resulting object set is behaviorally
identical.

## Validation strategy

- **Build green after every step** on the arm64 target; no step may leave the
  tree uncompilable.
- **Boot test**: arm64 under QEMU `virt` to userspace.
- **VM selftests**: the existing arm64 VM/thread self-tests
  (`kern/arm64` + `vm` demand-paging/COW/fork paths) must pass unchanged. These
  are the behavioral oracle for Requirement 7.
- **Leak/refcount check**: after fork + COW-break + process exit, free page
  count returns to baseline (`pmm_get_free_page_count`), confirming the
  neutral/backend refcount split did not drop or double-free references.
- **Activation-race test**: a stress loop in which one CPU repeatedly
  activates/deactivates a shared space while others `pmap_remove`/`pmap_protect`
  the same VAs, with a checker that no CPU ever reads through a removed/narrowed
  mapping. On arm64 this largely exercises the hardware broadcast, but it is the
  regression guard that keeps the protocol correct as the x86 IPI backend
  arrives.
- **Fork-rollback test**: inject failure at *each* sub-step of the *k*-th leaf
  (retain, child-enter, and parent-replace — the last being the leaf-partial
  case the progress tracking fixes) and assert (a) the child is fully torn
  down, (b) the parent is semantically unchanged in mappings, protections, and
  physical-page ownership, and (c) the free-page count returns to the pre-fork
  baseline (no leaked or double-freed retains, including on the failing leaf).
- **Opacity guard (CI grep)**: assert that `TTBR`, `CR3`, `ASID`, `PCID`,
  `S1E1`, `EL0`, `EL1`, and backend PTE-bit macros do **not** appear in
  `vm/pmap.h`, `vm/vm_types.h`, `vm/vm_map.c`, `vm/vm_fault.c`,
  `vm/address_space.c`, or `vm/address_space.h`. This mechanizes Requirements 4
  and 5.
- **Symbol-location guard**: assert the tokens `.root`, `.root_physical`,
  `.table_count` and `space->pmap.` do not appear outside `vm/arch/`,
  mechanizing practical opacity (no `pmap_t` field named by neutral code).

## Risks

### Copy-on-write

The refcount decision (reclaim in place when `pmm_page_refcount == 1`, else
copy to a fresh page) is **neutral policy** and must stay in `address_space.c`;
only "write this descriptor" (`pmap_replace`) and "copy the bytes"
(`pmap_copy_page`) are backend. The risk is a torn split where the backend
makes a refcount assumption. Mitigation: the backend never calls `pmm_*`; it
only edits descriptors and copies bytes it is told to. A COW page is hardware
read-only, so any code writing through its physical address must break sharing
first — this invariant is unchanged and must be re-verified by the selftests.

### Fork protocol (two-phase, transactional)

Fork is the one operation that mutates **two** address spaces and must be
all-or-nothing. It is implemented in the neutral `vm_address_space_fork` as:

**Lock acquisition.** Both `parent->lock` and `child->lock` are held for the
entire fork, acquired in a **deterministic global order: ascending by the
address of the `vm_address_space_t`** (lock `min(&parent,&child)` first, then
`max`), released in reverse. This order is the single rule that prevents an
ABBA deadlock against any other two-space operation. In practice `child` is a
freshly created, not-yet-installed space no other CPU can reach, so its lock is
uncontended — but the ordering is specified unconditionally so the invariant
holds even if a future caller forks into a shared child. Holding `parent->lock`
across the whole fork is **required**, not just for the snapshot: it freezes the
parent so no concurrent parent fault can COW-break a page between the moment we
downgrade it and the moment we might have to restore it on rollback.

**Phase 1 — enumerate (read-only, backend).** The core sizes a `pmap_leaf_t`
buffer from `parent->resident_pages` (core-owned, stable under the held lock),
allocates it, and calls `pmap_enumerate(&parent->pmap, buf, cap, &n)`. If the
buffer allocation fails, fork returns false having mutated **nothing**. No
page-table or refcount state has changed yet.

**Phase 2 — apply (neutral), per leaf.** A single leaf's application is itself a
multi-step sequence that can fail *between* steps (notably when `pmap_enter`
cannot allocate a child table, or `pmap_replace` fails). An earlier draft rolled
back starting at `k-1` and so leaked the partially-applied failing leaf `k`
(e.g. a page retained and entered into the child, but the parent downgrade
failed). To close that bug, each leaf records **how far it got** in a per-leaf
progress value, and rollback undoes the failing leaf according to its own
progress *before* unwinding the completed ones.

Define per leaf `i` a progress state, stored in a scratch field of `buf[i]` (or
a parallel array), advanced monotonically as the sub-steps commit:

```
NONE      -> RETAINED        after pmm_page_retain(buf[i].pa) succeeds
RETAINED  -> CHILD_ENTERED   after pmap_enter(child, ...) succeeds
CHILD_ENTERED -> DONE        after the parent side is settled:
                               - private-writable: pmap_replace(parent, ... cow=true) succeeds
                               - read-only / exec / shm: no parent change, so
                                 CHILD_ENTERED advances straight to DONE
```

Apply for leaf `i`:

1. `pmm_page_retain(buf[i].pa)` → on success `progress[i] = RETAINED`. On
   failure, leaf `i` stays `NONE`; go to rollback with failing-leaf progress
   `NONE`.
2. Classify (neutral COW policy) and `pmap_enter` into the child:
   - private & writable (`prot == VM_PROT_RW`, not in the shm window):
     `pmap_enter(child, va, pa, VM_PROT_RO, cow=true, active_child)`;
   - read-only / executable: `pmap_enter(child, va, pa, prot, cow=false, …)`;
   - shm-window page: `pmap_enter(child, va, pa, VM_PROT_RW, cow=false, …)`.
   On success `progress[i] = CHILD_ENTERED`; on failure go to rollback with
   failing-leaf progress `RETAINED`.
3. Settle the parent side. Private-writable only: `pmap_replace(parent, va, pa,
   VM_PROT_RO, cow=true, active_parent)`; other classes change nothing on the
   parent. On success (or no-op) `progress[i] = DONE`; on `pmap_replace`
   failure go to rollback with failing-leaf progress `CHILD_ENTERED`.
4. Bump `child->resident_pages` (and `shared_pages` where applicable). This is
   pure in-memory accounting that cannot fail; doing it only at `DONE` keeps the
   counters consistent with the rolled-back state.

**Rollback** undoes the **failing leaf `k` first, by its recorded progress**,
then every fully `DONE` leaf `i = k-1 … 0` in reverse:

```
undo_leaf(i):                      # by progress[i]
    if progress >= CHILD_ENTERED:  pmap_remove(child, buf[i].va, active_child)
    if progress == DONE and leaf i was private-writable:
                                   pmap_replace(parent, buf[i].va, buf[i].pa,
                                                VM_PROT_RW, cow=false, active_parent)
    if progress >= RETAINED:       pmm_free_page(buf[i].pa)
    # progress == NONE: nothing to undo
```

Applied as: `undo_leaf(k)` with `k`'s partial progress, then `undo_leaf(i)` for
`i = k-1 … 0` (each of those is `DONE`). Note the failing leaf `k` is undone
with the **CHILD_ENTERED** rule when its `pmap_replace` failed — it removes the
child entry and frees the retain, but issues **no** parent `pmap_replace`,
because the parent was never downgraded for leaf `k`. This is exactly the leaf
the earlier `k-1` start missed.

The parent restore is legitimate because `parent->lock` is held for the whole
fork, so no other CPU observed the transient RO+COW downgrade of any leaf.

Then `pmap_destroy(&child->pmap)` frees any tables the child accumulated
(`child` is not active, so quiesce is trivially satisfied), the child struct is
re-zeroed, the leaf buffer is freed, and fork returns false. **Postcondition:
the parent is semantically unchanged in its mappings, protections, and
physical-page ownership relative to its pre-fork state, and the global
free-page count is unchanged** (every `pmm_page_retain` that succeeded is paired
with exactly one `pmm_free_page`, including on the failing leaf).

Risks this closes: (a) a page retained but not installed in both spaces (retain
and both installs are one per-leaf sequence, and rollback undoes exactly the
retains it added); (b) a child left half-built (destroyed on any failure); (c) a
parent left with stray COW downgrades (restored, safe because of the held
parent lock); (d) deadlock against a concurrent two-space op (address-ordered
locking).

Residual concern worth noting: the leaf buffer is O(`resident_pages`) memory
held transiently — for a very large address space this is a sizable
allocation. Chunked enumerate/apply would bound it, but chunking complicates
transactional rollback (earlier committed chunks would need their own undo
record); it is deliberately **out of scope for Phase 0** and listed as a future
optimization.

### Failure and rollback for single-leaf operations (Requirement: explicit
rollback on partial modification)

Each single-leaf `pmap_*` mutation is specified to be **atomic with respect to
visible state**: it either completes and is globally coherent, or it makes no
observable change.

- **`pmap_enter`** may need to allocate intermediate table levels before it can
  write the leaf. If a table allocation fails partway, the leaf is **not**
  written, so no mapping becomes visible and the op returns false. The empty
  intermediate tables it already linked are left in place: they are valid,
  counted in `table_count`, harmless (they map nothing), and reclaimed wholesale
  by `pmap_destroy`. The core treats a false return as "page not mapped" and
  frees the physical page it had pre-allocated — exactly today's `vm_fault_user`
  behavior. (Unwinding the intermediate tables instead is a legal backend choice
  but not required; leaving them is simpler and equally safe.)
- **`pmap_remove` / `pmap_protect` / `pmap_replace`** each act on exactly one
  already-present leaf. `pmap_remove` is a single invalidating store;
  `pmap_protect` and `pmap_replace` change a live entry and therefore use the
  break-before-make sequence (write-invalid → TLBI → write-new) from the
  protocol section. Break-before-make has a transient *invalid* window, but it
  is **not observable** as a torn state: all three run under `space->lock`, and
  the fault path also takes `space->lock`, so a concurrent access to the same VA
  on another CPU either faults-and-blocks on the lock (then re-reads the settled
  entry) or is resolved by the broadcast TLBI — never a half-written descriptor.
  There is thus no intermediate state the *core* must roll back: the call
  commits the new entry or, on a pre-write failure, leaves the old one intact.
  They fail (changing nothing) only when the target slot is absent — a caller
  bug the contract surfaces as `false`.
- **`pmap_copy_page`** that uses a transient mapping must tear that mapping down
  on every exit path, success or failure; a failed copy leaves the destination
  page's contents undefined but the caller discards the page (it has not been
  installed anywhere yet in the COW/fork flows that use it).

The core's multi-leaf operations (`release_pages`, fork) compose these atomic
primitives and own their own rollback (fork above; `release_pages` is
monotonic teardown that cannot meaningfully fail mid-way and simply continues).

### Active address spaces

Freeing a table or root while a CPU still has the pmap loaded is a
use-after-free of a page the hardware walks. The core must call
`vm_address_space_quiesce` (wait until `active_cpus` is empty, ~2 s bound)
**before** any `pmap_destroy`, and `pmap_destroy`'s precondition states this.
Risk: a backend that frees eagerly inside `pmap_remove` for the *active* space.
It must not — `pmap_remove` only unmaps and invalidates; structural frees
happen only in `pmap_destroy` after quiesce.

### Concurrent mapping operations

Multiple threads of one process fault on different CPUs, on different pages and
the same page, while others mmap/munmap/fork. The region-check-plus-install
atomicity under `space->lock` is what makes this correct, and it now straddles
the neutral/backend boundary (`vm_map_region_at_locked` neutral,
`pmap_enter` backend, both under the one lock). Risks: (a) a backend that drops
and re-takes `space->lock` internally (it must never take it at all on the
locked path); (b) lock-ordering inversion if a backend takes `pmm` before
`space->lock` (forbidden — see Synchronization). "Another CPU already fixed
this fault" must remain success, not a fault, exactly as `vm_fault_present`
treats it today.

### Page-table destruction

Covered partly above; additional risk is the **TLB** at teardown. On x86
(future), removing the last mappings of an exiting process still requires
shootdown to any CPU in `active` before tables are freed; quiesce guarantees
the set is empty for the *final* free, but per-page `pmap_remove` during
`release_pages` on a still-active space must still shoot down. On arm64 the
broadcast covers it. The contract ("coherent on return") makes this uniform;
the risk is a future x86 backend that frees a table before its shootdown IPIs
are acknowledged.

## Phase 1 (i386) and Phase 2 (x86_64) — sketch

Both phases are now implemented; see the as-built notes below for what the
reality refined. This sketch is kept as the original plan of record.

- **Phase 1 — i386.** Add `vm/arch/i386/{pmap_types.h,pmap.c}` from the
  existing bodies of `kern/i386/{pmap.c, vmm_i386.c, address_space_i386.c}`
  (which already provide ~19 of the needed functions). **Delete**
  `kern/i386/vm_fault.c` and the mapping logic in `vm_init.c`; i386 switches to
  the shared `vm/vm_fault.c`, `vm/vm_map.c`, `vm/address_space.c`. This is where
  i386 inherits fork/COW/mmap from the shared core — the largest single
  maturation of that port. i386 needs no cross-CPU shootdown beyond its own
  SMP model; `active` is honored accordingly.
- **Phase 2 — x86_64.** `vm/x86_64/` is already a separate backend; rename its
  entry points to `pmap_*`, move its `x86_vmm_state_t` fields (already
  `root`/`root_physical`/`nx_enabled`/table counts — ~90 % of `pmap_t`) into
  `vm/arch/x86_64/pmap_types.h`, and **delete its forked upper layer** so
  it too rides the shared `vm_fault.c`/`vm_map.c`. Its `X86_PTE_*` formats and
  `invlpg`/IPI shootdown stay backend-private.

## Deferred (explicitly out of scope)

- **Kernel higher-half / direct-map facade.** `vm/vmm.h`'s TTBR1 API versus
  x86's top-of-CR3 direct map needs a neutral `vm_kern`-level facade. Hardest,
  riskiest piece; a phase of its own, after all three user-path backends
  conform (Requirement 6 keeps it out of Phase 0).
- **`vm_object` layer.** Does not exist yet (mappings are anonymous zero-fill +
  COW + shm). Add only when file-backed/named memory is wanted; the unification
  does not require it.

---

## As-built notes (Phase 0, arm64)

Phase 0 is implemented, built and booted. Where the implementation refined the
plan above, the reality is recorded here so this document stays the record:

- **Backend location.** The arm64 backend is `vm/arch/arm64/pmap.c`,
  implementing `vm/pmap.h`. It reuses the shared low-level arm64 table
  helpers in `vm/vmm_internal.h` (`vmm_root_get_l3_entry`, `vmm_allocate_table`,
  `vmm_invalidate_page`, ...) rather than copying them, since those are also
  used by the out-of-scope kernel higher-half path. `vm/vmm_tables.c` therefore
  did **not** move; only the user address-space logic did.
- **`pmap_release_user_pages` was added to the contract.** Teardown
  (`vm_address_space_release_pages`) must free core-owned user pages *and*
  preserve the pre-split single bulk TLB flush. A per-leaf `pmap_remove` loop
  would have done one broadcast invalidation per page. The backend call takes
  the core's free function as a callback: the core owns the frees, the backend
  owns the walk and the one bulk flush. (Fork's parent downgrade, by contrast,
  does use per-page `pmap_replace`; that per-page-vs-bulk invalidation delta is
  the already-listed deferred "batched invalidation" optimisation.)
- **Compatibility aliases live in `vm/address_space.h`, not `vm_types.h`.**
  `vm_types.h` holds only the canonical `vm_prot_t`/`vm_user_page_mapping_t`;
  `address_space.h` adds `typedef vm_prot_t vm_user_protection_t;` and the
  `VM_USER_PROTECTION_*` defines. This keeps `vmm.h`'s separate
  `vmm_protection_t` untouched (Requirement 6) while the many user-path callers
  compile unchanged. Removal stays assigned to the higher-half facade phase.
- **One residual arch touchpoint in neutral code.** `vm_address_space_deactivate`
  still references `g_vmm.ttbr0_disabled` / `vmm_disable_ttbr0()` for the
  boot-CPU identity-map teardown. That is a kernel/boot (higher-half) concern,
  explicitly out of Phase 0 scope; it names no `pmap_t` field, so the opacity
  guard still holds. It moves with the higher-half facade.
- **Field-access guard must cover dot forms too.** The struct split's
  mechanical rename initially missed two non-arrow sites — a dot-designated
  initializer (`kern/process/task.c`: `.map = {.root = 0, ...}`) and dot-member
  accesses in a test (`kern/i386/vm_selftest.c`: `space_a.root_physical`). An
  arrow-only (`->field`) grep did not see them; the full build did. The
  symbol-location guard greps `\.root`, `\.root_physical`, `\.table_count` and
  `->pmap\.`, which covers both forms.
- **Opacity-guard note.** The arch-token guard finds `TTBR0`/`EL0` only in
  *doc comments* of the shared headers / neutral file, not in code. Those
  comments were reworded to neutral terms where they described the neutral API.

### Verification performed

- arm64 kernel builds clean (`-Werror`) and links.
- arm64 boots under QEMU to userspace: `kern_init: root userspace services
  active`, and `bootd` (PID 1) forks and starts `logd`, `patchd`,
  `problemreporter`, `windowserver` and the dock — exercising the two-phase
  fork and COW-on-first-write live.
- POST self-tests pass unchanged: `vm_map` (region split/shrink/remove, exact
  page-count deltas), `vm_shm` (page shared R/W across two address spaces),
  `ipc_space` (port transferred across spaces), `post_test_vm_kern`.
- i386 and x86_64 kernels still build and link (mechanical field rename only;
  their backends remain at their current paths pending Phases 1-2).

## As-built notes (Phase 1, i386)

Phase 1 is implemented, built and tested. i386 now rides the shared neutral
`vm/address_space.c` + `vm/vm_fault.c`; `kern/i386/address_space_i386.c` is
deleted and the PAE page-table logic lives in `vm/arch/i386/pmap.c`.

- **i386 gained fork and COW.** They were stubs returning false before. The
  backend adds `PTE_SOFTWARE_COW` (free PTE bit 10), and `pmap_enter` (cow),
  `pmap_replace`, `pmap_copy_page` and `pmap_enumerate` let the shared
  two-phase fork and `cow_break` work. `kern/i386/vm_fault.c` (the arch trap
  handler — *not* deleted; the doc's earlier "delete vm_fault.c" was wrong)
  now routes faults to the shared `vm_fault_user`, so a COW write-fault
  resolves and a genuine bad access still returns false and is handled as
  before.
- **The neutral layer became genuinely arch-neutral** to build for i386. Three
  arm64-isms were removed from `vm/address_space.c`: the `g_vmm` /
  `vmm_disable_ttbr0` boot touchpoint in `deactivate` (the boot identity map is
  torn down by `kern_init`, not here — confirmed it calls `vmm_disable_ttbr0`
  directly), an inline `dsb ish` in `quiesce` (now a portable
  `__atomic_thread_fence(SEQ_CST)` — `dmb ish` on arm64, `mfence` on x86), and
  the include of `vm/vmm_internal.h`.
- **Pre-scheduler boot (`current_processor() == 0`).** i386 runs its VM
  self-test before the scheduler registers the boot CPU, so the neutral
  `activate`/`deactivate`/`quiesce`/`vm_cpu_space` now handle a null processor
  via a `g_boot_active_space` fallback (mirroring i386's old code). This is a
  generic early-boot accommodation; arm64 never hits it (its self-test runs
  after registration) so its behaviour is unchanged. A missed guard here
  (`self->active_space` / `self->cpu_id` in `quiesce`) caused a null-deref panic
  on the first i386 test run; `make test-i386-vm` caught it immediately.
- **i386 TLB is still local-only** (`invlpg`, no cross-CPU shootdown), matching
  the port's prior behaviour; the `active` cpuset is accepted but not yet used.
  SMP shootdown on i386 is a later improvement.

### Verification performed (Phase 1)

- i386 builds and links against the shared core; `make test-i386-vm` passes all
  11 cases, including `fault-write-ro`, `fault-user-ro`, `fault-unmapped`,
  `fault-null` and the rest, which exercise the new shared resolver.
- arm64 rebuilt, boots to userspace and passes `vm_map` / `vm_shm` /
  `ipc_space` after every shared-layer change (the neutral reconciliation and
  the `quiesce` fix are behaviour-identical there).

## Subsystem relocation (kern/vm → vm)

The contract and backends were first created under `kern/vm/`; they were later
moved wholesale to the top-level `vm/` tree so the entire VM subsystem — neutral
core, contract and per-arch backends — lives under one directory:

    vm/pmap.h                vm/vm_types.h            vm/machine/pmap_types.h
    vm/arch/<arch>/pmap.c    vm/arch/<arch>/pmap_types.h

Pure relocation: every `#include`, build manifest and doc reference was
rewritten `kern/vm/ → vm/`; the tree resolves through the existing `-I.` include
path. arm64 stayed byte-identical and i386 kept passing, confirming no behaviour
change.

## As-built notes (Phase 2, x86_64)

Phase 2 is implemented, built and linked. x86_64 now rides the shared neutral
`vm/address_space.c`; its forked upper layer `vm/x86_64/address_space.c` is
**deleted** and the four-level (PML4) page-table logic lives in
`vm/arch/x86_64/pmap.c`.

- **Backend location and reuse.** The x86_64 backend is `vm/arch/x86_64/pmap.c`,
  implementing `vm/pmap.h` against the existing `x86_vmm_*` helpers in
  `vm/x86_64/vmm_internal.h` (`x86_vmm_root_get_l3_entry`, `x86_vmm_allocate_table`,
  `x86_vmm_make_user_descriptor`, `x86_vmm_table_from_physical`, `g_x86_vmm`). As
  on arm64, the low-level table helpers and the kernel higher-half/direct-map
  code in `vm/x86_64/vmm.c` did **not** move; only the user address-space logic
  did. `pmap_copy_page` uses the permanent x86-64 direct map
  (`vmm_physical_to_higher_half`); `pmap_release_user_pages` does one bulk flush
  by reloading CR3.
- **No per-pmap NX/PCID state.** `pmap_t` stays `{root, root_physical,
  table_count}` — identical to arm64/i386. NX is a mode-wide property
  (`g_x86_vmm.nx_enabled`) and the kernel-half PML4 branches come from
  `g_x86_vmm.root`, so nothing arch-specific beyond those three fields is needed.
  `pmap_query`/`pmap_enumerate` report a COW page as hardware read-only with a
  separate `cow` flag (matching i386), not as the old x86_64 "cow ⇒ read-write"
  quirk, so the shared fork/`cow_break` policy drives it.
- **Scheduler seam `vm/vm_cpu.h`.** The shared core previously `#include`d
  `<kern/sched_prism/processor.h>` directly for `current_processor()` and a
  processor's `active_space`/`cpu_id`. That header embeds `run_queue_t`/`thread_t`
  by value and its x86_64 thread/GDT backing is an explicit `#error`
  ("not implemented yet"), so the core could not compile for x86_64 at all. The
  core now reaches those four operations through `vm/vm_cpu.h`: on the
  scheduler-bearing ports (arm64, i386) they are inline accessors straight onto
  `struct processor`, so the generated code — and the arm64 `kernel.bin` — is
  **byte-identical**; on x86_64 (no scheduler yet) `vm_current_cpu()` is a
  constant null, so the core runs entirely on its `g_boot_active_space` path and
  the per-CPU accessors are inline no-ops. This decouples the VM core from the
  scheduler's concrete layout — a port may now run the shared VM core before, or
  without, sched_prism.
- **Two single-CPU providers the nascent port lacked.** The shared core also
  references `timer_get_microseconds()` (added to `kern/x86_64/timer.c`, built on
  the existing TSC `timer_get_ticks`/`timer_get_frequency`; returns 0 until the
  TSC is calibrated, which is safe because quiesce returns on the empty CPU set
  first) and `machine_ipi_raise()` (new `kern/x86_64/smp.c`, a no-op on this
  one-CPU port — the only caller skips the calling CPU, and there is no other).
  Both must be implemented for real when x86_64 gains the APIC and the scheduler.
- **x86_64 TLB is local-only** (`invlpg`, or a CR3 reload for teardown), which is
  globally coherent on one CPU; the `active` cpuset is accepted but not yet used,
  matching i386. Cross-CPU shootdown is a later improvement.

### Verification performed (Phase 2)

- x86_64 builds and links against the shared core + new backend (`-Werror`);
  the forked `vm/x86_64/address_space.c` is gone. (This port has no QEMU boot
  path yet — the UEFI loader toolchain is not wired — so build+link is the
  oracle, as for the earlier x86_64 milestones.)
- arm64 rebuilt **byte-identical** (`kernel.bin` 2740224 bytes) after the seam
  refactor — the inline accessors compile to the same code as the former direct
  field access, so ARM64 is unchanged in both meaning and bits.
- i386 rebuilt against the reworked core and `make test-i386-vm` still passes all
  11 cases.

## Resolved decisions (approved)

All six prior open questions are now settled and baked into the contract above:

1. **Neutral-core location** — option **(a)**: the neutral core stays at
   top-level `vm/` for Phase 0; only `pmap.h`, `pmap_types.h` and the backends
   live under `vm/`. No subsystem relocation.
2. **Backend arguments** — `pmap_t *` + explicit `const nxu_cpuset_t *active`.
   The owning `vm_address_space_t` is never passed into a backend op.
3. **Fork** — **two-phase enumerate/apply** (`pmap_enumerate` + neutral apply),
   chosen for its deterministic, list-driven rollback; the callback form is
   rejected. Locking, ordering and rollback are specified in *Fork protocol*.
4. **COW copy** — `pmap_copy_page(dst_pa, src_pa)` stays, and backends **may**
   satisfy it with transient temporary mappings; a permanent direct map is not
   required.
5. **Mapping semantics** — `pmap_enter` **rejects** an occupied slot;
   `pmap_replace` handles existing mappings and **carries the `cow` state**
   explicitly.
6. **Protection aliases** — temporary compatibility aliases are kept for all of
   Phase 0; their removal is assigned to the future higher-half / direct-map
   facade migration (the phase that rewrites `vmm.*`).

## Remaining open items

None block Phase 0. Two minor items are **deferred by decision**, recorded so
they are not silently forgotten:

- **Fork leaf-buffer bounding.** The transactional apply holds an
  O(`resident_pages`) leaf buffer. Chunked enumerate/apply would bound the
  memory but needs a cross-chunk rollback record; deferred as a post-Phase-0
  optimization (see *Fork protocol*).
- **Ranged/batched invalidation.** The contract permits a backend to batch TLB
  maintenance across a multi-page operation (relevant to the future x86 IPI
  backend, where per-page shootdown is expensive). Phase 0 implements only the
  per-leaf form; the batched form is a future backend-local optimization that
  needs no contract change.

If either should instead be resolved before implementation, say so; otherwise
Phase 0 proceeds as the sequence above specifies.
