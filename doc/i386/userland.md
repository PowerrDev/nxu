# i386 userland

The ELF loader for 32-bit executables, the user-space runtime and ABI, the
non-UI programs built for i386, and the ext4 root disk image. Design context is
in `port.md`; this file is the userland area's contract and how-to.

## User ABI

| | |
|---|---|
| system call | `int $0x80` |
| number | `eax` (`NXU_SYS_*`) |
| arguments | `ebx, ecx, edx, esi, edi, ebp` (same order as arm64 `x0..x5`, six at most) |
| result | `eax`, signed 32-bit, negative is `-NXU_SYS_E_*` |
| preserved | every register except `eax`; the asm carries a `memory` clobber |
| process entry | ELF entry point, `esp = VM_USER_STACK_TOP` (`0xBFFFF000`), no arguments, GPRs zero |
| new thread | `eip = entry`, `esp = stack` exactly as passed to `nxu_thread_create`, `eax = arg`, other GPRs zero, nothing pushed |

`_start` (`frameworks/crt0_i386.S`) aligns the stack, calls `main()` (no
arguments, as on arm64) and exits with its return value (`NXU_SYS_EXIT`,
status in `ebx`).

`nxu_thread_spawn` (`frameworks/lib/thread_i386.c`) mmaps the stack, writes
`{real_entry, arg}` at its top (`top-8`, `top-4`), and starts the thread at the
asm `nxu_thread_trampoline` with `esp = top-8`. The trampoline reads the pair,
moves to a 16-byte-aligned cdecl frame, calls `entry(arg)`, and calls
`nxu_thread_exit(0)` if it returns.

### 64-bit arguments and results

The public API (`frameworks/include/nxu/syscall.h`) is unchanged, so its
`uint64_t`/`int64_t` types stay. The i386 wrappers
(`frameworks/lib/syscall_i386.c`) narrow at the boundary:

| what | decision |
|---|---|
| pointers | 32-bit values, passed as is |
| descriptors, pids, ids, flags, prot flags, addresses | must fit in 32 bits, else `-NXU_SYS_E_INVALID_ARGUMENT` (never silently truncated) |
| lengths, capacities, sizes | must fit in 31 bits (a larger result would read as an error), else `-NXU_SYS_E_INVALID_ARGUMENT` |
| `nxu_seek` offset | must fit in 32 bits: a 32-bit process cannot seek past 4 GiB - 1; larger is refused |
| `nxu_exit` / `nxu_thread_exit` status | low 32 bits |
| results | `eax` sign-extended |
| `nxu_mmap`, `nxu_shm_map` results | user addresses can exceed `INT32_MAX` (the user window runs to `0xBFFFFFFF`): zero-extended unless in the error range `-4095..-1` |
| `nxu_uptime_us` | the kernel returns the **low 32 bits** of the microsecond counter in `eax` (it wraps every 71.6 minutes and never fails; this is what natural truncation of the 64-bit value gives). The wrapper widens it to a monotonic 64-bit value by detecting wraps; it must be called at least once per wrap, which every caller (a poll loop) does |
| 64-bit data in memory | out-parameters and structs with 64-bit fields (`nxu_waitpid` status, `nxu_klog_read` cursor, `nxu_stat_t`, `nxu_dirent_t`, the `nxu_recovery_*` info structs) stay 64-bit objects; the kernel reads/writes all eight bytes little-endian, and layout is decided by the compiler from the shared header on both sides |

## Image layout

`makedefs/user-i386.ld` (`elf32-i386`, entry `_start`):

* one `PT_LOAD` R+X at `0x00400000`: `.text.start` first, then `.text`,
  `.rodata`;
* one `PT_LOAD` R+W on the next page boundary: `.data`, then `.bss` (NOLOAD, so
  `memsz > filesz`);
* no ELF/program headers inside a segment, so file offsets are page aligned and
  congruent with the virtual addresses; segments never share a page (W^X is
  per page);
* `.comment`, `.eh_frame`, `.note` discarded.

The whole image must stay in `[0x1000, VM_MAP_BASE)` = below `0x40000000`, the
window below the mmap area (`kern/i386/vm_param.h`). Don't `strip` the
binaries: the bss-only data segment's file offset lies beyond the end of the
code, and the loader (like the arm64 one) requires `offset + filesz` to lie
inside the file, which is only true because the symbol table follows.

## Loader (`kern/loader/`)

* `elf_format.h`: the pure part: on-disk layouts (`elf32_*`, `elf64_*`), the
  widened working types `loader_header_t` / `loader_segment_t`, and every check
  (header, program table bounds, per-segment alignment/range/W+X, file bounds).
  No VFS/VM dependency. `loader_image_inspect()` runs all of them over an
  in-memory image.
