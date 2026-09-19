# Core Kernel Initialization (`kern_init`)

`kern/kern_init.c` is the single translation unit that turns a booted
AArch64 core into a running kernel. This document covers its responsibilities
and implementation. The phase-by-phase description of the sequence it drives is
in [Initialization](../initialization.md); the two are deliberately not
duplicated.

## Role

`kern_init()` is the architecture-to-kernel transition point. Above it is
assembly that knows only about registers and memory; below it is C that knows
about subsystems. It is called exactly once, from `_start`, and never returns.

It is the only function in NXU that knows the full initialization order.

## Responsibilities

- Receive the DTB address that `_start` preserved in `x0`.
- Call each subsystem's initializer in the one order that satisfies every
  dependency.
- Check each result and halt with a printed reason on failure.
- Run boot-time validation between phases.
- Establish the machine's steady state: vectors installed, MMU on, caches on,
  allocators live, timer ticking, IRQs unmasked.
- Enter the idle loop.

## Non-responsibilities

- **Implementing any subsystem.** Every phase is a call into `platform`,
  `vm`, `mach/arm64` or the heap.
- **Deciding machine addresses.** Those come from `platform_t`.
- **Handling interrupts.** IRQs are dispatched by
  [`exception.c`](../../mach/arm64/exception.c).
- **Machine context switching.** The scheduler core exists, but `kern_init()` still uses the bootstrap EL0 bridge until AArch64 thread context switching is wired.
- **Providing a panic path.** Fatal exceptions are handled by
  `exception_handle()`. `kern_init()`'s halt loops are a different, simpler
  mechanism.

## Signature and lifetime

```c
void kern_init(const void *dtb_address);
```

`dtb_address` is the value QEMU placed in `x0`. It is `const void *` because
NXU only reads the blob.

Two important objects are **locals** of `kern_init()`:

```c
dtb_t      device_tree;
platform_t platform;
```

`platform` is roughly 800 bytes, dominated by the 32-entry VirtIO array, and
sits on the 16 KiB boot stack. Both are passed by pointer to `pmm_init()` and
`vmm_init()`, and neither callee retains the pointer beyond its own execution.

This works only because `kern_init()` never returns, so its frame never unwinds.
It is a real constraint on future code: any subsystem that wants platform
information after initialization must copy what it needs, because there is no
global `platform_t`.

## Why failure paths halt with `wfe`

Every failure is handled identically:

```c
uart_putln("<subsystem>: <what failed>");

for (;;) {
	__asm__ volatile("wfe");
}
```

The design choices behind this:

- **There is nothing to fall back to.** If the PMM cannot initialize, no
  allocation is possible. If the MMU cannot be enabled, no protection exists.
  Continuing would produce misleading behavior rather than a diagnosable stop.
- **The message is the diagnostic.** The UART is brought up first precisely so
  every later failure can say something before stopping.
- **`wfe` rather than a spin.** `wfe` parks the core in a low-power state until
  an event arrives. Because interrupts are masked at every point where a failure
  can occur, no event can resume useful execution, and the loop around it means
  a spurious wake-up simply re-enters `wfe`.
- **`wfe` rather than `wfi`.** The idle loop at the end uses `wfi` because it is
  *waiting for* an interrupt. The failure loops use `wfe` because they are not
  waiting for anything.

Because this pattern is repeated inline about fifteen times, adding a real
`panic()` helper — and routing it through the same report that
`exception_handle()` produces — is an obvious future consolidation.

## Boot-time validation

`kern_init()` contains four groups of self-tests. They are **development
validation**, not part of any interface, and their output is not a stable ABI.
They are expected to move into a dedicated test subsystem.

| Test | Implementation | Validates |
| --- | --- | --- |
| PMM allocate/free | inline, phase 6 | Three allocations return distinct pages and can be freed |
| Live VMM | `kern_test_live_vmm()` | Map, query, translate, protect, unmap of one page |
| Kernel virtual arena | `kern_test_vm_kern()` | Multi-page allocation, mapping attributes, physical aliasing, first-fit reuse |
| Heap | inline, phases 16a and 16b | Small allocation, free-block reuse, multi-page allocation with content verification, double-free rejection |

Two of these are worth describing, because what they prove is not obvious.

### `kern_test_live_vmm()`

Allocates one physical page and maps it at `0x1000000000` — an address chosen to
be far outside the identity map so a stale mapping cannot mask a bug. It then
proves, in order:

1. `vmm_query_page()` reports the expected physical address, memory type and
   protection.
2. `vmm_translate()` agrees with the descriptor, so the hardware walker sees
   what the software walker sees.
3. A write through the new virtual address is observable at the physical
   address. This is the actual proof that the mapping works, and it relies on
   the identity map making the physical address directly readable.
4. After `vmm_protect_page(READ_ONLY)`, `vmm_translate_write()` **fails** while
   `vmm_translate()` still succeeds — the permission change reached the
   hardware, not just the descriptor.
5. After `vmm_unmap_page()`, `vmm_translate()` fails, proving the TLB
   invalidation took effect.

Its cleanup path is deliberately asymmetric: if the page is still mapped and
unmapping fails, the function returns `false` **without** freeing the physical
page. Returning a still-mapped page to the PMM would let it be handed out twice.
Leaking one page is the safer failure.

