# ARM64 Architecture Layer

`arch/arm64` contains the machine-dependent half of the NXU kernel.
Everything here would have to be rewritten for a different processor
architecture; nothing here should encode machine (as opposed to architecture)
knowledge.

## Responsibilities

- Establish the initial machine state at reset: exception masks, stack pointer
  selection, `.bss` clearing, and the transfer into C.
- Own the exception vector table and the low-level entry and exit paths around
  it, including the saved register frame.
- Own the interrupt controller: GICv3 distributor, redistributor and CPU
  system-register interface.
- Own the timekeeping source: the ARM generic physical timer.
- Own cache discovery, maintenance and enablement.
- Provide inline accessors for the system registers other subsystems need.

## Non-responsibilities

- **Machine addresses.** Where a device lives is a platform question, answered
  by [hardware discovery](../platform/hardware-discovery.md). The GICv3 driver
  currently violates this by hard-coding its MMIO bases; that is a known defect,
  not the intended boundary.
- **Translation tables.** Although AArch64-specific, page tables belong to
  `vm`, because the ownership rules around physical pages and virtual
  ranges are memory-management policy rather than architecture mechanism.
- **Initialization order.** `kern` decides when each architecture
  facility is brought up.
- **Allocation.** The architecture layer never calls `kmalloc()`; all of its
  state is statically allocated.

## Execution level

NXU executes exclusively at EL1 in AArch64 state. It is delivered to EL1 by
QEMU and contains no code to drop from EL2 or EL3, and no code to enter EL0.
`kern_init()` reads `CurrentEL` and prints the decoded level, but does not
branch on it.

The consequences that matter elsewhere:

- Every system register NXU touches is EL1-accessible. Nothing requires EL2 or
  EL3.
- Only vector entries 4-7 ("current EL with `SP_ELx`") can currently be taken.
  Entries 0-3, 8-11 and 12-15 exist and are wired to the same handler, but no
  execution path can reach them today.
- `TCR_EL1`, `SCTLR_EL1`, `MAIR_EL1` and `TTBR0_EL1` are the only translation
  control registers in play; there is no stage-2 translation and no `VTTBR_EL2`.

## Register conventions in early boot

`_start` runs before any C code, so it follows the AArch64 procedure call
standard only at the point where it calls into C:

| Register | Role in `_start` |
| --- | --- |
| `x0` | DTB physical address from QEMU; preserved untouched and passed as `kern_init()`'s first argument |
| `x1` | `.bss` clearing cursor, then the stack top |
| `x2` | `.bss` clearing limit |
| `xzr` | Zero source for the clearing loop |
| `sp` | Set to `stack_top` only after `.bss` has been cleared |

Inside the exception path, `x16` has a special role: the vector entry needs one
scratch register to record which of the 16 entries was taken, so it saves `x16`
to the frame first, uses it, and `exception_vector_common` restores it last.
`x16` and `x17` are the architectural intra-procedure-call scratch registers,
which makes `x16` the conventional choice.

The kernel is built with `-mgeneral-regs-only`, so no FP or SIMD register is
ever live and the exception frame does not need to save any.

## Facilities

| Facility | Files | Document |
| --- | --- | --- |
| Boot entry | `start.S` | [Boot](../boot.md) |
| Exception vectors and dispatch | `exception_vectors.S`, `exception.c`, `exception.h` | [Exceptions](exceptions.md) |
| GICv3 | `gic.c`, `gic.h` | [Interrupt controller](interrupt-controller.md) |
| Generic physical timer | `timer.c`, `timer.h` | [Timer](timer.md) |
| Cache control | `cache.c`, `cache.h` | [Cache](cache.md) |
| System-register accessors | `system.h` | [System registers](system-registers.md) |

`system.h` is header-only: every accessor is a `static inline` wrapper around a
single `mrs` or `msr`. It carries no state and has no corresponding `.c` file.

## Interaction with machine-independent code

Three interfaces cross the boundary in each direction.

Machine-independent code calls into the architecture layer:

```text
kern_init()  -> exception_init()          install VBAR_EL1
             -> cache_init()              enable I and D caches
             -> gic_init()                bring up GICv3
             -> gic_enable_ppi()           enable a private peripheral interrupt
             -> timer_start_periodic()    arm the generic timer
             -> arm64_enable_irqs()       clear PSTATE.I
             -> timer_get_interrupt_count()
```

The architecture layer calls out only for diagnostics and interrupt dispatch:

```text
exception_handle() -> timer_handle_interrupt()   architecture-internal
                   -> uart_puts() and friends    platform console
cache_dump()       -> uart_puts() and friends    platform console
```

The architecture layer never calls into `vm`. The dependency runs the
other way: `vm/vmm.c` contains its own inline `mrs`/`msr` accessors for
the translation control registers rather than routing them through `system.h`.

## Source files

- [`arch/arm64/start.S`](../../arch/arm64/start.S)
- [`arch/arm64/system.h`](../../arch/arm64/system.h)
- [`arch/arm64/exception.c`](../../arch/arm64/exception.c)
- [`arch/arm64/gic.c`](../../arch/arm64/gic.c)
- [`arch/arm64/timer.c`](../../arch/arm64/timer.c)
- [`arch/arm64/cache.c`](../../arch/arm64/cache.c)

## Related documentation

- [Architecture](../architecture.md)
- [Boot](../boot.md)
- [Exceptions](exceptions.md)
- [Interrupt controller](interrupt-controller.md)
- [Timer](timer.md)
- [Cache](cache.md)
- [System registers](system-registers.md)
- [Virtual memory](../vm/virtual-memory.md)

## Current limitations

- Single core throughout. No per-CPU data, no SGI/IPI support, no secondary CPU
  release.
- No FP/SIMD, no SVE, no pointer authentication, no MTE.
- `arch/arm64/gic.c` hard-codes its MMIO bases instead of accepting them
  from `platform_t`.
- Duplicate system-register accessors exist in `system.h`, `vmm.c`, `cache.c`,
  `gic.c` and `timer.c`. There is no single accessor header.
