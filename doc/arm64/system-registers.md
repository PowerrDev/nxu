# System Register Reference

Every AArch64 system register accessed by NXU, where it is accessed, and what
NXU uses it for. Registers that are not touched by the current source are not
listed, except in the closing "not yet used" section, which is explicitly marked
as future work.

All of these are accessed at EL1. None requires EL2 or EL3.

## Identification and state

### `CurrentEL`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `arm_read_current_el_raw()`, [`system.h`](../../arch/arm64/system.h) |
| Caller | `kern_init()` |
| Subsystem | Core kernel |
| Privilege | EL1 and above |

Holds the current exception level in bits [3:2]; all other bits are zero. The
raw values are `0x0` (EL0), `0x4` (EL1), `0x8` (EL2), `0xC` (EL3).
`arm_read_current_el()` returns `(raw >> 2) & 0x3`.

NXU prints both forms and takes no action based on the result.

### `ID_AA64MMFR0_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `arm64_read_mmfr0()`, [`vmm.c`](../../vm/vmm.c) |
| Caller | `vmm_init()` |
| Subsystem | Virtual memory |

Memory model feature register 0. Two fields are used:

| Field | Bits | NXU use |
| --- | --- | --- |
| `PARange` | [3:0] | Physical address width; stored as `g_vmm.ips` and written to `TCR_EL1.IPS` |
| `TGran4` | [31:28] | 4 KiB granule support; `0xF` means unsupported and fails `vmm_init()` |

`PARange` is decoded through a lookup table to a bit width:

| Encoding | Width |
| --- | --- |
| 0 | 32 |
| 1 | 36 |
| 2 | 40 |
| 3 | 42 |
| 4 | 44 |
| 5 | 48 |

Any encoding of 6 or above is rejected. The observed QEMU `cortex-a72` reports
encoding 4, giving 44 bits.

### `ID_AA64MMFR2_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `cache_read_mmfr2()`, [`cache.c`](../../arch/arm64/cache.c) |
| Caller | `cache_uses_legacy_ccsidr()` |
| Subsystem | Cache |

Only `CCIDX` (bits [23:20]) is used, to determine which `CCSIDR_EL1` layout the
CPU implements. Non-zero fails cache initialization. See [Cache](cache.md).

## Translation control

### `SCTLR_EL1`

| Property | Value |
| --- | --- |
| Access | Read and write |
| Locations | `vmm_enable()`, [`vmm.c`](../../vm/vmm.c); `vmm_dump()`, [`vmm_debug.c`](../../vm/vmm_debug.c); `cache_read_sctlr()`, `cache_init()`, [`cache.c`](../../arch/arm64/cache.c) |
| Subsystem | Virtual memory, cache |

The EL1 system control register. Four bits are used:

| Bit | Name | Written by | Meaning |
| --- | --- | --- | --- |
| 0 | `M` | `vmm_enable()` — set | Stage-1 MMU enable |
| 2 | `C` | `cache_init()` — set | Data and unified cache enable |
| 12 | `I` | `cache_init()` — set | Instruction cache enable |
| 19 | `WXN` | `vmm_enable()` — set | Write permission implies execute-never |

`WXN` is a hardware-level W^X guarantee: with it set, any region writable at
EL1 is treated as non-executable regardless of what its descriptor says. It
means a mistake that produces a writable-executable descriptor cannot become an
exploitable one.

Both writers perform read-modify-write and each is followed by `isb`.

### `TCR_EL1`

| Property | Value |
| --- | --- |
| Access | Read and write |
| Locations | `vmm_enable()`, `vmm_read_tcr()`, [`vmm.c`](../../vm/vmm.c); `vmm_dump()`, [`vmm_debug.c`](../../vm/vmm_debug.c) |
| Subsystem | Virtual memory |

Translation control. The value NXU builds:

| Field | Bits | Value | Meaning |
| --- | --- | --- | --- |
| `T0SZ` | [5:0] | 25 | `TTBR0_EL1` covers 64 − 25 = 39 bits |
| `IRGN0` | [9:8] | `01` | Inner write-back write-allocate for table walks |
| `ORGN0` | [11:10] | `01` | Outer write-back write-allocate for table walks |
| `SH0` | [13:12] | `11` | Inner shareable table walks |
| `TG0` | [15:14] | `00` | 4 KiB granule for `TTBR0_EL1` |
| `T1SZ` | [21:16] | 25 | Set for symmetry; unused while `EPD1` is set |
| `EPD1` | 23 | 1 | **Disable `TTBR1_EL1` translation entirely** |
| `IRGN1` | [25:24] | `01` | As `IRGN0`, for `TTBR1_EL1` |
| `ORGN1` | [27:26] | `01` | As `ORGN0`, for `TTBR1_EL1` |
| `SH1` | [29:28] | `11` | As `SH0`, for `TTBR1_EL1` |
| `TG1` | [31:30] | `10` | 4 KiB granule for `TTBR1_EL1` |
| `IPS` | [34:32] | from `PARange` | Intermediate physical address size |