### `kern_test_vm_kern()`

Allocates a range spanning three pages, verifies each page's mapping attributes
and physical aliasing individually, allocates a second range, frees the first,
then allocates a same-sized range again and asserts it lands at the **same
address**. That last check is what proves the allocator is genuinely first-fit
and that `vm_kern_free()` returned the pages to the bitmap.

It tracks three independent "active" flags so its cleanup path releases exactly
what it allocated, and distinguishes test failure from cleanup failure by
returning `passed && cleanup_passed`.

## Why IRQs are unmasked last

`arm64_enable_irqs()` is the second-to-last statement in `kern_init()`, and its
position is load-bearing.

Until that call, control flow is strictly sequential. After it, a timer IRQ can
arrive between any two instructions. Every ordering hazard that would otherwise
need a lock is eliminated by making initialization run entirely in an
interrupt-free context:

- The vector table is installed, so an IRQ has somewhere to go.
- The GIC distributor, redistributor and CPU interface are fully configured, so
  an acknowledged interrupt is real.
- The timer PPI is enabled and the timer armed, so INTID 30 is expected.
- The PMM, VMM, arena and heap are all fully initialized, so the handler cannot
  observe half-built state.

It also means that the absence of locking in every NXU subsystem is a
consequence of this ordering, not an independent property. Introducing scheduler-driven concurrent kernel execution, a second core, or any interrupt handler that allocates would invalidate it. The scheduler now context-switches and can preempt EL0. General EL1 kernel preemption remains disabled, so boot-time kernel critical sections are not asynchronously switched underneath their locks.

## Idle loop

```c
uint64_t last_second = 0;

for (;;) {
	uint64_t interrupt_count = timer_get_interrupt_count();
	uint64_t seconds = interrupt_count / KERNEL_TIMER_HZ;

	if (seconds != last_second) {
		last_second = seconds;
		/* print uptime */
	}

	__asm__ volatile("wfi");
}
```

`wfi` halts the core until an interrupt is pending. The timer IRQ wakes it, the
vector entry runs, `timer_handle_interrupt()` rearms the timer and increments
the counter, `eret` returns to the instruction after `wfi`, and the loop
re-evaluates.

The physical timer continues at 100 Hz after scheduler bring-up. PID 1 is entered through the scheduler, and the deterministic EL0 preemption test returns execution to the bootstrap kernel thread after the userspace process exits. The bootstrap and idle threads remain the permanent CPU-0 kernel threads after the init test is reaped.

## Constants

| Constant | Value | Purpose |
| --- | --- | --- |
| `KERNEL_TIMER_HZ` | `100` | Timer rate, and the uptime divisor |
| `PHYSICAL_TIMER_INTID` | `30` | PPI passed to `gic_enable_ppi()` |
| `KERNEL_VMM_TEST_ADDRESS` | `0x1000000000` | Live mapping test address |
| `KERNEL_VMM_TEST_VALUE` | `0x41524D4F53564D4D` | Test pattern (`NXUVMM` in ASCII) |
| `KERNEL_VM_KERN_TEST_A_SIZE` | `2 * 4096 + 128` | Rounds up to three pages |
| `KERNEL_VM_KERN_TEST_B_SIZE` | `4096 + 256` | Rounds up to two pages |
| `KERNEL_HEAP_LARGE_TEST_SIZE` | `3 * 4096 + 333` | Rounds up to four pages |

`KERNEL_VMM_TEST_ADDRESS` equals `VM_KERN_BASE`. There is no conflict: the live
mapping test runs and fully unmaps before `vm_kern_init()` is called.

## Concurrency and interrupt context

`kern_init()` runs once, on the boot core, with IRQs masked for all but its
final loop. It must never be called from interrupt context, and nothing calls it
but `_start`.

The idle loop runs with IRQs unmasked and reads only `volatile` state written by
the handler.

## Current limitations

- No `panic()` helper; the halt-and-print pattern is repeated inline.
- No error propagation. Every failure is terminal.
- Self-tests are interleaved with bring-up rather than isolated.
- `platform_t` and `dtb_t` live on the boot stack with no global accessor.
- The initialization order is a hard-coded call list with no registration or
  dependency mechanism, so an ordering constraint exists only in this file and
  in the callees' precondition checks.
- The uptime divisor is `KERNEL_TIMER_HZ` rather than
  `timer_get_interrupt_rate()`; the two agree only because the same constant is
  passed to `timer_start_periodic()`.
- `kern_init()` never returns, and there is no shutdown or reboot path.

## Source files

- [`kern/kern_init.c`](../../kern/kern_init.c)
- [`mach/arm64/start.S`](../../mach/arm64/start.S)

## Related documentation

- [Initialization](../initialization.md)
- [Boot](../boot.md)
- [Architecture](../architecture.md)
- [Heap](heap.md)
- [Physical memory](../vm/physical-memory.md)
- [Virtual memory](../vm/virtual-memory.md)
- [Kernel virtual arena](../vm/kernel-virtual-arena.md)
- [Timer](../arm64/timer.md)
- [Exceptions](../arm64/exceptions.md)
