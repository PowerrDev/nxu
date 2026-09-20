# NXU
Power's ARM64 operational system.

# Source layout
The tree is split into subsystems, so that architecture-specific code, core
kernel code and machine discovery never bleed into one another.

```text
kern/arm64   ARM64-specific kernel code, including startup, exceptions,
             system registers, caches, GICv3 and the architectural timer.
kern         Core kernel, split by subsystem; no implementation files live at its root.
vfs          Virtual filesystem, vnodes, mounts, open files and ramfs.
vm           Physical and virtual memory management.
platform     Device Tree parsing, platform discovery, early console and
             machine-specific hardware descriptions.
libk         Freestanding C runtime and low-level kernel utility code.
user         Freestanding EL0 runtime and root daemons.
makedefs     Linker scripts and build definitions, including the test registry.
tools        Build inputs and host tooling: the DiskRoot image template, UI cursors,
             generated Recovery fonts, the test menu and font generator.
build        Object tree, mirroring the source tree.

kern/boot       Boot arguments and firmware-variable backends.
kern/console    Kernel console and boot-log presentation.
kern/loader     Static ELF userspace image loading.
kern/init       Ordered kernel bootstrap and machine handoff.
kern/interrupt  Machine-independent interrupt dispatch.
kern/ipc        Kernel IPC objects, ports and messages.
kern/logging    Kernel identity and logging infrastructure.
kern/memory     Core kernel allocation facilities.
kern/process    Processes, tasks and threads.
kern/sched      Processor state, run queues and scheduling policy.
kern/syscall    EL0 system-call ABI (syscall_defs.h, shared with userland) and dispatch.
kern/tests      Kernel subsystem self-tests.
```

Headers are included with a subsystem-qualified path, never a relative one:

```c
#include <kern/arm64/cache.h>
#include <kern/memory/heap.h>
#include <vm/pmm.h>
#include <platform/uart.h>
#include <string.h>
```

This works because the build uses two include roots: the repository root
resolves `<kern/...>`, `<vfs/...>`, `<vm/...>` and `<platform/...>`; and
`libk` resolves the freestanding runtime headers.

## Root userspace

`make userland` builds `bootd`, `logd`, `patchd`, and the recovery-only `triageOS`
as static AArch64 ELF64 programs. Disk image creation stages them under `/System/Library/CoreServices`;
the kernel loads `bootd` from the mounted system volume as PID 1. `bootd` then
discovers declarative XML service definitions from `/System/Library/BootDaemons`
instead of compiling daemon names into PID 1. See `doc/root-services.md` and
`doc/service-plists.md` for discovery, supervision, logging, and recovery.

Recovery UI is deliberately isolated from the main sevOS UI stack: Shift+R selects
`triageOS`, which links `Recovery.framework`/StartupOptionsUI only, while normal
sevOS continues to use the external UIService.framework. See `doc/recovery-ui.md`.

Development boot arguments are passed with `BOOT_ARGS`, for example
`make run-console BOOT_ARGS="-v -no-gpu"`. See `doc/boot-args.md` for the current
read-only Device Tree NVRAM backend and supported switches.

# UART
To print things in our kernel, we need to know about memory-mapped I/O.

On QEMU's ARM64 `virt` machine, the UART is a PL011 (a standard ARM serial chip used for data transfer) located at usually `0x09000000`.
**Note:** This address does not point to normal RAM. Reading and writing there communicates with hardware.

### Memory Mapped I/O
Normally, something like `value = *address;` means that it will read the value from memory, with memory-mapped hardware, the same operation can mean:
- read the UART status register;
- send a character;
- acknowledge an interrupt;
- configure a timer;

FYI, the address determines which hardware register you access, for the PL011 UART, the most important registers are:
```text
UART base + 0x00 = Data Register
UART base + 0x18 = Flag Register
```
So:
```text
0x09000000 = UARTDR
0x09000018 = UARTFR
```

# UART flag register 
Of course, before writing a character we must check whether the transmit FIFO is full (first-in, first-out).
The 5th bit of `UARTFR` is `TXFF`, and it's also the transmit FIFO full.

If bit 5 is 1, we must wait, something like:
```c
while (UARTFR & (1 << 5)) {

}
```

And finally, when the bits become 0, we can write character to UARTDR (UART data register)

## Why should we use `volatile`?
The type qualifier keyword `volatile` forces the CPU to read the actual memory address every single time the variable is accessed, rather than caching its value in a register.

