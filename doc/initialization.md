# Kernel Initialization Sequence

This document describes the bring-up sequence implemented by `kern_init()`,
phase by phase. For the responsibilities of the `kern_init` translation unit
itself — as opposed to the sequence it drives — see
[Core kernel initialization](kern/initialization.md).

The order below is taken directly from
[`kern/kern_init.c`](../kern/kern_init.c).

## Failure model

Every phase reports success as a `bool`. There is no error propagation, no
recovery and no `panic()` helper. A failed phase prints one diagnostic line to
the UART and enters:

```c
for (;;) {
	__asm__ volatile("wfe");
}
```

`wfe` parks the core in a low-power state until an event arrives; the
surrounding infinite loop means it never leaves. Interrupts are still masked at
every point where this can happen, so nothing can wake the kernel back into
useful execution. The practical contract is: **initialization either completes
entirely or the machine stops with a printed reason.**

## Phase summary

| # | Phase | Function | On failure |
| --- | --- | --- | --- |
| 0 | `CurrentEL` capture | `arm_read_current_el_raw()` / `arm_read_current_el()` | cannot fail |
| 1 | Early kernel-console message | `kputln()` | cannot fail |
| 2 | DTB validation | `dtb_init()` | halt |
| 3 | DTB structure walk | `dtb_dump()` | halt |
| 4 | Platform discovery | `platform_discover()` | halt |
| 5 | Physical memory manager | `pmm_init()` | halt |
| 6 | PMM self-test | `pmm_allocate_page()` / `pmm_free_page()` | halt |
| 7 | Exception vectors | `exception_init()` | cannot fail |
| 8 | Virtual memory manager | `vmm_init()` | halt |
| 9 | Kernel permission validation | `vmm_validate_kernel_permissions()` | halt |
| 10 | Stack and UART translation checks | `vmm_translate()` | halt |
| 11 | Cache enablement | `cache_init()` | halt |
| 12 | Live mapping self-test | `kern_test_live_vmm()` | halt |
| 13 | Kernel virtual arena | `vm_kern_init()` | halt |
| 14 | Arena self-test | `kern_test_vm_kern()` | halt |
| 15 | Kernel heap | `heap_init()` | halt |
| 16 | Heap self-tests | `kmalloc` / `kcalloc` / `kfree` | halt |
| 17 | GICv3 | `gic_init()` | cannot fail |
| 18 | Timer PPI | `gic_enable_ppi(30, 0x80)` | silently ignored if out of range |
| 19 | Periodic timer | `timer_start_periodic(100)` | silently returns |
| 20 | IRQ unmasking | `arm64_enable_irqs()` | cannot fail |
| 21 | Idle loop | `wfi` | never exits |

Phases 6, 12, 14 and 16 are boot-time validation rather than bring-up. They are
described in [Core kernel initialization](kern/initialization.md).

---

## Phase 0 — `CurrentEL` capture

**Preconditions.** None. This is the first thing `kern_init()` does.

**Work.** `arm_read_current_el_raw()` reads `CurrentEL`; `arm_read_current_el()`
extracts bits [3:2].

**State established.** Two locals. Nothing global changes.

**Why here.** The values are captured before any other subsystem can perturb
system state, but they are not *printed* until phase 6a, after the PMM. This is
purely a reporting decision; nothing depends on the ordering.

**Failure.** Cannot fail. NXU does not check that the result is EL1 and has no
code path for any other exception level.

## Phase 1 — Early UART

**Preconditions.** None. The PL011 at `0x09000000` is live from reset on the
QEMU `virt` machine, and the MMU is off, so the hard-coded MMIO address is the
physical address.

**Work.** Prints `kern_init: uart initialized`, then dumps the first four bytes
at the DTB address supplied in `x0`.

**State established.** A working console for every subsequent phase's
diagnostics. Every later failure path depends on this working.

**Why first.** Nothing can be debugged without output, and every later phase can
fail.

**Failure.** Cannot fail. There is no probe, no configuration and no readback —
if the address is wrong, the kernel writes into nothing and produces silence.

## Phase 2 — Device Tree validation

**Preconditions.** `x0` holds a plausible DTB address; the UART works.

**Work.** `dtb_init()` reads the 40-byte FDT header, converts every field from
big-endian, checks the magic against `0xD00DFEED`, checks that the structure,
strings and memory-reservation blocks lie inside `total_size`, and checks the
required alignments. On success it fills a stack-resident `dtb_t` with pointers
into the blob. The header fields are then printed.

