# i386 virtual memory

Classic two-level paging, 4 KiB pages, no PAE, no NX, one CPU. Constants live
in `kern/i386/vm_param.h`; the page-table code is `kern/i386/pmap.c`, the
`vm/vmm.h` and `vm/address_space.h` implementations are
`kern/i386/vmm_i386.c` and `kern/i386/address_space_i386.c`. The arm64-only
`vm/vmm*.c` and `vm/address_space.c` are not built for i386; `vm/pmm.c`,
`vm/vm_kern.c`, `vm/vm_shm.c`, `vm/vm_map.c`, `vm/user_copy.c` and
`kern/memory/heap.c` are shared as they are.

## Address map

| Virtual range | What |
|---|---|
| `0x00000000-0x00000FFF` | null guard, never mappable |
| `0x00001000-0x3FFFFFFF` | user image (ELF segments) |
| `0x40000000-0x5FFFFFFF` | `vm_map` window (anonymous mmap) |
| `0x60000000-0x6FFFFFFF` | `vm_shm` window |
| `0x70000000-0xBFFFEFFF` | free user space; `0xBFFFF000` is the top user page (`VM_USER_STACK_TOP`) |
| `0xC0000000-0xE7FFFFFF` | direct map of physical `[0, 640 MiB)` at `0xC0000000 + phys` (4 MiB pages) |
| `0xC0100000-__kernel_end` | kernel image, same direct map: `.boot`, `.text`, `.rodata` read-only, `.data`/`.bss` writable; a guard page under the boot stack is unmapped |
| `0xE8000000-0xF7FFFFFF` | `vm_kern` arena (`vm_kern_allocate`, 256 MiB: a 2x desktop's buffers, several app windows at their largest; taken from the direct map's top, which the 512 MiB VMs never used), page tables pre-allocated |
| `0xF8000000-0xFFFFFFFF` | MMIO window (`pmap_map_mmio`), uncached, page tables pre-allocated |

Physical: `[0, 1 MiB)` is left to firmware and the Multiboot structures (not
in the page allocator); the image is at `0x100000`; RAM above 640 MiB is
ignored. Physical addresses above the direct map (PCI BARs, APICs) are reached
with `pmap_map_mmio(phys, size, &va)`.

## Boot sequence

1. Multiboot loader jumps to `_start` (a physical address, section `.boot`)
   with paging off. `start.S` zeroes `.bss` through its physical alias, fills
   `i386_boot_pd` with 4 MiB pages (direct map at PDE 768..959 and an identity
   map of the first 4 MiB at PDE 0), sets `CR4.PSE`, loads CR3, sets
   `CR0.PG|WP`, and jumps to `i386_start_high` at `0xC01xxxxx`.
2. `i386_boot_relocate()` adds `0xC0000000` to the Multiboot command line,
   memory map and module addresses (so `i386_init.c` can dereference them),
   then clears PDE 0 and flushes the TLB. `i386_init()` runs.
3. `i386_gdt_init()` copies the live CR3 into the double-fault TSS.
4. Phase `vm` (`i386_init_vm`): `i386_memory_map_load()` into a local
   `platform_t`, loader data registered with `pmm_reserve_boot_range()`,
   `pmm_init()`, `vmm_init()` (`pmap_init()` refines the master directory in
   place: image tables, arena and MMIO tables, stack guard), layout and
   permission validation, `vm_kern_init()`.

The boot directory stays the master kernel directory for good. Every kernel
PDE is final after `pmap_init()`, so a new address space copies PDEs
768..1023 once and never needs fixing up.

## Contracts for other areas

* Kernel addresses of physical memory: `vmm_physical_to_higher_half()`, valid
  from the first instruction of C. `vmm_higher_half_direct_map_enabled()` is
  always true.
* User spaces: `vm_address_space_create/activate/deactivate/map_page/...`.
  Activation loads CR3; `deactivate` loads the master directory. Mapping
  changes to the active space use `invlpg`. Only page-aligned addresses in
  `[0x1000, 0xC0000000)` and pmm pages below 640 MiB can be mapped.
  `vm_address_space_release_pages()` drops one pmm reference per mapping and
  `vm_address_space_destroy()` frees the tables and directory (both i386
  additions declared in `vm/address_space.h`).
* No NX: `READ_ONLY` and `READ_EXECUTE` are both a read-only page; a software
  PTE bit (`PTE_SOFTWARE_EXEC`) preserves the difference for queries. Ring 0
  honours read-only pages (`CR0.WP`).
* Page faults: the strong `i386_trap_page_fault` (in `vm_fault.c`) describes
  kernel-mode faults (region, CR3, PDE/PTE) and returns false. A subsystem that
  wants to resolve faults defines the weak `i386_vm_fault_resolve()`.
* `heap_init()` may be called twice on i386 (the second call succeeds).

## Tests

`make test-i386-vm BUILD_ROOT=<scratch>` runs `tools/test_i386_vm.sh`: the
full self-test (`test=vm`), other RAM sizes (32 MiB, 1 GiB clipped to 640 MiB)
and the deliberate faults `vm-fault=write-ro|write-text|null|unmapped|
user-null|user-ro`, which must end in the `#PF` report with status 3. By hand:

    qemu-system-i386 -kernel <elf> -m 512M -display none -serial stdio \
      -monitor none -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
      -append "test=vm qemu-exit=1"

## Known gaps

No PAE/highmem, no global pages (every CR3 load flushes the kernel's TLB
entries), no NX, no demand paging, no SMP TLB shootdown, no ring-3 exercise of
the U/S bit yet (checked from ring 0 through page-table bits and `CR0.WP`).