So, hardware could (and can) change the UART status at any moment. This loop:
```c
while (UART_FR & UART_FR_TXFF) {}
```
must read `UART_FR`repeatdly; Without `volatile`, the compiler could theoretically turn it into:
```c
status = UART_FR;

while (status & UART_FR_TXFF) {
}
```

That would read the hardware only once and potentially loop forever. The type qualifier tells the compiler that every read and write is observable. Do not remove, cache, or merge these accesses.
**Note**: It does not make code thread-safe. It is mainly important here because we are communicating with hardware.

# ARM64 Exception Levels and `CurrentEL`
An ARM64 Exception Level, abbreviated EL, is a CPU privilege level.

It is not the same thing as a C++ or Java exception. In ARM terminology, an exception is an event such as:
- an interrupt
- a page fault
- an invalid instruction
- a system call
- a timer event

In total, we have four levels:
- `EL0`: Normally used for user applications, lowest privilege
- `EL1`: Used for operating system kernel, privileged.
- `EL2`: Used in the Hypervisor. More privileged.
- `EL3`: Used for secure monitor & firmware. Highest privilege.

EL0 and EL1 are mandatory architectural levels. EL2 and EL3 are optional, depending on whether virtualization and security extensions are implemented or exposed.

# How do I know the current level?
The register `CurrentEL` contains the Exception Level. FYI, it's not normal RAM, and it does not have a memory address. Therefore, we do not use `ldr` to read it.

For us, ARM64 provides `mrs` (Move Register from System Register):
```asm
mrs x0, CurrentEL
```
Conceptually, x0 = `CurrentEL` , furthermore, there is also a reverse instruction called `msr` (Move general-purpose register to System Register / Special Register)

# `CurrentEL` does not directly contain 1, 2, or 3
Remember this. CurrentEL is a 64-bit register, but the EL number is stored only in bits 3 and 2:
```plaintext
63                                      4 3   2 1   0
+----------------------------------------+-----+-----+
|                  zero                  | EL  |zero |
+----------------------------------------+-----+-----+
                                             ^
                                         bits [3:2]
```
The possible fields values are:
```plaintext
bits [3:2] = 00 -> EL0
bits [3:2] = 01 -> EL1
bits [3:2] = 10 -> EL2
bits [3:2] = 11 -> EL3
```
This means the raw register values look like:
| Level | Binary register value | Raw value |
| ----- | --------------------: | --------: |
| EL0   |                `0000` |     `0x0` |
| EL1   |                `0100` |     `0x4` |
| EL2   |                `1000` |     `0x8` |
| EL3   |                `1100` |     `0xC` |

For example, `EL1` is encoded like this:
```plaintext
CurrentEL = 0000 0100
                    ^^
                 bits 3:2
```
To extract the actual number, we shift it right by two bits:
```plaintext
0000 0100 >> 2
          =
0000 0001
```
So: `current_el = raw_current_el >> 2;`
Let's also apply a mask: `current_el = (raw_current_el >> 2) & 0x3;`

`0x3` is binary; therefore `0x3 = 0011`. This keeps only the lowest bits

# ARM64 Exceptions
Let's make our CPU execute a invalid instruction. Instead of silently freezing the kernel, it will log the exception so we know what's going on

The target output is something like:
```plaintext
panic(cpu 0 caller 0x0000000000000000): "Kernel trap type 1 -- Undefined Instruction"
Debugger message: panic
OS version: NXU 0.2.87-dev
Kernel version: NXU Kernel Version 0.2.87-dev (NXU-2I7A)

ARM Thread State (64-bit):
    x0: 0x0000000000000000       x1: 0x0000000000000000       x2: 0x0000000000000000
    x3: 0x0000000000000000       x4: 0x0000000000000000       x5: 0x0000000000000000
    x6: 0x0000000000000000       x7: 0x0000000000000000       x8: 0x0000000000000000
    x9: 0x0000000000000000      x10: 0x0000000000000000      x11: 0x0000000000000000
   x12: 0x0000000000000000      x13: 0x0000000000000000      x14: 0x0000000000000000
   x15: 0x0000000000000000      x16: 0x0000000000000000      x17: 0x0000000000000000
   x18: 0x0000000000000000      x19: 0x0000000000000000      x20: 0x0000000000000000
   x21: 0x0000000000000000      x22: 0x0000000000000000      x23: 0x0000000000000000
   x24: 0x0000000000000000      x25: 0x0000000000000000      x26: 0x0000000000000000
   x27: 0x0000000000000000      x28: 0x0000000000000000
    fp: 0x0000000000000000   lr: 0x0000000000000000
    sp: 0x0000000000000000   pc: 0x0000000000000000  cpsr: 0x00000000
   far: 0x0000000000000000  esr: 0x00000000

Kernel Extensions in backtrace: none
System uptime: unavailable
Kernel halted.
```

