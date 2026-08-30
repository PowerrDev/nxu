# Higher-Half Kernel

NXU is linked at its permanent TTBR1 virtual address while QEMU still loads a
raw image at the physical address expected by the `virt` machine.

## VMA and LMA

The linker defines three central constants:

```text
KERNEL_PHYS_BASE   = 0x0000000040080000
KERNEL_VIRT_OFFSET = 0xFFFFFF8000000000
KERNEL_VIRT_BASE   = 0xFFFFFF8040080000
```

Every loadable section has a higher-half virtual memory address (VMA) and a low
physical load memory address (LMA):

```text
.text VMA  0xFFFFFF8040080000  -> LMA 0x40080000
.rodata    VMA + offset        -> matching physical bytes
.data      VMA + offset        -> matching physical bytes
.bss       VMA + offset        -> matching physical reservation
```

`llvm-objcopy -O binary` emits the LMA image, so `kernel.bin` remains a small raw
file suitable for QEMU even though `kernel.elf` is symbolized in the higher half.

## Bootstrap alias

The MMU is disabled when QEMU enters `_start`, therefore the CPU initially
executes `.text` through its physical alias at `0x40080000`.

AArch64 PC-relative instructions preserve the section-relative distance when the
whole image is shifted by one constant. `_start` therefore uses `ADR`/`ADRP` and
normal relative branches rather than loading higher-half absolute addresses.
This lets the high-linked image execute long enough to discover RAM, build
translation tables and enable the MMU.

Static initialized pointers are different: the linker writes their actual VMA
into the image. This is intentional. They must become valid after the TTBR1
transition, and it prevents stale `0x4008...` pointers from surviving once TTBR0
belongs to userspace.

## TTBR1 mapping

`vmm_init_higher_half_alias()` builds a second translation-table hierarchy and
maps the kernel's linked VMA to its physical pages with section permissions:

```text
.text    r-x
.rodata  r--
.data    rw-
.bss     rw-
```

`vmm_map_higher_half_direct_map()` extends the same TTBR1 regime to RAM and MMIO
using:

```text
virtual = VMM_HIGHER_HALF_BASE | physical
```

For the kernel image this direct-map formula is deliberately the same address as
the linked VMA.

## Transition

After TTBR1 is valid, `arm64_enter_higher_half()`:

1. masks exceptions;
2. adds `VMM_HIGHER_HALF_BASE` to SP_EL1;
3. installs the higher-half VBAR_EL1;
4. branches to the higher-half `kern_init_higher_half()` continuation.

The continuation validates PC, SP, VBAR_EL1 and all linked section translations.
It also loads a static string-pointer table and a static function-pointer table
and verifies that both contain higher-half addresses. This is a regression test
for the exact class of low-pointer failure that previously broke panic/proc
printing.

## Dynamic physical pointers

Higher-half linking fixes linker-created pointers, but it does not transform
runtime values that originate as physical addresses. Translation tables, the PMM
bitmap, DTB interior pointers and MMIO bases are discovered/allocated at runtime.
Those objects are explicitly moved to their TTBR1 direct-map aliases before the
old TTBR0 kernel mapping is removed.

This is not relocation of the ELF. It is conversion of runtime physical pointers
to permanent kernel virtual pointers.

## TTBR0 handoff

Once all long-lived runtime pointers use TTBR1, `vmm_disable_ttbr0()` removes the
bootstrap kernel root. PID 1 then installs its own address-space root in TTBR0.
The kernel stays mapped through TTBR1 across that switch and across future
process switches.

## Symbolization

Panic PCs are now already ELF VMAs. Use them directly:

```bash
make symbolize ADDR=0xFFFFFF8040081234
```

No manual subtraction of `0xFFFFFF8000000000` is needed.

## Source files

- `makedefs/linker.ld`
- `arch/arm64/start.S`
- `arch/arm64/transition.S`
- `vm/vmm.c`
- `vm/vmm_ttbr1.c`
- `vm/vmm_debug.c`
- `kern/kern_init.c`


## Pre-MMU relocation rule

NXU links normal kernel symbols at their permanent TTBR1 virtual addresses,
but `_start` and the first part of `kern_init()` execute through the physical
load alias while the MMU is still disabled. Direct AArch64 `BL`, `ADR` and
`ADRP`/`ADD` references remain valid during this phase because the physical and
virtual layouts differ by one constant offset.

Stored absolute pointers are different. A compiler-generated `.rodata`
aggregate containing a function or string pointer contains the final higher-half
VMA. Such a pointer cannot be dereferenced or called before TTBR1 is live.
Early-boot callback structures therefore materialize their pointer fields at
runtime instead of using constant aggregate initializers. Once
`kern_enter_higher_half()` completes, normal static pointer tables are safe and
intentionally contain higher-half addresses.