* `elf.c`: what `loader_spawn` does with a validated image (map, copy, stack,
  bootstrap-port handoff, start). It accepts **one** class per build: ELF64 /
  `EM_AARCH64` on arm64 (byte-for-byte the code it always was), ELF32 /
  `EM_386` on i386 (32-bit records are widened into the same 64-bit working
  structs, so the checks are shared). Where the i386 path calls the shared
  helper, the arm64 path keeps the original inline expression, because the
  arm64 kernel image is required to be byte-identical across this change.
* `kern/machine/cache.h`: dispatch header; arm64 forwards to
  `kern/arm64/cache.h`, i386 (`kern/i386/cache.h`) has coherent caches and
  no-op stubs with the same names (`cache_sync_instruction_range`, ...).
* `elf.c` is **not** in the i386 kernel's source list yet (it needs
  vfs/vm/proc/sched for i386); `make i386-loader-check` compiles it for i386
  with `-Werror` so it stays valid. The integrator adds
  `kern/loader/elf.c` to `I386_C_SOURCES` when those exist and implements
  `i386_init_userland` (spawn `/disk/System/Library/CoreServices/bootd`).
* `elf_selftest.c` defines the strong `i386_init_userland_selftest` (boot with
  `test=userland`): 31 hand-built ELF32 images (one valid, the rest each broken
  in one way: class, machine, type, endianness, header/program-header sizes,
  table and file bounds including 32-bit wraparound, null page, stack overlap,
  kernel space, misalignment, W+X, shared page, entry placement, ignored
  non-`PT_LOAD` records).

## Programs and disk image

Built by `makedefs/i386/userland.mk` into `$(BUILD_ROOT)/i386/user`, with
`--target=i386-none-elf`, the same flags as the arm64 userland, `-Werror`:
`bootd`, `logd`, `patchd`, `ipctest_a`, `ipctest_b`, `threadtest`,
`sockettest_server`, `sockettest_client`. No UI programs. No arm64 source
needed changing: they were already 32-bit clean. 64-bit division is provided by
`libk/udivmoddi4.c` in an archive (`libnxurt.a`), pulled in only if a program
divides 64-bit values (none does today).

The plists are not duplicated: `com.nxu.logd.plist` / `com.nxu.patchd.plist`
name `/disk/System/Library/CoreServices/<program>`, which is architecture
neutral, and bootd discovers `/disk/System/Library/BootDaemons/*.plist`. Only
those two are staged; the windowserver / about-sevos plists would just make
bootd retry a launch that cannot succeed.

Staged tree (`$(BUILD_ROOT)/i386/diskroot`, same shape as `tools/DiskRoot`,
which is only read): `hello.txt`, `System/README.txt`,
`System/Library/CoreServices/{bootd,bootd.recovery,logd,logd.recovery,patchd,
patchd.recovery,ipctest_a,ipctest_b,threadtest,sockettest_server,
sockettest_client}`, `System/Library/BootDaemons/*.plist`, `System/Recovery/`
(empty: there is no triageOS), `var/log/`, `var/db/patchd/`. The `.recovery`
copies are byte-identical, as bootd and patchd compare them.

## Building and testing

    export PATH="$HOME/.cargo/bin:/usr/local/opt/llvm/bin:/usr/local/opt/e2fsprogs/sbin:$PATH"
    make i386-userland BUILD_ROOT=/tmp/nxu     # programs
    make i386-disk BUILD_ROOT=/tmp/nxu         # /tmp/nxu/i386/disk.img (16M ext4)
    make i386-disk-check BUILD_ROOT=/tmp/nxu   # e2fsck -fn + ls CoreServices
    make test-i386-userland BUILD_ROOT=/tmp/nxu

The image uses the arm64 rule's `mkfs.ext4` options: `-b 4096 -I 256 -O
has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,
^fast_commit -E lazy_itable_init=0,lazy_journal_init=0 -L NXU`, 16M.
`I386_DISK_SIZE` overrides the size.

`test-i386-userland` builds the kernel, the programs and the image, compiles
`elf.c` for i386, runs `e2fsck`, `tools/check_i386_user.sh` (per program:
ELF32/EM_386/ET_EXEC, entry inside an executable `PT_LOAD`, no W+X, page
aligned and offset/vaddr congruent, inside `[0x1000, VM_MAP_BASE)`, no shared
page, file bounds; prints sizes) and `tools/test_i386_userland.sh` (boots
`test=userland`).

## Not done yet

* Launching bootd end to end: needs the loader linked into the i386 kernel and
  the int 0x80 path, vfs, vm and threads. The user code has been checked
  statically (ELF checks, disassembly of the wrappers, crt0 and trampoline),
  not executed.
* The kernel side of the ABI above (threads area), notably `NXU_SYS_UPTIME_US`
  returning the low 32 bits in `eax`, and syscalls that take user pointers to
  64-bit objects (`waitpid`, `klog_read`) reading/writing all eight bytes.
* `kern/kern_init.c` still includes `<kern/arm64/cache.h>` directly (arm64
  only, not mine to change).