BTW, *your* exact address and `SPSR_EL1` value can be different.

## What even happens during an exception?
Let's suppose the CPU reaches `udf #0` - `udf` means permanently undefined instruction. It intentionally causes a synchronous exception.

The CPU will stop normal execution, record why the execution happened in ESR_EL1, record where it happened in ELR_EL1, save the old processor state in SPSR_EL1, look up the exception-vector base in VBAR_EL1, choose one entry in the vector table and finally start executing that entry.

`VBAR_EL1` contains the base address of the vector table used for exceptions taken to EL1. The vector table itself contains executable instructions, not function pointers, and the CPU selects a fixed offset based on the exception type and where it came from

## Synchronous VS asynchronous exceptions

Our invalid instruction causes a synchronous exception.

“Synchronous” means the exception is directly caused by the instruction currently executing. That includes:
- Undefined instruction
- Invalid memory access
- System call
- Breakpoint
- Alignment fault

Other exception types are asynchronous, such as:
- IRQ
- FIQ
- SError

For example, a timer interrupt is asynchronous because it can arrive between unrelated instructions.

An invalid instruction **is** synchronous because the CPU can identify the exact instruction that caused it.

## The mighty exception vector table

An AArch64 vector table contains 16 entries.
Each entry occupies `0x80`, or 128 bytes:
```plaintext
Entry 0  starts at VBAR + 0x000
Entry 1  starts at VBAR + 0x080
Entry 2  starts at VBAR + 0x100
...
Entry 15 starts at VBAR + 0x780
```

The whole table occupies 2048 bytes (16 entries x 128 bytes). Therefore, the base must be aligned to a 2048-byte boundary. ARM groups the entries by the originating Exception Level, selected stack pointer, execution state, and exception type.

## Registers the CPU fills in

ESR_EL1 - The Exception Syndrome Register explains why an exception was taken to EL1; Its important high layout is:
```plaintext
63             32 31       26 25 24                 0
+----------------+-----------+--+---------------------+
|      zero      |    EC     |IL|         ISS         |
+----------------+-----------+--+---------------------+
```

`EC` means Exception Class. It occupies bits [31:26]; EC identifies the broad reason (System call, undefined instruction, etc).
`IL` means instruction length. For an A64 instruction, it will normally indicate a 32-bit instruction.
`ISS`means Instruction Specific Syndrome.
It provides extra information whose interpretation depends on the exception class.
`ELR_EL1` is the Exception Link Register. For our undefined instruction, it will point at or identify the address of the instruction that caused the exception. AArch64 uses dedicated exception-link registers rather than relying on the normal function-call link register `x30`
`SPSR_EL1` stores the processor state from before the exception so when a real handler eventually executes `eret` the CPU will restore its execution state from `SPSR_EL1` and resume using `ELR_EL1`
`FAR_EL1` means Fault Address Register. It is especially useful for memory faults. For an undefined instruction, FAR_EL1 is not the important register, but we will print it because it will matter during page-fault work.



## Input drivers

NXU now includes a modern VirtIO-MMIO core with split virtqueues and VirtIO
Input drivers for the QEMU `virt` keyboard and relative mouse. `make run` adds
`virtio-keyboard-device` and `virtio-mouse-device`; their MMIO frames and GIC
interrupts are discovered through the boot Device Tree. See
`doc/drivers/virtio.md` and `doc/drivers/input.md`.


## VFS and ext4

NXU now has a filesystem-neutral VFS with per-process file descriptors,
resident vnodes and a global mount table. `ramfs` remains the bootstrap root,
while the development VirtIO Block disk is mounted as writable ext4 at
`/disk`.

`make run` keeps a persistent 64 MiB ext4 `disk.img`. A filesystem-format stamp
recreates an older lesson image once when the required on-disk profile changes;
after that, normal kernel rebuilds preserve guest data. Use `make disk-reset`
when a deliberately fresh filesystem is required.

The current ext4 path reads and mutates the primary superblock, group
descriptors, allocation bitmaps, inode tables, depth-zero extent roots and
classic directory entries. It supports regular-file create/write/truncate/
unlink and directory creation through VFS. Writable metadata is protected by
CRC32C `metadata_csum` checks and committed through an internal JBD2 journal.
The development targets include a deterministic commit-before-checkpoint crash
test and journal replay on the next boot. See `doc/vfs/ext4.md` and
`doc/vfs/jbd2.md`.
