# Boot

This document covers the path from QEMU loading the kernel image to the first
instruction of `kern_init()`. Everything after that point is described in
[Initialization](initialization.md).

## Image loading

NXU is booted as a raw binary. The `run` target passes
`-kernel build/kernel.bin` to `qemu-system-aarch64`. QEMU loads that image into
guest RAM and begins execution at its first byte.

NXU does not implement, and must not be described as supporting, any of the
following: UEFI, U-Boot, the Linux ARM64 boot protocol, PSCI-based secondary
CPU bring-up, or an ELF-aware bootloader. There is no image header, no magic
number and no boot-protocol handshake in the repository.

## Physical load address and higher-half VMA

QEMU loads `kernel.bin` at physical address `0x40080000`. The ELF itself is
linked at `0xFFFFFF8040080000`. `makedefs/linker.ld` expresses that split with
`AT()` so each section has a high virtual address (VMA) and the corresponding
low physical load address (LMA). The difference is always
`0xFFFFFF8000000000`.

The CPU initially executes the bytes at their physical alias because the MMU is
off. AArch64 PC-relative `ADR`/`ADRP`/branch references remain valid because the
VMA and LMA layouts differ by one constant offset. `_start` uses PC-relative
address formation for `.bss` and the bootstrap stack for this reason.

Once TTBR1 is built, the same image becomes reachable at its actual linked VMA.
The assembly transition moves SP and VBAR_EL1 to that alias and branches to the
high-linked continuation. TTBR0 can then be replaced by a user address space.

The linker exports normal higher-half section symbols (`__text_start`,
`__rodata_start`, etc.) and physical `__*_phys_*` symbols for ELF/tooling
inspection. Section boundaries remain 4 KiB aligned and the exception vectors
remain 2048-byte aligned.

## `kernel.elf` versus `kernel.bin`

The link step produces `build/kernel.elf`; `llvm-objcopy -O binary` then strips
it down to `build/kernel.bin`.

- `kernel.bin` is what QEMU boots. `llvm-objcopy` uses the ELF load addresses,
  so its first byte is the physical image byte for `0x40080000` even though the
  ELF section VMA is in the higher half. It carries no symbols or DWARF.
- `kernel.elf` is retained because it carries the symbol table and, because the
  build passes `-g`, full debug information. `make symbolize ADDR=...` runs
  `llvm-addr2line` against it, which is how a panic's `caller` address is
  translated back into a function and source line. It is also the file a
  debugger attaches to.

Both are produced by every build; see [Build system](build-system.md).

## Boot flow

```text
QEMU: load kernel.bin at 0x40080000, x0 = DTB physical address
     |
     v
_start (.text.boot)
     |
     +--> msr DAIFSet, #0xf        mask D, A, I and F
     |
     +--> msr SPSel, #1 ; isb      select SP_EL1, not SP_EL0
     |
     +--> clear [__bss_start, __bss_end) eight bytes at a time
     |         x0 is never touched
     |
     +--> dsb sy                   zeroing visible before C runs
     |
     +--> mov sp, stack_top        16 KiB stack, inside the cleared .bss
     |
     +--> bl kern_init             x0 = DTB address = first C argument
     |
     v
.Lhalt: wfe ; b .Lhalt            unreachable: kern_init never returns
```

## Register `x0`

On entry, `x0` holds the physical address of the Device Tree Blob that QEMU
constructed for the machine. This is the only piece of state the boot code
receives from the outside world, and preserving it is the reason the `.bss`
clearing loop uses `x1` and `x2` as its cursor and limit. When `_start` executes
`bl kern_init`, `x0` is still the DTB address, so it becomes the `dtb_address`
parameter of `kern_init()` under the standard AArch64 procedure call standard.

`kern_init()` immediately dumps the first four bytes at that address, which is
how the boot log shows `D0 0D FE ED` — the big-endian FDT magic — before
anything else is trusted.

## Interrupt masking

