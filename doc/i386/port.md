# i386 port: design, contracts and ownership

The goal is to run the whole NXU kernel on 32-bit x86 (i386) under
`qemu-system-i386 -M pc`: boot, memory management, interrupts, threads and the
scheduler, ring 3 userland with syscalls, virtio devices, the ext4 root
filesystem, and bootd/logd/patchd plus the test daemons. UI frameworks
(UIService, WindowServer, Recovery/triageOS, aqua, XAmethyst, the GUI test
daemons) are out of scope. The serial console is the display.

The arm64 kernel must keep building and behaving exactly as before. Shared
code stays shared; anything that differs by architecture goes behind a
`<mach/machine/...>` dispatch header (XNU's `<machine/...>` idea) or a
per-architecture source file, never behind `#ifdef __aarch64__` inside shared
logic unless there is no better seam.

## What already exists

* `mach/i386/`: Multiboot entry (`start.S`), serial console, TSC timer, GDT
  with a double-fault task gate, IDT, trap frame (`x86_saved_state32_t`), and
  a panic report. See `trap.h`, `gdt.h`, `idt.h`.
* `mach/machine/{machine_routines,timer,vm_param}.h`: dispatch headers.
* `mach/i386/boot_info.h`: the ordered boot **phases** (platform, interrupts,
  vm, kernel, threads, drivers, userland) as weak hooks in `i386_init.c`. Read
  the comment in that header; it is the integration contract.
* `mach/i386/trap.h`: weak **trap extension points** (`i386_trap_irq`,
  `i386_trap_syscall`, `i386_trap_user_exception`, `i386_trap_page_fault`,
  `i386_trap_exit`).
* `makedefs/i386.mk` and per-area fragments in `makedefs/i386/*.mk`.
* `make i386`, `make run-i386`, `make test-i386`.

## Target machine

`qemu-system-i386 -M pc -cpu qemu32` (or `pentium3`), 512 MiB RAM, Multiboot
via `-kernel`. Legacy PC platform: 8259 PIC, 8254 PIT, CMOS RTC, 16550 COM1,
PCI bus. Devices are **virtio over PCI** (`virtio-blk-pci`, `virtio-keyboard-pci`,
`virtio-mouse-pci`; use `-device ...,disable-modern=on` if you want the legacy
I/O-port transport, which is the simplest). No ACPI, no APIC required.

## Memory model (fixed; do not change without telling the others)

Classic 32-bit two-level paging, 4 KiB pages, **no PAE, no NX**. See
`mach/i386/vm_param.h` for the exact constants and the address map.

* Kernel is linked at virtual `0xC0100000`, loaded at physical `0x100000`
  (`VMM_HIGHER_HALF_BASE = 0xC0000000`). Boot code runs at physical addresses
  with paging off, builds a temporary mapping, enables paging, and jumps to the
  higher half.
* Direct map: physical `[0, VM_DIRECT_MAP_SIZE)` (768 MiB) appears at
  `0xC0000000 + phys`. RAM above 768 MiB is ignored (no highmem).
* `vm_kern` arena `0xF0000000..0xF3FFFFFF`; MMIO window `0xF8000000..`.
* User space `0x1000..0xBFFFFFFF`; the kernel half (PDEs 768..1023) is
  identical in every address space. Segments stay flat (base 0, limit 4 GiB),
  so a pointer is the same number in ring 0 and ring 3.
* `unsigned long` is 32 bits, `uint64_t` is `unsigned long long`. Use
  `(uintptr_t)` when converting between pointers and integers, and remember
  that 64-bit division needs libk's `__udivdi3` (already linked).

## Interfaces are the existing shared headers

Implement the machine-independent APIs that shared code already calls, with
the same signatures: `vm/vmm.h`, `vm/address_space.h`, `vm/pmm.h`,
`kern/irq/irq.h`, `mach/arm64/timer.h`-shaped `mach/i386/timer.h`,
`kern/process/thread.h`'s machine hooks, `kern/syscall/syscall.h`,
`drivers/virtio/*`, `platform/*.h`. If a shared header genuinely has to
change, change it **additively** (new function, new field, new dispatch
include) so the arm64 build is unaffected, and mention it in your report.

## Ownership (each area edits only its own files)

| Area | Owns |
|------|------|
| interrupts | `mach/i386/{pic,irq,timer}.*` (extend `timer.c`), `kern/irq/irq.c`, the strong `i386_trap_irq`, `makedefs/i386/interrupts.mk`, `doc/i386/interrupts.md` |
| vm | `mach/i386/{start.S,linker script,pmap*,memory_map*}`, `makedefs/linker-i386.ld`, `vm/*`, `kern/memory/heap.c`, `mach/i386/vm_param.h`, `makedefs/i386/vm.mk`, `doc/i386/vm.md` |
| devices | `platform/i386/*` (except `uart.c`), `platform/platform.h` (additive), `drivers/virtio/*`, `drivers/block/*`, `drivers/input/*`, `drivers/video/*` as needed, `makedefs/i386/devices.mk`, `doc/i386/devices.md` |
| threads | `mach/i386/{thread*,context_switch.S,transition*,syscall_trap*}`, `mach/machine/thread.h` dispatch, `kern/process/*`, `kern/sched_prism/*`, `kern/syscall/syscall.c`, `kern/lock.h`, the strong `i386_trap_syscall`/`i386_trap_user_exception`/`i386_trap_exit`, `makedefs/i386/threads.mk`, `doc/i386/threads.md` |
| userland | `kern/loader/*`, `mach/machine/cache.h` dispatch, `frameworks/lib/*`, `frameworks/crt0*`, `frameworks/CoreFoundation.framework/*`, `frameworks/BootDaemons.framework/*` (non-UI ones), `makedefs/user-i386.ld`, `makedefs/i386/userland.mk`, `tools/*` for building an i386 disk image, `doc/i386/userland.md` |

Nobody edits `Makefile`, `makedefs/i386.mk`, `mach/i386/i386_init.c`,
`mach/i386/trap.c` or `mach/i386/boot_info.h` in wave 1 (the integrator does).
Add sources, flags and targets through your own `makedefs/i386/<area>.mk`
fragment (`I386_C_SOURCES += ...`). A source listed twice is built once.
Supply your phase by defining the strong `i386_init_<phase>` and
`i386_init_<phase>_selftest` hooks in your own files.

## Build and test

* `make i386 BUILD_ROOT=<scratch dir>` builds `<scratch>/i386/kernel.elf` with
  `-Werror`. Always pass a scratch `BUILD_ROOT`; never build into the tree.
* `tools/test_i386.sh <kernel.elf>` runs the trap tests. Boot a single area's
  self-test with `-append "test=<phase> qemu-exit=1"`; `qemu-exit=1` ends QEMU
  with status 1 after the phases, a panic ends it with status 3.
* Add `tools/test_i386_<area>.sh` (bash 3.2 compatible, `perl -e 'alarm N'` for
  timeouts, no `timeout` command on macOS) and a `test-i386-<area>` target in
  your fragment that runs your area's cases. `-device isa-debug-exit,iobase=0xf4,iosize=0x04`
  is how the guest ends QEMU.
* **Regression gate:** the arm64 kernel must still build, and shared files you
  touched must leave it byte-identical unless you deliberately changed arm64
  behaviour. Check with
  `make -j8 all BUILD_ROOT=<scratch> CONFIG=base` and
  `shasum -a 256 <scratch>/base/kernel.bin`; the reference hash for the base
  commit is recorded in your task prompt.
* Environment: `export PATH="$HOME/.cargo/bin:/usr/local/opt/llvm/bin:/usr/local/opt/e2fsprogs/sbin:$PATH"`.
  The shell is zsh, which does not word-split unquoted variables: loop over
  file lists in a `bash` script. macOS ships make 3.81 and bash 3.2 (no
  associative arrays, no `$(let)`/`$(file)`).
* Do not modify `disk.img` or anything under `tools/DiskRoot` (arm64 artifacts).

## Style

Match the surrounding code: tabs, `type\nname(...)` or `static type name(...)`
as the neighbours do, `kprintf` log lines prefixed with the emitting function
name, no field widths in `kprintf` (unsupported). Commit in the repo's XNU
style: `subsystem/path: imperative summary`, a body explaining what and why,
several focused commits, ending with
`Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>`.