**State established.** `device_tree` — a validated, bounds-checked view of the
blob. Note that it is a **local** of `kern_init()`, so its lifetime is the
lifetime of the kernel's initial stack frame, which never unwinds.

**Why here.** Everything the kernel knows about the machine comes from here.

**Failure.** `dtb: invalid Device Tree Blob`, then halt.

## Phase 3 — Structure block walk

**Preconditions.** Phase 2 succeeded.

**Work.** `dtb_dump()` walks every token in the structure block through
`dtb_walk()`, printing each node and property with its depth and value length,
and finishing with a node/property count.

**State established.** None. This phase exists to prove the walker handles the
real blob and to make the machine's device list visible in the boot log.

**Why here.** It validates the walker before `platform_discover()` depends on
it, and a malformed tree is caught here with a clear message rather than as a
confusing discovery failure.

**Failure.** `dtb: malformed structure block`, then halt.

## Phase 4 — Platform discovery

**Preconditions.** Phase 2 succeeded.

**Work.** `platform_discover()` performs a second `dtb_walk()`, this time with a
visitor that tracks `#address-cells`/`#size-cells` inheritance and matches
`compatible` and `device_type` strings. It fills a `platform_t` with RAM
regions, the UART frame, the GIC distributor and redistributor frames, the PCI
ECAM window and bus range, and every VirtIO-MMIO transport. It then requires
that at least one RAM region, the UART, the distributor and the redistributor
were all found.

**State established.** `platform` — again a local of `kern_init()`, passed by
pointer to `pmm_init()` and `vmm_init()`.

**Why here.** The PMM needs the RAM base and size; the VMM needs every MMIO
frame to map. Neither can run before this.

**Failure.** `platform: hardware discovery failed`, then halt.

## Phase 5 — Physical memory manager

**Preconditions.** Phase 4 succeeded; `__kernel_end` is known from the linker.

**Work.** `pmm_init()` takes `platform->memory_regions[0]`, aligns it inward to
page boundaries, computes the page count and the bitmap size, places the bitmap
on the first page-aligned address above `__kernel_end` (stepping over the DTB if
they would overlap), zeroes it, and then reserves three ranges permanently: from
the RAM base through `__kernel_end`, the bitmap storage itself, and the DTB.
Bits past the last real page are marked used so they can never be handed out.

**State established.** A working page allocator. From here on,
`pmm_allocate_page()` returns zeroed 4 KiB pages.

**Why here.** The VMM's very first action is to allocate its root translation
table, which requires the PMM. The PMM in turn requires platform discovery for
the RAM range and the DTB bounds.

**Failure.** `pmm: initialization failed`, then halt.

## Phase 6 — PMM self-test and EL report

Three pages are allocated, printed and freed. Then the `CurrentEL` values
captured in phase 0 are printed. Failure of either allocation or free halts.

## Phase 7 — Exception vectors

**Preconditions.** The vector table is linked at a 2048-byte-aligned address.

**Work.** `exception_init()` writes `VBAR_EL1` with the address of
`exception_vectors` and issues an `isb`.

**State established.** Exceptions now have a defined destination. Note that
IRQs remain masked; what this phase actually buys is that a *synchronous* fault
from this point on produces the NXU panic report instead of undefined
behavior.

**Why here — and why not earlier.** This is the ordering decision most worth
noting. The vectors are installed immediately *before* `vmm_init()`, which is
the first phase that can plausibly fault: it rewrites `TTBR0_EL1`, `TCR_EL1`,
`MAIR_EL1` and `SCTLR_EL1.M`, and a mistake there produces an instruction or
data abort. Installing the vectors first means such a mistake prints a
diagnosable panic rather than hanging.

**Failure.** Cannot fail.

## Phase 8 — Virtual memory manager

**Preconditions.** Phases 5 and 7 succeeded.

**Work.** `vmm_init()` checks `ID_AA64MMFR0_EL1.TGran4`, derives the physical
address width from `ID_AA64MMFR0_EL1.PARange`, allocates the L1 root table, and
builds an identity map covering:

- the RAM region containing the kernel, split by linker section into `r-x`,
  `r--` and `rw-` ranges, with all remaining RAM `rw-`;
- any other RAM region, `rw-`;
- the UART, GIC distributor, GIC redistributor and PCI ECAM as Device memory;
- every discovered VirtIO-MMIO window as Device memory.