Note that `TG1`'s 4 KiB encoding is `0b10`, not `0b00` as for `TG0`. The two
fields use different encodings and the source reflects that.

`EPD1 = 1` is what makes NXU a single-address-space kernel today: the entire
upper half faults. Clearing it is the first step of the `TTBR1_EL1` migration
described in [Roadmap to userland](../roadmap-to-userland.md).

### `TTBR0_EL1`

| Property | Value |
| --- | --- |
| Access | Read and write |
| Locations | `vmm_enable()`, `vmm_read_ttbr0()`, [`vmm.c`](../../vm/vmm.c); `vmm_dump()`, [`vmm_debug.c`](../../vm/vmm_debug.c) |
| Subsystem | Virtual memory |

Translation table base for the lower virtual address range. NXU writes the
physical address of the L1 root table, allocated from the PMM. Because the
address space is identity-mapped, that value is also a valid kernel pointer.

`TTBR1_EL1` holds the permanent higher-half kernel root; `TTBR0_EL1` is used for bootstrap and then task-owned EL0 address spaces.

### `MAIR_EL1`

| Property | Value |
| --- | --- |
| Access | Read and write |
| Locations | `vmm_enable()`, `vmm_read_mair()`, [`vmm.c`](../../vm/vmm.c); `vmm_dump()`, [`vmm_debug.c`](../../vm/vmm_debug.c) |
| Subsystem | Virtual memory |

Eight 8-bit attribute slots indexed by a descriptor's `AttrIndx` field. NXU
programs two:

| Index | Value | Meaning |
| --- | --- | --- |
| 0 | `0xFF` | Normal, inner and outer write-back, read/write-allocate |
| 1 | `0x04` | Device-nGnRE |

The resulting register value is `0x00000000000004FF`. See [Cache](cache.md) for
why the distinction matters.

### `PAR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `vmm_translate_access()`, [`vmm_tables.c`](../../vm/vmm_tables.c) |
| Callers | `vmm_translate()`, `vmm_translate_write()`, `vmm_validate_kernel_permissions()` |
| Subsystem | Virtual memory |

Physical Address Register: the result of the most recent `AT` instruction.

| Field | Meaning |
| --- | --- |
| Bit 0 (`F`) | 1 = the translation faulted; 0 = success |
| Bits [47:12] | Physical address of the containing page, on success |

NXU treats bit 0 as the sole success indicator and reconstructs the full
address as `(PAR_EL1 & 0x0000FFFFFFFFF000) | (va & 0xFFF)`. It does not decode
the fault status bits on failure — a fault is reported to the caller as `false`
with no further detail.

`AT S1E1R` and `AT S1E1W` are the instructions that produce it; they perform a
stage-1 EL1 translation for a read or a write respectively, honouring access
permissions, without actually performing the access. This is what lets
`vmm_validate_kernel_permissions()` prove that `.text` is not writable without
risking a fault.

## Exception state

### `VBAR_EL1`

| Property | Value |
| --- | --- |
| Access | Write |
| Location | `arm64_write_vbar_el1()`, [`system.h`](../../arch/arm64/system.h) |
| Caller | `exception_init()` |
| Subsystem | Exceptions |

Base address of the EL1 exception vector table. Bits [10:0] are reserved, so the
table must be 2048-byte aligned; the linker script enforces this. The write is
followed by `isb`.

### `ELR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Locations | `exception_vector_common` ([`exception_vectors.S`](../../arch/arm64/exception_vectors.S)), `arm64_read_elr_el1()` ([`system.h`](../../arch/arm64/system.h)) |
| Subsystem | Exceptions |

Exception Link Register: the address execution resumes at when `eret` runs. For
a synchronous fault this identifies the faulting instruction. NXU saves it
into the exception frame at offset 256 and prints it as `pc` and as the panic
`caller`.

Written by hardware on exception entry; NXU never writes it, so `eret` always
resumes exactly where the exception occurred.

### `SPSR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Locations | `exception_vector_common`, `arm64_read_spsr_el1()` |
| Subsystem | Exceptions |