`msr DAIFSet, #0xf` sets all four masks: Debug, SError, IRQ and FIQ. At this
point `VBAR_EL1` still holds whatever QEMU left in it, so taking any exception
would jump somewhere undefined. The masks stay set through the entire
initialization sequence. Only `arm64_enable_irqs()`, at the end of
`kern_init()`, clears `PSTATE.I`; `PSTATE.D`, `PSTATE.A` and `PSTATE.F` are
never cleared, so NXU currently never takes a debug exception, an SError or
an FIQ.

## Stack pointer selection

`msr SPSel, #1` switches from `SP_EL0` to `SP_EL1`. AArch64 lets an exception
level use either the shared `SP_EL0` or its own banked stack pointer, and the
choice also determines which half of the vector table an exception from the
current EL uses. NXU selects `SP_EL1`, so exceptions taken while the kernel is
running use vector entries 4-7 rather than 0-3. The `isb` after the write
ensures the change has taken effect before `sp` is used.

`sp` itself is not written yet, deliberately: the stack lives in `.bss` and must
be cleared first.

## Clearing `.bss`

The loop walks from `__bss_start` to `__bss_end` storing `xzr` eight bytes at a
time. This is necessary because the raw binary contains no bytes for `.bss` —
the section is `(NOLOAD)` — so on entry that memory holds whatever QEMU left
there. C code assumes zero-initialized statics, and every NXU subsystem keeps
its state in a file-scope `static` structure, so skipping this step would leave
`g_pmm`, `g_vmm`, `g_heap` and the rest filled with garbage.

Eight bytes per iteration is safe without a tail loop because the linker script
aligns both `__bss_start` and `__bss_end` to 4096.

The `dsb sy` afterwards guarantees the zeroing stores have completed before any
C code observes those globals. The MMU and caches are still off at this point,
so these are uncached accesses to memory.

## Stack initialization

The stack is a 16 KiB `.skip` reservation in `.bss.stack`, aligned to 16 bytes
as the procedure call standard requires. `stack_top` is the label immediately
after the reservation, and AArch64 stacks grow downward, so `sp` is set to
`stack_top`.

There is one stack. There is no guard page below `stack_bottom`, and no
overflow detection: a deep enough call chain silently runs into whatever `.bss`
object was linked below it.

## Entry into C

`bl kern_init` is a normal call. `kern_init()` never returns, so the `wfe` loop
after it exists only as a defensive backstop.

At the moment `kern_init()` starts, the machine state is:

| State | Value |
| --- | --- |
| Exception level | EL1 |
| `DAIF` | All masked |
| `SPSel` | 1 (`SP_EL1`) |
| `sp` | `stack_top` |
| `x0` | DTB physical address |
| `.bss` | Zeroed |
| MMU (`SCTLR_EL1.M`) | Off |
| Caches (`SCTLR_EL1.C`/`.I`) | Off |
| `VBAR_EL1` | Not yet written by NXU |

Because the MMU is off, execution initially uses the physical alias. The VMM
first preserves that alias in TTBR0, then constructs the permanent high-linked
TTBR1 mapping and transitions the kernel to it before TTBR0 is handed to EL0.

## Source files

- [`kern/arm64/start.S`](../kern/arm64/start.S)
- [`makedefs/linker.ld`](../makedefs/linker.ld)
- [`kern/kern_init.c`](../kern/kern_init.c)
- [`Makefile`](../Makefile)

## Related documentation

- [Initialization](initialization.md)
- [Memory map](memory-map.md)
- [Build system](build-system.md)
- [Exceptions](arm64/exceptions.md)
- [Core kernel initialization](kern/initialization.md)

## Current limitations

- Only the QEMU raw-image boot path is supported. No firmware protocol is
  implemented.
- Secondary CPUs are not released by the boot path in `start.S`: `kern_start_scheduler()`
  starts them later with PSCI (see [SMP](kern/smp.md)). `-smp 1` still works.
- No stack guard page and no stack-overflow detection.
- The boot code cannot drop from EL2 or EL3. If the CPU were delivered at a
  higher exception level, NXU would run there and misbehave; `kern_init()`
  reports `CurrentEL` but does not act on it.