It then programs `MAIR_EL1`, `TCR_EL1` and `TTBR0_EL1`, invalidates the TLB,
and sets `SCTLR_EL1.M` and `SCTLR_EL1.WXN`.

**State established.** Stage-1 translation is live. Because the map is an
identity map, every pointer that was valid before remains valid.

**Why the ordering matters inside this phase.** The register writes are one
inline assembly block ending in `isb`, so the MMU is enabled only after all four
control registers are in place and the TLB has been flushed.

**Verification.** The message `vmm: stage-1 MMU enabled` printing at all proves
that the running code, the stack, kernel globals and the UART are correctly
mapped — reaching the `kputln()` call requires all four.

**Failure.** `vmm: initialization failed`, then halt. A failure *inside* the
enable sequence would instead surface as a panic through the vectors installed
in phase 7.

## Phase 9 — Kernel permission validation

**Preconditions.** Phase 8 succeeded.

**Work.** `vmm_validate_kernel_permissions()` uses the `AT S1E1R` and
`AT S1E1W` address-translation instructions and reads `PAR_EL1` to confirm, for
each linker section, that reads succeed and translate to the identity address,
and that writes to `.text` and `.rodata` *fail*.

**State established.** None. This is a hardware-level assertion that the
descriptors carry the intended access permissions.

**Why here.** It must run after the MMU is enabled, because `AT S1E1*` uses the
live translation regime. It runs before caches are enabled so that a permission
error is diagnosed in the simplest possible machine state.

**Failure.** `vmm: kernel permission validation failed`, then halt.

Two further checks follow: that the current stack address and the discovered
UART base both translate to themselves.

## Phase 10 — Cache enablement

**Preconditions.** `SCTLR_EL1.M` is set — `cache_init()` explicitly refuses to
run otherwise.

**Work.** `cache_init()` reads `CLIDR_EL1` and `CTR_EL0`, derives the
instruction and data cache line sizes, verifies the legacy `CCSIDR_EL1` format
via `ID_AA64MMFR2_EL1.CCIDX`, invalidates every data and unified cache level by
set and way, invalidates the instruction cache, and then sets `SCTLR_EL1.C` and
`SCTLR_EL1.I`. It reads `SCTLR_EL1` back to confirm.

**Why the MMU must come first.** Cache behavior is determined by the memory
attributes in the translation tables. Enabling the caches before Normal memory
is marked cacheable and MMIO is marked Device would either be ineffective or
would allow the UART and GIC to be cached, which is incorrect.

**Why invalidation must come before enabling.** Cache contents at reset are
architecturally unknown. Enabling a cache that contains stale lines lets those
lines satisfy loads. `cache_invalidate_data_all()` therefore refuses to run if
`SCTLR_EL1.C` is already set, because invalidating a live data cache would
discard dirty lines.

**Failure.** `cache: initialization failed`, then halt.

## Phase 11 — Live mapping self-test

`kern_test_live_vmm()` exercises `vmm_map_page()`, `vmm_query_page()`,
`vmm_translate()`, `vmm_protect_page()`, `vmm_translate_write()` and
`vmm_unmap_page()` against one PMM page at `0x1000000000`. Failure halts.

## Phase 12 — Kernel virtual arena

**Preconditions.** `vmm_is_enabled()` must return true; `vm_kern_init()` refuses
to run otherwise, and refuses to run twice.

**Work.** Zeroes the arena bitmap and allocation-record table and marks the
arena initialized. No pages are mapped and no memory is consumed.

**State established.** A 64 MiB reserved virtual range at `0x1000000000` from
which page runs can be allocated.

**Why here.** It needs live page mapping (phase 8) because every allocation
calls `vmm_map_page()`. It must precede the heap, which uses it for large
allocations.

**Failure.** `vm_kern: initialization failed`, then halt.

Followed by `kern_test_vm_kern()`, which validates allocation, mapping
attributes, physical aliasing, release and first-fit reuse.

## Phase 13 — Kernel heap

**Preconditions.** The PMM works (small arenas) and `vm_kern` is initialized
(large allocations).

**Work.** `heap_init()` allocates one physical page from the PMM, writes the
page header and one free block covering the remainder, and records it as the
head of the arena list.

**State established.** `kmalloc()`, `kcalloc()` and `kfree()` are usable.