Saved Program Status Register: the `PSTATE` value from before the exception,
including the condition flags, the `DAIF` masks, the exception level and the
stack pointer selection. `eret` restores `PSTATE` from it. NXU saves it at
frame offset 264 and prints its low 32 bits as `cpsr`.

### `ESR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Locations | `exception_vector_common`, `arm64_read_esr_el1()` |
| Subsystem | Exceptions |

Exception Syndrome Register.

| Field | Bits | NXU accessor |
| --- | --- | --- |
| `EC` — exception class | [31:26] | `exception_class()` |
| `IL` — instruction length | 25 | `exception_instruction_length()` |
| `ISS` — instruction specific syndrome | [24:0] | `exception_iss()` |

Saved at frame offset 280. See [Exceptions](exceptions.md) for the exception
classes NXU names.

### `FAR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Locations | `exception_vector_common`, `arm64_read_far_el1()` |
| Subsystem | Exceptions |

Fault Address Register: the faulting virtual address for aborts and alignment
faults. Saved at frame offset 272 and printed unconditionally, although it is
only meaningful for the exception classes that define it.

## Cache identification

### `CLIDR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `cache_read_clidr()`, [`cache.c`](../../arch/arm64/cache.c) |
| Subsystem | Cache |

Cache Level ID Register. NXU reads the 3-bit `Ctype` field for each of up to 7
levels to decide which levels need data-cache invalidation. Cached into
`g_cache.clidr` and printed by `cache_dump()`.

### `CCSIDR_EL1`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `cache_read_ccsidr()`, [`cache.c`](../../arch/arm64/cache.c) |
| Subsystem | Cache |

Describes the cache currently selected by `CSSELR_EL1`. NXU decodes the legacy
layout only:

| Field | Bits | Use |
| --- | --- | --- |
| `LineSize` | [2:0] | `line_shift = LineSize + 4` for the set/way operand |
| `Associativity` | [12:3] | Ways minus one; also drives `way_shift = CLZ(ways-1)` |
| `NumSets` | [27:13] | Sets minus one |

Never cached; read fresh after each `CSSELR_EL1` selection.

### `CSSELR_EL1`

| Property | Value |
| --- | --- |
| Access | Write |
| Location | `cache_write_csselr()`, [`cache.c`](../../arch/arm64/cache.c) |
| Subsystem | Cache |

Cache Size Selection Register: chooses what `CCSIDR_EL1` reports.

| Field | Bits | NXU value |
| --- | --- | --- |
| `InD` | 0 | 0 — data or unified cache |
| `Level` | [3:1] | The level being invalidated |

Restored to 0 when the invalidation walk completes. Each write is followed by
`isb`.

### `CTR_EL0`

| Property | Value |
| --- | --- |
| Access | Read |
| Location | `cache_read_ctr()`, [`cache.c`](../../arch/arm64/cache.c) |
| Subsystem | Cache |

Cache Type Register. Readable at EL0, which is why it carries the `_EL0` suffix.

| Field | Bits | Use |
| --- | --- | --- |
| `IminLine` | [3:0] | Minimum instruction cache line, as log2(words) |
| `DminLine` | [19:16] | Minimum data/unified cache line, as log2(words) |

Both are converted to bytes as `4 << encoding`. Cached into `g_cache.ctr`.

## Interrupt controller (GICv3 CPU interface)

All `ICC_*_EL1` registers require `ICC_SRE_EL1.SRE` to be set before they are
architecturally accessible.

| Register | Access | Location | Value / meaning |
| --- | --- | --- | --- |
| `ICC_SRE_EL1` | Read, write | `gic_read_sre()`, `gic_write_sre()` | Bit 0 `SRE` set: enable the system-register interface |
| `ICC_CTLR_EL1` | Read, write | `gic_read_control()`, `gic_write_control()` | Bit 1 `EOImode` cleared: EOI performs drop and deactivate |
| `ICC_PMR_EL1` | Write | `gic_cpu_interface_init()` | `0xFF`: priority mask threshold admits every interrupt |
| `ICC_BPR1_EL1` | Write | `gic_cpu_interface_init()` | `0`: no priority grouping |
| `ICC_IGRPEN1_EL1` | Write | `gic_cpu_interface_init()` | `1`: enable Group 1 signaling to this CPU |
| `ICC_IAR1_EL1` | Read | `gic_acknowledge_interrupt()` | Acknowledges and returns the pending INTID (low 24 bits) |
| `ICC_EOIR1_EL1` | Write | `gic_end_interrupt()` | Signals completion of the given INTID |

All in [`gic.c`](../../arch/arm64/gic.c). Reading `ICC_IAR1_EL1` has the
side effect of activating the interrupt; it is not a passive query. See
[Interrupt controller](interrupt-controller.md).

## Generic timer

All in [`timer.c`](../../arch/arm64/timer.c).

| Register | Access | Location | Meaning |
| --- | --- | --- | --- |
| `CNTFRQ_EL0` | Read | `arm64_read_counter_frequency()` | System counter increment rate in Hz |
| `CNTPCT_EL0` | Read | `arm64_read_physical_counter()` | Current physical counter value; preceded by `isb` |
| `CNTP_TVAL_EL0` | Write | `arm64_write_physical_timer_value()` | Countdown value; sets the comparison point |
| `CNTP_CTL_EL0` | Write | `arm64_write_physical_timer_control()` | Bit 0 `ENABLE`, bit 1 `IMASK`; written as `1` |

See [Timer](timer.md).

## `PSTATE` fields

These are special-purpose fields written with `msr` immediate forms rather than
conventional system registers.

| Field | Operation | Location | Purpose |
| --- | --- | --- | --- |
| `DAIF` | `msr DAIFSet, #0xf` | [`start.S`](../../arch/arm64/start.S) | Mask Debug, SError, IRQ and FIQ at reset |
| `DAIF` | `msr DAIFClr, #2` | `arm64_enable_irqs()`, [`gic.c`](../../arch/arm64/gic.c) | Clear `PSTATE.I` — unmask IRQs |
| `DAIF` | `msr DAIFSet, #2` | `arm64_disable_irqs()`, [`gic.c`](../../arch/arm64/gic.c) | Set `PSTATE.I` — mask IRQs |
| `SPSel` | `msr SPSel, #1` | [`start.S`](../../arch/arm64/start.S) | Select `SP_EL1` rather than `SP_EL0` |

The `#2` operand is a bitmask over the `DAIF` fields in which bit 1 is `I`.
`PSTATE.D`, `PSTATE.A` and `PSTATE.F` are set at boot and never cleared, so
NXU never takes a debug exception, an SError or an FIQ.

## Not yet used

The following are relevant to planned work and are deliberately **not** accessed
by any current source file. They are listed so the register table above can be
read as exhaustive.

| Register | Why it will be needed |
| --- | --- |
| `TTBR1_EL1` | Higher-half kernel mappings |
| `TCR_EL1.EPD1` (clearing it) | Enabling `TTBR1_EL1` translation |
| `CNTP_CVAL_EL0` | Absolute timer deadlines without drift |
| `TPIDR_EL1` / `TPIDR_EL0` | Per-CPU or per-thread pointers |
| `MPIDR_EL1` | CPU identification under SMP |
| `SP_EL0` | A separate user stack pointer |
| `ICC_SGI1R_EL1` | Inter-processor interrupts |
| `CPACR_EL1` | Enabling FP/SIMD access, currently unnecessary |

See [Roadmap to userland](../roadmap-to-userland.md).

## Source files

- [`arch/arm64/system.h`](../../arch/arm64/system.h)
- [`arch/arm64/cache.c`](../../arch/arm64/cache.c)
- [`arch/arm64/gic.c`](../../arch/arm64/gic.c)
- [`arch/arm64/timer.c`](../../arch/arm64/timer.c)
- [`arch/arm64/start.S`](../../arch/arm64/start.S)
- [`arch/arm64/exception_vectors.S`](../../arch/arm64/exception_vectors.S)
- [`vm/vmm.c`](../../vm/vmm.c)
- [`vm/vmm_tables.c`](../../vm/vmm_tables.c)
- [`vm/vmm_debug.c`](../../vm/vmm_debug.c)

## Related documentation

- [ARM64 overview](overview.md)
- [Exceptions](exceptions.md)
- [Interrupt controller](interrupt-controller.md)
- [Timer](timer.md)
- [Cache](cache.md)
- [Virtual memory](../vm/virtual-memory.md)
- [Translation tables](../vm/translation-tables.md)

## Current limitations

- System-register accessors are duplicated across `system.h`, `vmm.c`,
  `cache.c`, `gic.c` and `timer.c` rather than centralized.
- `PAR_EL1` fault status is discarded; a failed `AT` yields only `false`.
- No feature registers beyond `ID_AA64MMFR0_EL1` and `ID_AA64MMFR2_EL1` are
  consulted, so NXU makes no other capability decisions at run time.