**Failure.** `heap: initialization failed`, then halt.

Followed by two groups of self-tests: small allocation, free, free-block reuse;
then a multi-page allocation with per-page content verification, free, and a
double-free rejection check.

## Phase 14 — GICv3

**Preconditions.** The GIC MMIO frames are mapped as Device memory by phase 8.

**Work.** `gic_init()` runs three steps in a fixed order:

1. **Distributor.** Write `GICD_CTLR = 0`, wait for `RWP`, then set
   `ARE_NS | EnableGrp1`, wait for `RWP` again.
2. **Redistributor.** Clear `GICR_WAKER.ProcessorSleep`, `dsb sy`, then poll
   until `GICR_WAKER.ChildrenAsleep` clears.
3. **CPU interface.** Set `ICC_SRE_EL1.SRE`, clear `ICC_CTLR_EL1.EOImode`, set
   `ICC_PMR_EL1 = 0xFF`, `ICC_BPR1_EL1 = 0`, and `ICC_IGRPEN1_EL1 = 1`.

**Why this order.** The routing model must be configured before the
redistributor is woken, and the redistributor must be awake before its CPU
interface is meaningful. `ICC_SRE_EL1.SRE` must be set before any other `ICC_*`
register is written, because the system-register interface is otherwise not
architecturally accessible.

**Failure.** `gic_init()` returns `void` and cannot report failure. The
redistributor wake step polls without a timeout: if the hardware never
acknowledges, the kernel spins here forever with no message.

## Phase 15 — Timer PPI and periodic timer

`gic_enable_ppi(30, 0x80)` places INTID 30 into Group 1, sets its priority to
`0x80`, configures it level-sensitive, clears any pending state and enables
forwarding. `timer_start_periodic(100)` divides `CNTFRQ_EL0` by 100, writes
`CNTP_TVAL_EL0` and sets `CNTP_CTL_EL0 = 1`.

The timer is armed here, but the CPU still has `PSTATE.I` set, so the interrupt
merely becomes pending rather than being taken.

Both functions return `void`. `gic_enable_ppi()` silently ignores an INTID
outside 16-31, and `timer_start_periodic()` silently returns if either the
requested rate or `CNTFRQ_EL0` is zero.

## Phase 16 — IRQ unmasking

`arm64_enable_irqs()` clears `PSTATE.I` with `msr DAIFClr, #2` followed by
`isb`. Nothing else is unmasked.

**Why last.** This is the point where control flow becomes asynchronous. Doing
it earlier would let a timer IRQ arrive while `VBAR_EL1` was unset, or while the
GIC CPU interface was half-configured, or while the heap was mid-allocation. By
placing it after every other phase, NXU guarantees that all initialization ran
in a strictly sequential, interrupt-free context — which is precisely why no
subsystem needs a lock.

## Phase 17 — Idle loop

```c
for (;;) {
	uint64_t seconds = timer_get_interrupt_count() / KERNEL_TIMER_HZ;
	if (seconds != last_second) { /* print uptime */ }
	__asm__ volatile("wfi");
}
```

`wfi` halts the core until an interrupt is pending. The timer IRQ wakes it,
the vector entry runs, `timer_handle_interrupt()` rearms the timer and
increments the counter, `ERET` returns to the instruction after `wfi`, and the
loop re-evaluates. `kern_init()` never returns.

## Source files

- [`kern/kern_init.c`](../kern/kern_init.c)
- [`mach/arm64/start.S`](../mach/arm64/start.S)

## Related documentation

- [Boot](boot.md)
- [Core kernel initialization](kern/initialization.md)
- [Physical memory](vm/physical-memory.md)
- [Virtual memory](vm/virtual-memory.md)
- [Kernel virtual arena](vm/kernel-virtual-arena.md)
- [Heap](kern/heap.md)
- [Interrupt controller](arm64/interrupt-controller.md)
- [Timer](arm64/timer.md)
- [Cache](arm64/cache.md)

## Current limitations

- No error propagation. Every failure is terminal.
- `platform_t` and `dtb_t` live on `kern_init()`'s stack frame. They remain
  valid only because that frame never unwinds; no subsystem retains a copy.
- `gic_init()` and `timer_start_periodic()` cannot report failure to the caller.
- The redistributor wake loop and the distributor `RWP` loop have no timeout.
- The initialization sequence is a hard-coded call list. There is no
  registration or dependency mechanism.
